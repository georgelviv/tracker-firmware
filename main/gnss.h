#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    double lat;        // decimal degrees, +N / -S
    double lon;        // decimal degrees, +E / -W
    float alt_m;
    float speed_kmh;
    float course_deg;
    float hdop;
    int sats;          // satellites used in the fix
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
} gnss_nmea_state_t;

// Parses one NMEA sentence (with or without CRLF) into `st`.
// Returns true if the sentence was a valid RMC or GGA with a correct checksum.
bool gnss_nmea_parse(const char *line, gnss_nmea_state_t *st);
