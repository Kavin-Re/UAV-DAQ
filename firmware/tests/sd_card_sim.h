/* Behavioural model of an SDHC card in SPI mode, for host-side driver tests. */
#ifndef SD_CARD_SIM_H
#define SD_CARD_SIM_H

#include <stdint.h>

#define SD_SIM_SECTORS 64U

typedef struct {
    int present;          /* 0: empty socket, MISO pulled up                     */
    int stuck_busy;       /* 1: card holds MISO low forever                      */
    int acmd41_retries;   /* ACMD41 attempts answered "still idle" before ready  */
    int busy_bytes_after_write;
    uint8_t mem[SD_SIM_SECTORS * 512U];

    /* Observations */
    int violations;       /* command while busy, junk tokens, noise on MOSI ...  */
    int commands;
    int blocks_read;
    int blocks_written;
    int initialised;
} SdCardSim;

extern SdCardSim sd_sim;

void sd_sim_reset(void);
void sd_sim_set_cs(int low);
uint8_t sd_sim_xfer(uint8_t mosi);

#endif
