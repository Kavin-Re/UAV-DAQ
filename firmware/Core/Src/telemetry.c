/**
  ******************************************************************************
  * @file    telemetry.c
  * @brief   Telemetry line decoder, OLED view and SD logger (application layer).
  *
  * Moved out of main.c so that (a) STM32CubeMX can regenerate main.c without
  * touching application code and (b) the logic can be unit-tested on a PC
  * (firmware/tests). The data path is unchanged:
  *
  *     USART1 (polled, 1 byte) -> line buffer -> parse -> OLED -> SD append
  *
  * NOTE: every change relative to the original main.c is listed in
  * docs/FIRMWARE_AUDIT.md and still NEEDS HARDWARE RE-TEST.
  ******************************************************************************
  */
#include "telemetry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "fatfs.h"
#include "ssd1306.h"
#include "ssd1306_fonts.h"

#define LOG_FILE_NAME "telemetry.csv"
#define LOG_HEADER    "TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage\n"

extern UART_HandleTypeDef huart1;

static TelemetryData_t  g_telemetry;
static TelemetryStats_t g_stats;
static uint8_t  g_have_previous;      /* at least one record accepted          */
static uint8_t  rx_buffer[TELEMETRY_RX_BUFFER_SIZE];
static uint32_t rx_index;
static uint8_t  rx_discard;           /* drop bytes until the next '\n'        */
static FATFS    fs;
static FIL      fil;
static uint8_t  is_mounted;           /* volume mounted and last write was OK  */
static uint8_t  mount_attempted;
static uint32_t last_mount_attempt_ms;

/* ---------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------- */

static const char *skip_blanks(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') {
        p++;
    }
    return p;
}

/* Parse an unsigned decimal field followed by ','. Returns NULL on error. */
static const char *parse_u32_field(const char *p, uint32_t *value)
{
    char *end;
    unsigned long v;

    p = skip_blanks(p);
    if (*p < '0' || *p > '9') {
        return NULL;                  /* also rejects '-' and '+'              */
    }
    v = strtoul(p, &end, 10);
    if (v > 0xFFFFFFFFUL) {
        return NULL;
    }
    end = (char *)skip_blanks(end);
    if (*end != ',') {
        return NULL;
    }
    *value = (uint32_t)v;
    return end + 1;
}

/* Parse a float field followed by @p terminator (',' or '\0'). */
static const char *parse_float_field(const char *p, float *value, char terminator)
{
    char *end;
    float v;

    p = skip_blanks(p);
    if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.')) {
        return NULL;                  /* rejects "nan", "inf", empty fields    */
    }
    v = strtof(p, &end);
    if (end == p || !isfinite(v)) {
        return NULL;
    }
    end = (char *)skip_blanks(end);
    if (*end != terminator) {
        return NULL;
    }
    *value = v;
    return (terminator == '\0') ? end : end + 1;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

TelemetryLineStatus_t Telemetry_ParseLine(char *line, TelemetryData_t *out)
{
    uint32_t time_ms, h, m, s;
    float alt, spd, volt;
    const char *p;
    char *star = strrchr(line, '*');

    if (star != NULL) {
        /* Optional NMEA-style checksum: XOR of everything before '*'. */
        uint8_t sum = 0;
        const char *q;
        const char *tail = star + 1;
        int hi, lo;

        hi = hex_nibble(tail[0]);
        lo = (hi < 0) ? -1 : hex_nibble(tail[1]);
        if (lo < 0 || *skip_blanks(tail + 2) != '\0') {
            return TELEMETRY_LINE_BAD_CHECKSUM;
        }
        for (q = line; q < star; q++) {
            sum ^= (uint8_t)*q;
        }
        if (sum != (uint8_t)((hi << 4) | lo)) {
            return TELEMETRY_LINE_BAD_CHECKSUM;
        }
        *star = '\0';
    }

    /* --- CSV FORMAT: TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage --- */
    p = line;
    if ((p = parse_u32_field(p, &time_ms)) == NULL ||
        (p = parse_u32_field(p, &h)) == NULL ||
        (p = parse_u32_field(p, &m)) == NULL ||
        (p = parse_u32_field(p, &s)) == NULL ||
        (p = parse_float_field(p, &alt, ',')) == NULL ||
        (p = parse_float_field(p, &spd, ',')) == NULL ||
        (p = parse_float_field(p, &volt, '\0')) == NULL) {
        return TELEMETRY_LINE_MALFORMED;
    }
    if (m > 59U || s > 59U) {
        return TELEMETRY_LINE_MALFORMED;
    }

    out->timestamp_ms = time_ms;
    out->hours = h;
    out->minutes = m;
    out->seconds = s;
    out->altitude = alt;
    out->speed = spd;
    out->voltage = volt;
    return TELEMETRY_LINE_OK;
}

static void Telemetry_HandleLine(char *line)
{
    TelemetryData_t sample = g_telemetry;

    if (Telemetry_ParseLine(line, &sample) != TELEMETRY_LINE_OK) {
        g_stats.lines_rejected++;     /* CSV headers end up here too           */
        return;
    }

    /* Rate of change needs a previous record and time moving forwards. */
    if (g_have_previous && sample.timestamp_ms > g_telemetry.timestamp_ms) {
        float delta_time = (float)(sample.timestamp_ms - g_telemetry.timestamp_ms) / 1000.0f;

        sample.altitude_rate = (sample.altitude - g_telemetry.altitude) / delta_time;
        sample.speed_rate    = (sample.speed - g_telemetry.speed) / delta_time;
        sample.voltage_rate  = (sample.voltage - g_telemetry.voltage) / delta_time;
    } else {
        sample.altitude_rate = 0.0f;
        sample.speed_rate = 0.0f;
        sample.voltage_rate = 0.0f;
    }

    g_telemetry = sample;
    g_have_previous = 1;
    g_stats.lines_ok++;

    Telemetry_Display(&g_telemetry);
    Telemetry_Log(&g_telemetry);
}

/** Receives one byte of telemetry; on end of line parses, displays and logs. */
void Telemetry_ReceiveAndParse(void)
{
    uint8_t rx_char;
    uint8_t overrun;

    /* A receiver overrun (ORE) means bytes that arrived *after* the one now
       waiting in the data register were lost - typically while the display or
       the SD card was being updated. The flag is cleared by the SR-then-DR
       read sequence inside HAL_UART_Receive(), so sample it first. */
    overrun = __HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE) ? 1U : 0U;

    if (HAL_UART_Receive(&huart1, &rx_char, 1, 100) != HAL_OK) {
        if (overrun) {
            __HAL_UART_CLEAR_OREFLAG(&huart1);
            rx_discard = 1;
        }
        return;
    }

    if (rx_char == '\n') {
        if (rx_discard) {
            g_stats.lines_rejected++;
        } else if (rx_index > 0U) {
            rx_buffer[rx_index] = '\0';
            Telemetry_HandleLine((char *)rx_buffer);
        }
        rx_index = 0;
        rx_discard = 0;
    } else if (rx_discard) {
        /* skipping the rest of a damaged / over-long line */
    } else if (rx_index >= TELEMETRY_RX_BUFFER_SIZE - 1U) {
        rx_discard = 1;               /* over-long line: never parse a fragment */
    } else {
        rx_buffer[rx_index++] = rx_char;
    }

    if (overrun) {
        /* rx_char itself was intact; whatever followed it is incomplete. */
        rx_discard = 1;
    }
}

/* ---------------------------------------------------------------------------
 * OLED
 * ------------------------------------------------------------------------- */

void Telemetry_Display(const TelemetryData_t *data)
{
    char lineBuffer[32];

    ssd1306_Fill(Black);

    // Line 1: Time
    ssd1306_SetCursor(0, 0);
    snprintf(lineBuffer, sizeof(lineBuffer), "T:%02lu:%02lu:%02lu",
             (unsigned long)data->hours, (unsigned long)data->minutes, (unsigned long)data->seconds);
    ssd1306_WriteString(lineBuffer, Font_7x10, White);

    // Line 2: Alt + its rate
    ssd1306_SetCursor(0, 12);
    snprintf(lineBuffer, sizeof(lineBuffer), "ALT:%.2f(%+.2f)", data->altitude, data->altitude_rate);
    ssd1306_WriteString(lineBuffer, Font_7x10, White);

    // Line 3: Speed + its rate
    ssd1306_SetCursor(0, 24);
    snprintf(lineBuffer, sizeof(lineBuffer), "SPD:%.2f(%+.2f)", data->speed, data->speed_rate);
    ssd1306_WriteString(lineBuffer, Font_7x10, White);

    // Line 4: Volt + its rate
    ssd1306_SetCursor(0, 36);
    snprintf(lineBuffer, sizeof(lineBuffer), "V:%.2f(%+.3f)", data->voltage, data->voltage_rate);
    ssd1306_WriteString(lineBuffer, Font_7x10, White);

    ssd1306_SetCursor(0, 48);
    ssd1306_WriteString(is_mounted ? "LOGGING: OK" : "LOGGING: FAIL", Font_6x8, White);

    ssd1306_UpdateScreen();
}

/* ---------------------------------------------------------------------------
 * SD card logging
 * ------------------------------------------------------------------------- */

/** Attempt to mount SD card and write CSV header if file is new */
void Mount_SD_Card(void)
{
    FRESULT res;

    if (is_mounted) {
        return;
    }
    mount_attempted = 1;
    last_mount_attempt_ms = HAL_GetTick();

    res = f_mount(&fs, "", 1);
    if (res == FR_OK) {
        res = f_open(&fil, LOG_FILE_NAME, FA_OPEN_ALWAYS | FA_WRITE);
    }
    if (res == FR_OK) {
        if (f_size(&fil) == 0 && f_puts(LOG_HEADER, &fil) < 0) {
            res = FR_DISK_ERR;
        }
        if (f_close(&fil) != FR_OK) {
            res = FR_DISK_ERR;
        }
    }
    if (res == FR_OK) {
        is_mounted = 1;
    } else {
        g_stats.log_errors++;
    }
}

void Telemetry_Log(const TelemetryData_t *data)
{
    char logBuffer[128];
    UINT bytes_written = 0;
    FRESULT res;
    int len;

    if (!is_mounted) {
        /* A failed mount can block for a long time (card timeouts), and every
           blocked millisecond loses UART bytes - so do not retry on every
           record. This record is not logged. */
        if (!mount_attempted || (HAL_GetTick() - last_mount_attempt_ms) >= TELEMETRY_SD_RETRY_MS) {
            Mount_SD_Card();
        }
        return;
    }

    len = snprintf(logBuffer, sizeof(logBuffer), "%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f\n",
        (unsigned long)data->timestamp_ms, (unsigned long)data->hours,
        (unsigned long)data->minutes, (unsigned long)data->seconds,
        data->altitude, data->speed, data->voltage);
    if (len <= 0 || len >= (int)sizeof(logBuffer)) {
        g_stats.log_errors++;         /* would be truncated: skip the record    */
        return;
    }

    res = f_open(&fil, LOG_FILE_NAME, FA_OPEN_APPEND | FA_WRITE);
    if (res == FR_OK)
    {
        res = f_write(&fil, logBuffer, (UINT)len, &bytes_written);
        if (res == FR_OK && bytes_written != (UINT)len) {
            res = FR_DENIED;          /* disk full */
        }
        if (res == FR_OK) {
            res = f_sync(&fil);
        }
        if (f_close(&fil) != FR_OK && res == FR_OK) {
            res = FR_DISK_ERR;
        }
    }
    if (res != FR_OK) {
        /* Report the failure on the OLED and re-mount after the back-off. */
        g_stats.log_errors++;
        is_mounted = 0;
        last_mount_attempt_ms = HAL_GetTick();
    }
}

/* ---------------------------------------------------------------------------
 * Start-up and accessors
 * ------------------------------------------------------------------------- */

void Telemetry_Init(void)
{
    ssd1306_Init();
    Mount_SD_Card();
    Telemetry_Display(&g_telemetry);
}

const TelemetryData_t *Telemetry_GetData(void)
{
    return &g_telemetry;
}

const TelemetryStats_t *Telemetry_GetStats(void)
{
    return &g_stats;
}
