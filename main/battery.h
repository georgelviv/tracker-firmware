#pragma once

#include "esp_err.h"

// Sets up the ADC on the battery sense pin (GPIO35, 1:2 divider on the board).
esp_err_t battery_init(void);

// Battery voltage in millivolts, averaged over a few samples; -1 on error.
// With USB and no battery fitted this reads the charger's output (~4.2 V).
int battery_read_mv(void);
