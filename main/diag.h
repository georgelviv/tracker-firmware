#pragma once

#include <stdint.h>

typedef struct {
    const char *reset_reason;  // why the chip last restarted, e.g. "brownout"
    uint32_t boots;            // total boots, kept in NVS
    uint32_t brownouts;        // boots caused by a supply-voltage dip
} diag_info_t;

// Records this boot in NVS. Call once, after nvs_flash_init().
void diag_init(void);

const diag_info_t *diag_get(void);
