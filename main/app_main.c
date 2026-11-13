#include <inttypes.h>
#include <string.h>
#include "battery.h"
#include "cell.h"
#include "diag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "firebase.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss.h"
#include "modem.h"
#include "nvs_flash.h"
#include "wifi.h"

static const char *TAG = "tracker";

// Wi-Fi while it is connected, otherwise mobile data. The IP stack routes the
// same way (Wi-Fi has the higher route priority), so this only describes it.
static bool pick_link(link_info_t *out, const cell_info_t *cell)
{
    if (!modem_data_up() && wifi_wait_connected(5000)) {
        return wifi_get_info(out);
    }
    if (wifi_wait_connected(0)) {
        return wifi_get_info(out);
    }
    if (!modem_get_ip(out->ip, sizeof(out->ip))) {
        return false;
    }
    out->type = "lte";
    strlcpy(out->name, cell && cell->op[0] ? cell->op : "", sizeof(out->name));
    out->rssi = cell && cell->rsrp != CELL_NONE ? (int)cell->rsrp : 0;  // RSRP, dBm
    out->channel = 0;
    return true;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    diag_init();
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable");
    }

    ESP_LOGI(TAG, "device %s, reporting every %d s", CONFIG_TRACKER_DEVICE_ID,
             CONFIG_TRACKER_REPORT_INTERVAL_S);

    ESP_ERROR_CHECK(gnss_init());  // start searching for satellites right away
    wifi_start();

    // Mobile data is the fallback when Wi-Fi is out of range.
    // A failure here does not stop tracking over Wi-Fi.
    if (modem_init() != ESP_OK) {
        ESP_LOGW(TAG, "modem not responding; continuing with Wi-Fi only");
    }

    const TickType_t period = pdMS_TO_TICKS(CONFIG_TRACKER_REPORT_INTERVAL_S * 1000);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        gnss_fix_t fix = {0};
        err = gnss_get_fix(&fix);
        gps_state_t state = err == ESP_OK               ? GPS_FIX
                            : err == ESP_ERR_NOT_FOUND ? GPS_SEARCHING
                                                       : GPS_NO_DATA;
        if (state == GPS_FIX) {
            ESP_LOGI(TAG, "fix %.6f,%.6f alt=%.1fm %.1fkm/h sats=%d/%d %dD hdop=%.1f snr=%.0f ts=%" PRId64,
                     fix.lat, fix.lon, fix.alt_m, fix.speed_kmh, fix.sats,
                     fix.in_view[0] + fix.in_view[1] + fix.in_view[2] + fix.in_view[3],
                     fix.fix_type, fix.hdop, fix.snr_avg, fix.ts_ms);
        } else if (state == GPS_SEARCHING) {
            ESP_LOGI(TAG, "waiting for GPS fix... (satellites in use: %d)", fix.sats);
        } else {
            ESP_LOGW(TAG, "no data from the L76K GPS module - check that it is fitted and powered");
        }

        // Report every cycle so the map can tell "online, no fix" from offline.
        static cell_info_t cell;  // ~250 bytes, kept off the stack
        bool have_cell = cell_read(&cell) == ESP_OK;
        link_info_t link;
        bool have_link = pick_link(&link, have_cell ? &cell : NULL);
        bool ok = false;
        if (have_link) {
            ok = firebase_publish(state, &fix, &link, have_cell ? &cell : NULL) == ESP_OK;
            if (ok) {
                ESP_LOGI(TAG, "published%s via %s", state == GPS_FIX ? "" : " status (no fix)",
                         link.type);
            }
        } else {
            ESP_LOGW(TAG, "no Wi-Fi or mobile data, report dropped");  // TODO: queue offline
        }
        diag_record(state != GPS_FIX ? DIAG_NO_FIX : ok ? DIAG_UPLOADED : DIAG_UPLOAD_FAILED,
                    battery_read_mv());
        vTaskDelayUntil(&last, period);
    }
}
