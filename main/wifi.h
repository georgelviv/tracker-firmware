#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "link.h"

// Starts Wi-Fi station mode and keeps reconnecting in the background.
esp_err_t wifi_start(void);

// Blocks until an IP address is assigned or the timeout expires.
bool wifi_wait_connected(int timeout_ms);

// Fills `out` with the current access point and IP. Returns false if not connected.
bool wifi_get_info(link_info_t *out);
