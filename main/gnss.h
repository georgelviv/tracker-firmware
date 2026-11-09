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
    int sats;          // satellites used (sum over constellations)
    int64_t ts_ms;     // UTC epoch milliseconds from the GNSS fix
} gnss_fix_t;

// Turns on the A7670E built-in GNSS engine. Call after modem_init().
esp_err_t gnss_init(void);

// Queries the current fix. Returns ESP_OK and fills `out` when there is a fix,
// ESP_ERR_NOT_FOUND when the receiver has no fix yet.
esp_err_t gnss_get_fix(gnss_fix_t *out);

// Exposed for unit testing the parser.
bool gnss_parse_cgnssinfo(const char *line, gnss_fix_t *out);
