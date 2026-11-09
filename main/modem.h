#pragma once

#include <stddef.h>
#include "esp_err.h"

// Powers the A7670 modem on and waits until it answers AT commands.
esp_err_t modem_init(void);

// Sends `cmd` (without CRLF) and collects the reply into `resp` until a final
// "OK" / "ERROR" line or the timeout. Returns ESP_OK on "OK",
// ESP_FAIL on "ERROR", ESP_ERR_TIMEOUT otherwise. `resp` may be NULL.
// Thread-safe: calls are serialized with a mutex.
esp_err_t modem_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms);
