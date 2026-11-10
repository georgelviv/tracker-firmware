#include "diag.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "diag";
static diag_info_t s_info;

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_PWR_GLITCH: return "power-glitch";
    default:                return "unknown";
    }
}

void diag_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    s_info.reset_reason = reason_name(r);
    s_info.prev.min_mv = -1;
    s_info.cur.min_mv = -1;

    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof(s_info.prev);
        if (nvs_get_blob(h, "run", &s_info.prev, &len) != ESP_OK || len != sizeof(s_info.prev)) {
            memset(&s_info.prev, 0, sizeof(s_info.prev));
            s_info.prev.min_mv = -1;
        }
        nvs_get_u32(h, "boots", &s_info.boots);
        nvs_get_u32(h, "brownouts", &s_info.brownouts);
        s_info.boots++;
        if (r == ESP_RST_BROWNOUT || r == ESP_RST_PWR_GLITCH) {
            s_info.brownouts++;
        }
        nvs_set_u32(h, "boots", s_info.boots);
        nvs_set_u32(h, "brownouts", s_info.brownouts);
        nvs_commit(h);
        nvs_close(h);
    }
    s_info.cur.boot = s_info.boots;
    ESP_LOGI(TAG, "boot #%lu, reset reason: %s, brownouts so far: %lu",
             (unsigned long)s_info.boots, s_info.reset_reason, (unsigned long)s_info.brownouts);
    const diag_run_t *p = &s_info.prev;
    if (p->boot) {
        ESP_LOGI(TAG, "previous run (boot #%lu): up %lu s, %lu uploads, %lu failed, %lu without fix, "
                      "lowest battery %ld mV",
                 (unsigned long)p->boot, (unsigned long)p->uptime_s, (unsigned long)p->uploads,
                 (unsigned long)p->failures, (unsigned long)p->no_fix, (long)p->min_mv);
    }
}

void diag_record(diag_event_t ev, int battery_mv)
{
    diag_run_t *c = &s_info.cur;
    switch (ev) {
    case DIAG_UPLOADED:      c->uploads++; break;
    case DIAG_UPLOAD_FAILED: c->failures++; break;
    case DIAG_NO_FIX:        c->no_fix++; break;
    }
    if (battery_mv > 0 && (c->min_mv < 0 || battery_mv < c->min_mv)) {
        c->min_mv = battery_mv;
    }
    c->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    // Save every cycle for the first 10 minutes (when problems show up),
    // then every ~5 minutes to spare the flash.
    static uint32_t cycles;
    cycles++;
    if (c->uptime_s > 600 && cycles % 10 != 0) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "run", c, sizeof(*c));
        nvs_commit(h);
        nvs_close(h);
    }
}

const diag_info_t *diag_get(void)
{
    return &s_info;
}
