#include <inttypes.h>
#include "esp_event.h"
#include "esp_log.h"
#include "firebase.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss.h"
#include "modem.h"
#include "nvs_flash.h"
#include "wifi.h"

static const char *TAG = "tracker";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "device %s, reporting every %d s", CONFIG_TRACKER_DEVICE_ID,
             CONFIG_TRACKER_REPORT_INTERVAL_S);

    ESP_ERROR_CHECK(gnss_init());  // start searching for satellites right away
    wifi_start();

    // The modem is not needed for Wi-Fi tracking; it is powered up now so the
    // LTE path can be added later. A failure here does not stop tracking.
    if (modem_init() != ESP_OK) {
        ESP_LOGW(TAG, "modem not responding; continuing with Wi-Fi only");
    }

    const TickType_t period = pdMS_TO_TICKS(CONFIG_TRACKER_REPORT_INTERVAL_S * 1000);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        gnss_fix_t fix;
        err = gnss_get_fix(&fix);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "fix %.6f,%.6f alt=%.1fm %.1fkm/h sats=%d/%d %dD hdop=%.1f snr=%.0f ts=%" PRId64,
                     fix.lat, fix.lon, fix.alt_m, fix.speed_kmh, fix.sats,
                     fix.in_view[0] + fix.in_view[1] + fix.in_view[2] + fix.in_view[3],
                     fix.fix_type, fix.hdop, fix.snr_avg, fix.ts_ms);
            link_info_t link;
            if (wifi_wait_connected(5000)) {
                if (firebase_publish_fix(&fix, wifi_get_info(&link) ? &link : NULL) == ESP_OK) {
                    ESP_LOGI(TAG, "published");
                }
            } else {
                ESP_LOGW(TAG, "no Wi-Fi, fix dropped");  // TODO: queue offline / send via LTE
            }
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "waiting for GPS fix... (satellites in use: %d)", fix.sats);
        } else {
            ESP_LOGW(TAG, "no data from the L76K GPS module - check that it is fitted and powered");
        }
        vTaskDelayUntil(&last, period);
    }
}
