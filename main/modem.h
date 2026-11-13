#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Powers the A7670 modem on, waits until it answers AT commands, and starts
// a background task that brings up mobile data (PPP) once the SIM is
// registered. Needs esp_netif_init() and the default event loop.
esp_err_t modem_init(void);

// Sends `cmd` (without CRLF) and collects the reply into `resp` until a final
// "OK" / "ERROR" line or the timeout. Returns ESP_OK on "OK",
// ESP_FAIL on "ERROR", ESP_ERR_TIMEOUT otherwise, and ESP_ERR_INVALID_STATE
// while the modem is unavailable. `resp` may be NULL.
// Thread-safe: calls are serialized with a mutex.
esp_err_t modem_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms);

// True once modem_init() succeeded.
bool modem_is_ready(void);

// True while mobile data has an IP address.
bool modem_data_up(void);

// Mobile data IP address as text. Returns false while data is down.
bool modem_get_ip(char *out, size_t len);
