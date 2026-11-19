/* Behavioural model of an SDHC card in SPI mode (see sd_card_sim.h).
 *
 * It is deliberately strict about the things the original driver got wrong:
 *  - it ignores everything while CS is high and only drives MISO while selected,
 *  - it stays busy (MISO low) after a write and rejects commands sent meanwhile,
 *  - a multi-block read is aborted if CS is released between blocks,
 *  - data must follow every 0xFC / 0xFE token.
 */
#include "sd_card_sim.h"

#include <string.h>

SdCardSim sd_sim;

enum { ST_CMD, ST_READ_SINGLE, ST_READ_MULTI, ST_WRITE_TOKEN, ST_WRITE_DATA };

static struct {
    int cs_low;
    int state;
    int idle;
    int app_cmd;
    int acmd41_left;
    int multi;
    int busy;
    uint32_t addr;
    uint8_t cmd[6];
    int cmd_len;
    uint8_t out[1024];
    int out_head, out_len;
    uint8_t block[514];
    int block_len;
} c;

static void out_push(uint8_t b)
{
    if (c.out_len < (int)sizeof(c.out)) {
        c.out[(c.out_head + c.out_len++) % (int)sizeof(c.out)] = b;
    }
}

static void queue_r1(uint8_t r1)
{
    out_push(0xFF); /* Ncr: one byte before the response */
    out_push(r1);
}

static void queue_block(void)
{
    out_push(0xFF);
    out_push(0xFF);
    out_push(0xFE);
    for (int i = 0; i < 512; i++) {
        out_push(sd_sim.mem[c.addr * 512U + (uint32_t)i]);
    }
    out_push(0xAA); /* CRC, not checked by the driver */
    out_push(0x55);
    sd_sim.blocks_read++;
}

void sd_sim_reset(void)
{
    memset(&c, 0, sizeof(c));
    memset(&sd_sim, 0, sizeof(sd_sim));
    sd_sim.present = 1;
    sd_sim.acmd41_retries = 3;
    sd_sim.busy_bytes_after_write = 40;
    c.idle = 1;
}

void sd_sim_set_cs(int low)
{
    if (c.cs_low && !low) {
        /* De-selecting aborts whatever was in progress. */
        if (c.state == ST_READ_MULTI || (c.state == ST_READ_SINGLE && c.out_len > 0) ||
            c.state == ST_WRITE_DATA) {
            sd_sim.violations++;
        }
        c.state = ST_CMD;
        c.cmd_len = 0;
        c.out_len = 0;
    }
    c.cs_low = low;
}

static void exec_command(void)
{
    uint8_t cmd = c.cmd[0] & 0x3F;
    uint32_t arg = ((uint32_t)c.cmd[1] << 24) | ((uint32_t)c.cmd[2] << 16) | ((uint32_t)c.cmd[3] << 8) | c.cmd[4];
    int app = c.app_cmd;

    sd_sim.commands++;
    c.app_cmd = 0;
    c.out_len = 0;

    switch (cmd) {
    case 0:
        c.idle = 1;
        c.acmd41_left = sd_sim.acmd41_retries;
        sd_sim.initialised = 0;
        queue_r1(c.cmd[5] == 0x95 ? 0x01 : 0x09);
        break;
    case 8:
        queue_r1(c.cmd[5] == 0x87 ? 0x01 : 0x09);
        out_push(0x00);
        out_push(0x00);
        out_push(c.cmd[3] & 0x0F);
        out_push(c.cmd[4]);
        break;
    case 55:
        c.app_cmd = 1;
        queue_r1(c.idle ? 0x01 : 0x00);
        break;
    case 41:
        if (!app) {
            queue_r1(0x04);
            break;
        }
        if ((arg & (1UL << 30)) && c.acmd41_left-- <= 0) {
            c.idle = 0;
            sd_sim.initialised = 1;
        }
        queue_r1(c.idle ? 0x01 : 0x00);
        break;
    case 58:
        queue_r1(c.idle ? 0x01 : 0x00);
        out_push(c.idle ? 0x00 : 0xC0); /* power-up done + CCS (block addressing) */
        out_push(0xFF);
        out_push(0x80);
        out_push(0x00);
        break;
    case 12:
        queue_r1(0x00);
        c.busy = 4;
        c.state = ST_CMD;
        break;
    case 17:
    case 18:
    case 24:
    case 25:
        if (c.idle) {
            queue_r1(0x05);
        } else if (arg >= SD_SIM_SECTORS) {
            queue_r1(0x40);
        } else {
            c.addr = arg;
            queue_r1(0x00);
            if (cmd == 17 || cmd == 18) {
                c.state = (cmd == 17) ? ST_READ_SINGLE : ST_READ_MULTI;
                queue_block();
            } else {
                c.state = ST_WRITE_TOKEN;
                c.multi = (cmd == 25);
            }
        }
        break;
    default:
        queue_r1(0x04);
        break;
    }
}

static void feed_command_byte(uint8_t mosi)
{
    if (c.cmd_len == 0) {
        if ((mosi & 0xC0) != 0x40) {
            if (mosi != 0xFF) {
                sd_sim.violations++;
            }
            return;
        }
        if (c.busy > 0) {
            sd_sim.violations++; /* command while the card is busy: ignored */
            return;
        }
    }
    c.cmd[c.cmd_len++] = mosi;
    if (c.cmd_len == 6) {
        c.cmd_len = 0;
        exec_command();
    }
}

uint8_t sd_sim_xfer(uint8_t mosi)
{
    uint8_t miso;

    if (!sd_sim.present || !c.cs_low) {
        return 0xFF;
    }
    if (sd_sim.stuck_busy) {
        return 0x00;
    }

    /* MISO for this byte is decided before MOSI is interpreted. */
    if (c.out_len > 0) {
        miso = c.out[c.out_head];
        c.out_head = (c.out_head + 1) % (int)sizeof(c.out);
        c.out_len--;
    } else if (c.busy > 0) {
        miso = 0x00;
        c.busy--;
    } else {
        miso = 0xFF;
    }

    switch (c.state) {
    case ST_READ_SINGLE:
        if (c.out_len == 0) {
            c.state = ST_CMD;
        }
        if (mosi != 0xFF) {
            sd_sim.violations++; /* host must keep MOSI high during a read */
        }
        break;
    case ST_READ_MULTI:
        if (c.cmd_len > 0 || mosi == (0x40 | 12)) {
            c.cmd[c.cmd_len++] = mosi;
            if (c.cmd_len == 6) {
                c.cmd_len = 0;
                exec_command();
            }
        } else {
            if (mosi != 0xFF) {
                sd_sim.violations++;
            }
            if (c.out_len == 0) {
                if (++c.addr < SD_SIM_SECTORS) {
                    queue_block();
                }
            }
        }
        break;
    case ST_WRITE_TOKEN:
        if (c.busy > 0 && mosi != 0xFF) {
            sd_sim.violations++; /* token sent while still programming */
        } else if (mosi == 0xFE && !c.multi) {
            c.state = ST_WRITE_DATA;
            c.block_len = 0;
        } else if (mosi == 0xFC && c.multi) {
            c.state = ST_WRITE_DATA;
            c.block_len = 0;
        } else if (mosi == 0xFD && c.multi) {
            out_push(0xFF);
            c.busy = sd_sim.busy_bytes_after_write;
            c.state = ST_CMD;
        } else if (mosi != 0xFF) {
            sd_sim.violations++;
        }
        break;
    case ST_WRITE_DATA:
        c.block[c.block_len++] = mosi;
        if (c.block_len == 514) {
            if (c.addr < SD_SIM_SECTORS) {
                memcpy(&sd_sim.mem[c.addr * 512U], c.block, 512);
                sd_sim.blocks_written++;
                out_push(0xE5); /* data accepted */
            } else {
                out_push(0xED); /* write error */
            }
            c.busy = sd_sim.busy_bytes_after_write;
            if (c.multi) {
                c.addr++;
                c.state = ST_WRITE_TOKEN;
            } else {
                c.state = ST_CMD;
            }
        }
        break;
    default:
        feed_command_byte(mosi);
        break;
    }
    return miso;
}
