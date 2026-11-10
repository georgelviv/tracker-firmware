#include "diag.h"

#include "esp_log.h"
#include "esp_system.h"
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

    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) == ESP_OK) {
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
    ESP_LOGI(TAG, "boot #%lu, reset reason: %s, brownouts so far: %lu",
             (unsigned long)s_info.boots, s_info.reset_reason, (unsigned long)s_info.brownouts);
}

const diag_info_t *diag_get(void)
{
    return &s_info;
}
