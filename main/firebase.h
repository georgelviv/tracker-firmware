#pragma once

#include "esp_err.h"
#include "cell.h"
#include "gnss.h"
#include "link.h"

typedef enum { GPS_FIX, GPS_SEARCHING, GPS_NO_DATA } gps_state_t;

// Updates /devices/<id> in the Realtime Database in one request:
//   status - every call: GPS state, satellites, connection, battery, diagnostics
//   latest - only when state is GPS_FIX: the position (no history is kept)
// Signs in (Firebase Auth e-mail/password) on first use and when the ID token
// is about to expire. Uses whatever IP link is up (Wi-Fi, else LTE/PPP).
// `link` describes the connection used and `cell` the mobile modem; either may be NULL.
esp_err_t firebase_publish(gps_state_t state, const gnss_fix_t *fix, const link_info_t *link,
                           const cell_info_t *cell);
