/* Host-side tests: firmware/Core/Src/telemetry.c (parser, OLED view, SD logging). */
#include "fatfs.h"
#include "ssd1306.h"
#include "stm32f4xx_hal.h"
#include "test_util.h"

/* Pull the module into this translation unit so its static state can be reset. */
#include "../Core/Src/telemetry.c"

static void setup(void)
{
    stub_reset();
    memset(&g_telemetry, 0, sizeof(g_telemetry));
    memset(&g_stats, 0, sizeof(g_stats));
    g_have_previous = 0;
    rx_index = 0;
    rx_discard = 0;
    is_mounted = 0;
    mount_attempted = 0;
    last_mount_attempt_ms = 0;
}

/* Run the main-loop body until the simulated UART is empty. */
static void pump(void)
{
    while (stub_uart_pending() > 0) {
        Telemetry_ReceiveAndParse();
    }
}

static TelemetryLineStatus_t parse(const char *text, TelemetryData_t *out)
{
    char line[256];
    strncpy(line, text, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    return Telemetry_ParseLine(line, out);
}

static void test_parse_valid(void)
{
    TelemetryData_t d = {0};
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12", &d), TELEMETRY_LINE_OK);
    CHECK_INT(d.timestamp_ms, 114805);
    CHECK_INT(d.hours, 0);
    CHECK_INT(d.minutes, 1);
    CHECK_INT(d.seconds, 54);
    CHECK(fabsf(d.altitude - 536.49f) < 1e-3f);
    CHECK(fabsf(d.speed - 1.25f) < 1e-6f);
    CHECK(fabsf(d.voltage - 24.12f) < 1e-4f);
    CHECK_INT(parse("114805, 0, 1, 54, -3.5, .5, +24\r", &d), TELEMETRY_LINE_OK);
    CHECK(d.altitude == -3.5f && d.speed == 0.5f && d.voltage == 24.0f);
    CHECK_INT(parse("4294967295,0,0,0,0,0,0", &d), TELEMETRY_LINE_OK);
}

static void test_parse_rejects_malformed(void)
{
    static const char *const bad[] = {
        "",
        "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage",
        "536.49,0.0,24.122",                      /* legacy 3-column line */
        "114805,0,1,54,536.49,1.25",              /* too few fields       */
        "114805,0,1,54,536.49,1.25,24.12,7",      /* too many fields      */
        "114805,0,1,54,536.49,1.25,24.12x",       /* trailing junk        */
        "114805,0,1,54,536.49,,24.12",            /* empty field          */
        "-5,0,1,54,536.49,1.25,24.12",            /* negative time        */
        "1.5,0,1,54,536.49,1.25,24.12",           /* fractional time      */
        "99999999999,0,1,54,536.49,1.25,24.12",   /* > uint32             */
        "114805,0,60,54,536.49,1.25,24.12",       /* minutes out of range */
        "114805,0,1,60,536.49,1.25,24.12",        /* seconds out of range */
        "114805,0,1,54,nan,1.25,24.12",
        "114805,0,1,54,536.49,inf,24.12",
        "114805,0,1,54,536.49,1.25,1e99",         /* overflows a float    */
    };
    TelemetryData_t d = {0};
    d.altitude = 42.0f;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (parse(bad[i], &d) != TELEMETRY_LINE_MALFORMED) {
            printf("  accepted or misclassified: \"%s\"\n", bad[i]);
            test_failures++;
        }
        test_checks++;
    }
    CHECK(d.altitude == 42.0f); /* output untouched on failure */
}

static void test_parse_checksum(void)
{
    TelemetryData_t d = {0};
    /* XOR of "114805,0,1,54,536.49,1.25,24.12" is 0x29 (same value as the Python tests). */
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*29", &d), TELEMETRY_LINE_OK);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*29\r", &d), TELEMETRY_LINE_OK);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*2A", &d), TELEMETRY_LINE_BAD_CHECKSUM);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.13*29", &d), TELEMETRY_LINE_BAD_CHECKSUM);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*2", &d), TELEMETRY_LINE_BAD_CHECKSUM);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*", &d), TELEMETRY_LINE_BAD_CHECKSUM);
    CHECK_INT(parse("114805,0,1,54,536.49,1.25,24.12*299", &d), TELEMETRY_LINE_BAD_CHECKSUM);
}

static void test_stream_display_and_log(void)
{
    setup();
    Telemetry_Init();
    CHECK_STR(stub_oled[4], "LOGGING: OK");
    CHECK_STR(stub_disk.data, "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n");

    stub_uart_push("TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n"
                   "114805,0,1,54,536.49,0.00,24.12\r\n"
                   "\n"
                   "115805,0,1,55,537.63,0.50,24.02\n");
    pump();

    CHECK_INT(g_stats.lines_ok, 2);
    CHECK_INT(g_stats.lines_rejected, 1); /* the header */
    CHECK_INT(g_stats.log_errors, 0);
    CHECK_STR(stub_oled[0], "T:00:01:55");
    CHECK_STR(stub_oled[1], "ALT:537.63(+1.14)");
    CHECK_STR(stub_oled[2], "SPD:0.50(+0.50)");
    CHECK_STR(stub_oled[3], "V:24.02(-0.100)");
    CHECK_STR(stub_oled[4], "LOGGING: OK");
    CHECK_STR(stub_disk.data, "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n"
                              "114805,0,1,54,536.49,0.00,24.12\n"
                              "115805,0,1,55,537.63,0.50,24.02\n");
    CHECK_INT(stub_disk.open_files, 0);
}

static void test_first_record_and_time_reversal_have_zero_rates(void)
{
    setup();
    Telemetry_Init();
    stub_uart_push("114805,0,1,54,536.49,3.00,24.12\n");
    pump();
    CHECK_STR(stub_oled[1], "ALT:536.49(+0.00)"); /* original code showed 536.49 / 114.8 s */
    stub_uart_push("100000,0,1,40,500.00,3.00,24.12\n"); /* log restarted */
    pump();
    CHECK_STR(stub_oled[1], "ALT:500.00(+0.00)");
    stub_uart_push("100000,0,1,40,510.00,3.00,24.12\n"); /* duplicate timestamp */
    pump();
    CHECK_STR(stub_oled[1], "ALT:510.00(+0.00)");
}

static void test_overlong_line_is_dropped_whole(void)
{
    char line[400];
    setup();
    Telemetry_Init();
    /* 200 characters whose first 127 would parse as a valid record if truncated. */
    memset(line, '0', sizeof(line));
    memcpy(line, "1,0,0,1,1.0,2.0,3.", 18);
    line[200] = '\n';
    line[201] = '\0';
    stub_uart_push(line);
    stub_uart_push("2000,0,0,2,5.00,6.00,7.00\n");
    pump();
    CHECK_INT(g_stats.lines_ok, 1);
    CHECK_INT(g_stats.lines_rejected, 1);
    CHECK_INT(g_telemetry.timestamp_ms, 2000);
}

static void test_extreme_values_do_not_overflow_buffers(void)
{
    setup();
    Telemetry_Init();
    stub_uart_push("1000,0,0,1,3e38,3e38,3e38\n");
    stub_uart_push("1001,0,0,1,-3e38,-3e38,-3e38\n"); /* rate = -6e41 -> -inf */
    pump();                                           /* ASan/UBSan would abort here */
    CHECK_INT(g_stats.lines_ok, 2);
    CHECK_INT(g_stats.log_errors, 2);                 /* too long for one log record: skipped */
    CHECK_INT(strlen(stub_oled[1]), 31);              /* truncated to the row buffer */
    CHECK_STR(stub_disk.data, "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n");
}

static void test_bad_checksum_line_is_ignored(void)
{
    setup();
    Telemetry_Init();
    stub_uart_push("114805,0,1,54,536.49,1.25,24.12*29\n"
                   "114905,0,1,54,936.49,1.25,24.12*29\n");
    pump();
    CHECK_INT(g_stats.lines_ok, 1);
    CHECK_INT(g_stats.lines_rejected, 1);
    CHECK(fabsf(g_telemetry.altitude - 536.49f) < 1e-3f);
}

static void test_uart_overrun_discards_damaged_line(void)
{
    setup();
    Telemetry_Init();
    stub_uart_push("1000,0,0,1,1.00,");
    pump();
    stub_uart_set_overrun();     /* bytes were lost after the ',' */
    stub_uart_push("3.00\n2000,0,0,2,5.00,6.00,7.00\n");
    pump();
    CHECK_INT(g_stats.lines_ok, 1);
    CHECK_INT(g_stats.lines_rejected, 1);
    CHECK_INT(g_telemetry.timestamp_ms, 2000);
}

static void test_sd_missing_backoff_and_recovery(void)
{
    setup();
    stub_disk.mount_result = FR_NOT_READY;
    Telemetry_Init();
    CHECK_STR(stub_oled[4], "LOGGING: FAIL");
    CHECK_INT(stub_disk.mount_calls, 1);

    stub_uart_push("1000,0,0,1,1.00,2.00,3.00\n1200,0,0,1,1.00,2.00,3.00\n");
    pump();
    CHECK_INT(stub_disk.mount_calls, 1); /* no remount storm while data is flowing */
    CHECK_INT(g_stats.lines_ok, 2);
    CHECK_STR(stub_oled[4], "LOGGING: FAIL");

    stub_disk.mount_result = FR_OK;      /* card inserted */
    stub_advance_ms(TELEMETRY_SD_RETRY_MS);
    stub_uart_push("7000,0,0,7,1.00,2.00,3.00\n");
    pump();
    CHECK_INT(stub_disk.mount_calls, 2);
    stub_uart_push("7200,0,0,7,1.50,2.00,3.00\n");
    pump();
    CHECK_STR(stub_oled[4], "LOGGING: OK");
    CHECK_STR(stub_disk.data, "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n7200,0,0,7,1.50,2.00,3.00\n");
}

static void test_write_failure_is_reported(void)
{
    setup();
    Telemetry_Init();
    stub_disk.write_result = FR_DISK_ERR;
    stub_uart_push("1000,0,0,1,1.00,2.00,3.00\n");
    pump();
    CHECK_INT(g_stats.log_errors, 1);
    stub_uart_push("1200,0,0,1,1.00,2.00,3.00\n");
    pump();
    CHECK_STR(stub_oled[4], "LOGGING: FAIL"); /* original code kept showing OK */
    CHECK_INT(stub_disk.open_files, 0);

    setup();
    Telemetry_Init();
    stub_disk.short_write = 1;               /* disk full */
    stub_uart_push("1000,0,0,1,1.00,2.00,3.00\n");
    pump();
    CHECK_INT(g_stats.log_errors, 1);
    CHECK_INT(is_mounted, 0);
}

int main(void)
{
    RUN(test_parse_valid);
    RUN(test_parse_rejects_malformed);
    RUN(test_parse_checksum);
    RUN(test_stream_display_and_log);
    RUN(test_first_record_and_time_reversal_have_zero_rates);
    RUN(test_overlong_line_is_dropped_whole);
    RUN(test_extreme_values_do_not_overflow_buffers);
    RUN(test_bad_checksum_line_is_ignored);
    RUN(test_uart_overrun_discards_damaged_line);
    RUN(test_sd_missing_backoff_and_recovery);
    RUN(test_write_failure_is_reported);
    return TEST_SUMMARY();
}
