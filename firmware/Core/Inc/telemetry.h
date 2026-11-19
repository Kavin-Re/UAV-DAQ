/**
  ******************************************************************************
  * @file    telemetry.h
  * @brief   Telemetry line decoder, OLED view and SD logger (application layer).
  *
  * Wire format (docs/PROTOCOL.md, "v1"):
  *
  *     TimeMS,Hour,Min,Sec,Altitude,Speed,Voltage[*HH]\n
  *
  * The optional "*HH" suffix is an NMEA-style XOR checksum over every character
  * before the '*'. Lines without it are accepted; lines with a wrong one are
  * dropped.
  ******************************************************************************
  */
#ifndef TELEMETRY_H
#define TELEMETRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/** Longest accepted line is TELEMETRY_RX_BUFFER_SIZE - 1 characters. */
#define TELEMETRY_RX_BUFFER_SIZE 128U

/** Minimum time between SD mount attempts while the card is unavailable. */
#define TELEMETRY_SD_RETRY_MS 5000U

typedef struct {
    float altitude;          /* m   (AHR2.Alt)                    */
    float speed;             /* m/s (ground speed from XKF1.VN/VE) */
    float voltage;           /* V   (BAT.Volt)                    */

    uint32_t timestamp_ms;   /* From log (milliseconds since autopilot boot) */
    uint32_t hours;          /* From log */
    uint32_t minutes;
    uint32_t seconds;

    /* Rates of change between the last two accepted records (per second) */
    float altitude_rate;
    float speed_rate;
    float voltage_rate;
} TelemetryData_t;

typedef enum {
    TELEMETRY_LINE_OK = 0,
    TELEMETRY_LINE_MALFORMED,    /* wrong field count / not a number / range */
    TELEMETRY_LINE_BAD_CHECKSUM  /* "*HH" present but wrong                  */
} TelemetryLineStatus_t;

typedef struct {
    uint32_t lines_ok;
    uint32_t lines_rejected;     /* malformed, bad checksum, over-long, overrun */
    uint32_t log_errors;         /* failed SD mounts / opens / writes           */
} TelemetryStats_t;

/**
  * Decode one text line (no terminator needed) into the acquisition fields of
  * @p out (timestamp, h/m/s, altitude, speed, voltage). Rates are not touched.
  * @p line is modified (the checksum suffix is cut off).
  */
TelemetryLineStatus_t Telemetry_ParseLine(char *line, TelemetryData_t *out);

/** Initialise the OLED, try to mount the SD card and draw the first screen. */
void Telemetry_Init(void);

/** Poll USART1 for one byte; on a complete line parse, display and log it. */
void Telemetry_ReceiveAndParse(void);

void Telemetry_Display(const TelemetryData_t *data);
void Telemetry_Log(const TelemetryData_t *data);
void Mount_SD_Card(void);

const TelemetryData_t  *Telemetry_GetData(void);
const TelemetryStats_t *Telemetry_GetStats(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_H */
