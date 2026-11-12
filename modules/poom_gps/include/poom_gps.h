// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Presence bits: an absent field is never a valid zero. All times are UTC. */
enum { GPS_POSITION=1, GPS_DATE=2, GPS_TIME=4, GPS_ALTITUDE=8,
       GPS_SPEED=16, GPS_COURSE=32, GPS_HDOP=64, GPS_SATELLITES=128,
       GPS_QUALITY=256, GPS_TYPE=512 };
typedef struct {
    double latitude, longitude, altitude_m, speed_kmh, course_deg, hdop;
    uint32_t fields;
    uint64_t updated_ms, position_ms, sentence_ms, date_ms, time_ms;
    unsigned year, month, day, hour, minute, second, satellites, quality, fix_type;
    bool connected, fix;
    char receiver[24];
} poom_gps_fix_t;
typedef struct {
    poom_gps_fix_t fix;
    char sentence[128];
    size_t length;
    uint32_t valid_sentences, checksum_errors, framing_errors, overflows;
} poom_nmea_t;
bool poom_nmea_checksum(const char *line);
void poom_nmea_init(poom_nmea_t *p);
/* Returns true for a checksum-valid, supported, structurally valid sentence. */
bool poom_nmea_sentence(poom_nmea_t *p, const char *line, uint64_t now_ms);
bool poom_nmea_feed(poom_nmea_t *p, uint8_t byte, uint64_t now_ms);
poom_gps_fix_t poom_nmea_snapshot(const poom_nmea_t *p, uint64_t now_ms);
/* Apply the same freshness policy to a cached fix at observation time. */
poom_gps_fix_t poom_gps_fix_at(const poom_gps_fix_t *fix, uint64_t now_ms);
/* Adapter contract: no parser/engine knowledge of transport. Return bytes,
 * zero for idle, -1 for communication failure. No persistent config writes. */
typedef struct {
    const char *name;
    bool (*open)(void *context);
    int (*read)(void *context, uint8_t *out, size_t capacity);
    void (*close)(void *context);
    void *context;
} poom_gps_adapter_t;
typedef void (*poom_gps_diagnostic_cb)(const char *sentence, bool valid, void *context);
typedef struct {
    poom_nmea_t parser;
    const poom_gps_adapter_t *adapter;
    uint64_t retry_ms;
    bool opened;
    poom_gps_diagnostic_cb diagnostic;
    void *diagnostic_context;
} poom_gps_service_t;
void poom_gps_service_init(poom_gps_service_t *s, const poom_gps_adapter_t *adapter);
void poom_gps_service_poll(poom_gps_service_t *s, uint64_t now_ms);
void poom_gps_service_close(poom_gps_service_t *s);
const poom_gps_adapter_t *poom_gps_xa1110(void);
