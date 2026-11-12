#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Constellations reported in GSV sentences.
enum { GNSS_SYS_GPS, GNSS_SYS_GLONASS, GNSS_SYS_BEIDOU, GNSS_SYS_GALILEO, GNSS_SYS_COUNT };

// Most satellites tracked at once (GPS + BeiDou typically show 20-30).
#define GNSS_MAX_SATS 40
#define GNSS_MAX_PER_SYS 24

// One satellite as reported in GSV (and GSA for whether it is used).
typedef struct {
    uint8_t sys;    // GNSS_SYS_*
    uint8_t prn;    // satellite number within its constellation
    int8_t elev;    // degrees above the horizon (0-90), -1 unknown
    int16_t az;     // degrees from true north (0-359), -1 unknown
    int8_t snr;     // C/N0 in dB-Hz, -1 when not tracked
    bool used;      // part of the position solution (GSA)
} gnss_sat_t;

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
    int64_t clock_ms;  // receiver UTC clock, also while searching (0 = not set yet)
    char mode;         // RMC mode: A autonomous, D differential, E estimated, N none
    float err_lat_m, err_lon_m, err_alt_m;  // 1-sigma error estimate (GST), 0 if not sent
    char antenna[8];   // "OK" / "OPEN" / "SHORT" from $GPTXT, empty if not sent
    int ttff_s;        // seconds from power-on to the first fix, -1 not yet (device only)
    int sat_count;
    gnss_sat_t sat[GNSS_MAX_SATS];  // every satellite in view
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
    // Satellites per constellation (last complete GSV group) and the one being read.
    gnss_sat_t sys_sat[GNSS_SYS_COUNT][GNSS_MAX_PER_SYS];
    int sys_sat_n[GNSS_SYS_COUNT];
    gnss_sat_t gsv_sat[GNSS_MAX_PER_SYS];
    int gsv_sat_n;
    // Satellite numbers used in the solution, per constellation (GSA).
    uint8_t used[GNSS_SYS_COUNT][12];
    int used_n[GNSS_SYS_COUNT];
} gnss_nmea_state_t;

// Parses one NMEA sentence (with or without CRLF) into `st`.
// Returns true if the sentence was a valid RMC, GGA, GSA, GSV, GST or TXT with a correct checksum.
bool gnss_nmea_parse(const char *line, gnss_nmea_state_t *st);
