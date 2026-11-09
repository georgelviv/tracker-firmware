#pragma once

#include "esp_err.h"
#include "gnss.h"

// Overwrites /devices/<id>/latest in the Realtime Database with the fix.
// Only the newest position is kept; there is no history.
// Signs in (Firebase Auth e-mail/password) on first use and when the ID token
// is about to expire. Uses whatever IP link is up (Wi-Fi now, LTE/PPP later).
esp_err_t firebase_publish_fix(const gnss_fix_t *fix);
