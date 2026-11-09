#include "wifi.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define CONNECTED_BIT BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t wifi_start(void)
{
    if (strlen(CONFIG_TRACKER_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Wi-Fi SSID not set - run `idf.py menuconfig`");
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_TRACKER_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_TRACKER_WIFI_PASSWORD, sizeof(cfg.sta.password));
    // Minimum accepted security; WPA3 networks still connect.
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_TRACKER_WIFI_SSID);
    return ESP_OK;
}

bool wifi_get_info(link_info_t *out)
{
    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!wifi_wait_connected(0) || esp_wifi_sta_get_ap_info(&ap) != ESP_OK || !netif ||
        esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    out->type = "wifi";
    strlcpy(out->name, (const char *)ap.ssid, sizeof(out->name));
    out->rssi = ap.rssi;
    out->channel = ap.primary;
    snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
    return true;
}

bool wifi_wait_connected(int timeout_ms)
{
    if (!s_events) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return bits & CONNECTED_BIT;
}
