#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Constellations reported in GSV sentences.
enum { GNSS_SYS_GPS, GNSS_SYS_GLONASS, GNSS_SYS_BEIDOU, GNSS_SYS_GALILEO, GNSS_SYS_COUNT };

typedef struct {
    double lat;        // decimal degrees, +N / -S
    double lon;        // decimal degrees, +E / -W
    float alt_m;       // above mean sea level
    float geoid_m;     // geoid separation (GGA)
    float speed_kmh;
    float course_deg;
    float hdop, pdop, vdop;
    int sats;          // satellites used in the fix
    int quality;       // GGA fix quality: 0 none, 1 GPS, 2 DGPS, 6 estimated
    int fix_type;      // GSA: 1 none, 2 = 2D, 3 = 3D
    int in_view[GNSS_SYS_COUNT];  // satellites in view per constellation (GSV)
    float snr_avg;     // mean C/N0 (dB-Hz) of satellites with a signal
    int snr_max;
    int64_t ts_ms;     // UTC epoch milliseconds from the GNSS fix
} gnss_fix_t;

// Starts the UART to the L76K GPS module and a task that parses its NMEA output.
esp_err_t gnss_init(void);

// Returns ESP_OK and fills `out` when there is a current fix,
// ESP_ERR_NOT_FOUND while the receiver is still searching, and
// ESP_ERR_INVALID_STATE when no NMEA data is arriving at all (wiring/power).
esp_err_t gnss_get_fix(gnss_fix_t *out);

// ---- exposed for host unit tests ----

typedef struct {
    gnss_fix_t fix;
    bool rmc_valid;  // last RMC sentence had status 'A'
    // GSV is split over several sentences per constellation; signal stats
    // are collected here and published to `fix` when the last one arrives.
    int snr_sum[GNSS_SYS_COUNT], snr_cnt[GNSS_SYS_COUNT], snr_max[GNSS_SYS_COUNT];
    int gsv_sum, gsv_cnt, gsv_max;  // running totals of the current GSV group
} gnss_nmea_state_t;

// Parses one NMEA sentence (with or without CRLF) into `st`.
// Returns true if the sentence was a valid RMC, GGA, GSA or GSV with a correct checksum.
bool gnss_nmea_parse(const char *line, gnss_nmea_state_t *st);
