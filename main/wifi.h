#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Starts Wi-Fi station mode and keeps reconnecting in the background.
esp_err_t wifi_start(void);

// Blocks until an IP address is assigned or the timeout expires.
bool wifi_wait_connected(int timeout_ms);
