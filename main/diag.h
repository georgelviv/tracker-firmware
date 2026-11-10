#pragma once

#include <stdbool.h>
#include <stdint.h>

// What a run (one boot until power loss or reset) got done; saved in NVS so
// the next boot can report on a run nobody was watching, e.g. on battery.
typedef struct {
    uint32_t boot;      // boot number of that run (0 = no record)
    uint32_t uptime_s;  // how long it ran, to within the save interval
    uint32_t uploads;   // successful publishes
    uint32_t failures;  // publish attempts that failed (no Wi-Fi / HTTP error)
    uint32_t no_fix;    // report cycles without a GPS fix
    int32_t min_mv;     // lowest battery reading seen (-1 = unknown)
} diag_run_t;

typedef struct {
    const char *reset_reason;  // why the chip last restarted, e.g. "brownout"
    uint32_t boots;            // total boots, kept in NVS
    uint32_t brownouts;        // boots caused by a supply-voltage dip
    diag_run_t prev;           // the run before this boot
    diag_run_t cur;            // this run so far
} diag_info_t;

typedef enum { DIAG_UPLOADED, DIAG_UPLOAD_FAILED, DIAG_NO_FIX } diag_event_t;

// Records this boot in NVS. Call once, after nvs_flash_init().
void diag_init(void);

const diag_info_t *diag_get(void);

// Records one report cycle and periodically saves this run to NVS.
void diag_record(diag_event_t ev, int battery_mv);
