/* Host-side tests: firmware/drivers/user_diskio.c against a simulated SDHC card. */
#include "sd_card_sim.h"
#include "stm32f4xx_hal.h"
#include "test_util.h"

/* Pull the driver into this translation unit so its static state can be reset. */
#include "../drivers/user_diskio.c"

static BYTE pattern[4 * 512];
static BYTE readback[4 * 512];

static void setup(void)
{
    stub_reset();
    Stat = STA_NOINIT;
    CardType = 0;
    for (unsigned i = 0; i < sizeof(pattern); i++) {
        pattern[i] = (BYTE)(i * 7U + (i >> 8));
    }
    memset(readback, 0, sizeof(readback));
}

static void test_init_sdhc(void)
{
    setup();
    CHECK_INT(USER_initialize(0), 0);
    CHECK_INT(USER_status(0), 0);
    CHECK(sd_sim.initialised);
    CHECK_INT(CardType & 4, 4); /* block-addressed (SDHC) */
    CHECK_INT(hspi1.Instance->CR1 & SPI_CR1_BR_Msk, SPI_BAUDRATEPRESCALER_2);
    CHECK_INT(sd_sim.violations, 0);
    CHECK_INT(USER_initialize(1), STA_NOINIT); /* only drive 0 exists */
}

static void test_no_card_fails_fast(void)
{
    setup();
    sd_sim.present = 0;
    CHECK_INT(USER_initialize(0), STA_NOINIT);
    CHECK(HAL_GetTick() < 2000U);
    CHECK_INT(USER_read(0, readback, 0, 1), RES_NOTRDY);
    CHECK_INT(USER_write(0, pattern, 0, 1), RES_NOTRDY);
}

static void test_stuck_card_times_out(void)
{
    setup();
    sd_sim.stuck_busy = 1;
    CHECK_INT(USER_initialize(0), STA_NOINIT);
    CHECK(HAL_GetTick() < 3000U); /* original driver: 5 s per command */
}

static void test_single_block_read(void)
{
    setup();
    memcpy(&sd_sim.mem[5 * 512], pattern, 512);
    USER_initialize(0);
    CHECK_INT(USER_read(0, readback, 5, 1), RES_OK);
    CHECK(memcmp(readback, pattern, 512) == 0);
    CHECK_INT(sd_sim.violations, 0);
}

static void test_single_block_write_then_read(void)
{
    setup();
    USER_initialize(0);
    CHECK_INT(USER_write(0, pattern, 9, 1), RES_OK);
    CHECK(memcmp(&sd_sim.mem[9 * 512], pattern, 512) == 0);
    /* The card is still busy programming; the next command must wait for it. */
    CHECK_INT(USER_read(0, readback, 9, 1), RES_OK);
    CHECK(memcmp(readback, pattern, 512) == 0);
    CHECK_INT(sd_sim.violations, 0);
}

static void test_back_to_back_writes(void)
{
    setup();
    USER_initialize(0);
    for (DWORD sector = 0; sector < 8; sector++) {
        CHECK_INT(USER_write(0, pattern + 512 * (sector % 4), sector, 1), RES_OK);
    }
    CHECK_INT(sd_sim.blocks_written, 8);
    CHECK(memcmp(&sd_sim.mem[7 * 512], pattern + 3 * 512, 512) == 0);
    CHECK_INT(sd_sim.violations, 0);
}

static void test_multi_block_write_and_read(void)
{
    setup();
    USER_initialize(0);
    CHECK_INT(USER_write(0, pattern, 20, 4), RES_OK);
    CHECK_INT(sd_sim.blocks_written, 4);
    CHECK(memcmp(&sd_sim.mem[20 * 512], pattern, sizeof(pattern)) == 0);
    CHECK_INT(USER_read(0, readback, 20, 4), RES_OK);
    CHECK(memcmp(readback, pattern, sizeof(pattern)) == 0);
    /* and the card accepts a new command afterwards */
    CHECK_INT(USER_read(0, readback, 21, 1), RES_OK);
    CHECK(memcmp(readback, pattern + 512, 512) == 0);
    CHECK_INT(sd_sim.violations, 0);
}

static void test_errors_are_reported(void)
{
    setup();
    USER_initialize(0);
    CHECK_INT(USER_read(0, readback, SD_SIM_SECTORS + 1, 1), RES_ERROR); /* address out of range */
    CHECK_INT(USER_write(0, pattern, SD_SIM_SECTORS + 1, 1), RES_ERROR);
    CHECK_INT(USER_read(0, readback, 0, 0), RES_PARERR);
    CHECK_INT(USER_read(1, readback, 0, 1), RES_NOTRDY);
}

static void test_ioctl(void)
{
    WORD sector_size = 0;
    setup();
    USER_initialize(0);
    USER_write(0, pattern, 1, 1);
    CHECK_INT(USER_ioctl(0, CTRL_SYNC, 0), RES_OK);
    CHECK_INT(USER_ioctl(0, GET_SECTOR_SIZE, &sector_size), RES_OK);
    CHECK_INT(sector_size, 512);
    CHECK_INT(USER_ioctl(0, 99, 0), RES_PARERR);
    CHECK_INT(sd_sim.violations, 0);
}

static void test_reinit_failure_clears_ready_state(void)
{
    setup();
    USER_initialize(0);
    sd_sim.present = 0; /* card pulled out */
    CHECK_INT(USER_initialize(0), STA_NOINIT);
    CHECK_INT(USER_read(0, readback, 0, 1), RES_NOTRDY);
}

int main(void)
{
    RUN(test_init_sdhc);
    RUN(test_no_card_fails_fast);
    RUN(test_stuck_card_times_out);
    RUN(test_single_block_read);
    RUN(test_single_block_write_then_read);
    RUN(test_back_to_back_writes);
    RUN(test_multi_block_write_and_read);
    RUN(test_errors_are_reported);
    RUN(test_ioctl);
    RUN(test_reinit_failure_clears_ready_state);
    return TEST_SUMMARY();
}
