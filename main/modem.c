#include "modem.h"

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "modem";
static SemaphoreHandle_t s_lock;
static esp_modem_dce_t *s_dce;
static esp_netif_t *s_ppp;
static EventGroupHandle_t s_events;
#define PPP_UP_BIT BIT0
static bool s_ready;     // answered AT after power-up
static bool s_cmux;      // AT commands and PPP share the UART through CMUX
static bool s_data_only; // PPP owns the UART; AT commands are unavailable

static void out_pin(int pin, int level)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void power_sequence(void)
{
    // Power rail on.
    out_pin(BOARD_POWERON_PIN, 1);

    // Hardware reset pulse.
    out_pin(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(2600));
    gpio_set_level(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

    // PWRKEY pulse turns the module on.
    out_pin(BOARD_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_PIN, 0);

    // Keep DTR low so the modem does not enter sleep.
    out_pin(MODEM_DTR_PIN, 0);
}

// ---------------------------------------------------------------------------
// AT commands
// ---------------------------------------------------------------------------

// Over the plain UART esp_modem hands the callback everything received so
// far; through CMUX it hands over each frame on its own, so those are
// appended. s_lock makes these statics safe.
static char *s_resp;
static size_t s_resp_len;
static size_t s_got;

static esp_err_t on_reply(uint8_t *data, size_t len)
{
    size_t at = s_cmux ? s_got : 0;
    size_t n = len < s_resp_len - 1 - at ? len : s_resp_len - 1 - at;
    memcpy(s_resp + at, data, n);
    s_got = at + n;
    s_resp[s_got] = '\0';
    if (strstr(s_resp, "\r\nOK\r\n") || strncmp(s_resp, "OK\r\n", 4) == 0) {
        return ESP_OK;
    }
    if (strstr(s_resp, "ERROR")) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;  // keep waiting
}

esp_err_t modem_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    char local[256];
    if (!resp) {
        resp = local;
        resp_len = sizeof(local);
    }
    resp[0] = '\0';
    if (!s_dce || s_data_only) {
        return ESP_ERR_INVALID_STATE;
    }

    char line[96];
    snprintf(line, sizeof(line), "%s\r", cmd);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_resp = resp;
    s_resp_len = resp_len;
    s_got = 0;
    esp_err_t ret = esp_modem_command(s_dce, line, on_reply, timeout_ms);
    s_resp = NULL;
    xSemaphoreGive(s_lock);

    if (ret != ESP_OK && ret != ESP_FAIL) {
        ret = ESP_ERR_TIMEOUT;
    }
    ESP_LOGD(TAG, "%s -> %s", cmd, resp);
    return ret;
}

bool modem_is_ready(void)
{
    return s_ready;
}

// ---------------------------------------------------------------------------
// Mobile data (PPP)
// ---------------------------------------------------------------------------

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "mobile data up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, PPP_UP_BIT);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "mobile data lost");
        xEventGroupClearBits(s_events, PPP_UP_BIT);
    }
}

bool modem_data_up(void)
{
    return s_events && (xEventGroupGetBits(s_events) & PPP_UP_BIT);
}

bool modem_get_ip(char *out, size_t len)
{
    esp_netif_ip_info_t ip;
    if (!modem_data_up() || esp_netif_get_ip_info(s_ppp, &ip) != ESP_OK) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    return true;
}

static bool registered(void)
{
    char buf[96];
    for (const char *cmd = "AT+CEREG?"; cmd; cmd = strcmp(cmd, "AT+CEREG?") == 0 ? "AT+CREG?" : NULL) {
        if (modem_at(cmd, buf, sizeof(buf), 1000) == ESP_OK) {
            const char *p = strstr(buf, "REG: ");
            p = p ? strchr(p, ',') : NULL;
            if (p && (p[1] == '1' || p[1] == '5')) {
                return true;
            }
        }
    }
    return false;
}

// Back to plain AT commands so the next attempt starts clean.
static void drop_data(void)
{
    xEventGroupClearBits(s_events, PPP_UP_BIT);
    if (s_cmux || s_data_only) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        xSemaphoreGive(s_lock);
    }
    s_cmux = s_data_only = false;
}

// Brings mobile data up once the SIM is registered, and back up if it drops.
// PPP runs alongside Wi-Fi; the IP stack sends through Wi-Fi while it is
// connected (higher route priority) and falls back to LTE when it is not.
static void data_task(void *arg)
{
    int64_t since_down = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_cmux || s_data_only) {
            if (modem_data_up()) {
                since_down = 0;
            } else if ((since_down += 5) >= 60) {  // dialled but no IP for a minute
                ESP_LOGW(TAG, "mobile data did not come up; redialling");
                drop_data();
                since_down = 0;
            }
            continue;
        }
        if (!registered()) {
            continue;
        }
        ESP_LOGI(TAG, "registered; starting mobile data (APN \"%s\")", CONFIG_TRACKER_APN);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX);
        xSemaphoreGive(s_lock);
        if (err == ESP_OK) {
            s_cmux = true;
            continue;
        }
        // Without CMUX, PPP takes the whole UART and the modem status is not refreshed.
        ESP_LOGW(TAG, "CMUX failed (%s); using plain data mode", esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        xSemaphoreGive(s_lock);
        if (err == ESP_OK) {
            s_data_only = true;
        } else {
            ESP_LOGE(TAG, "could not start mobile data (%s)", esp_err_to_name(err));
            drop_data();
        }
    }
}

// ---------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------

esp_err_t modem_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();

    ESP_LOGI(TAG, "powering on modem");
    power_sequence();

    esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp = esp_netif_new(&ppp_cfg);
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));

    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte.uart_config.port_num = MODEM_UART_NUM;
    dte.uart_config.baud_rate = MODEM_BAUDRATE;
    dte.uart_config.tx_io_num = MODEM_TX_PIN;
    dte.uart_config.rx_io_num = MODEM_RX_PIN;
    dte.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
    dte.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
    dte.uart_config.rx_buffer_size = 4096;
    dte.uart_config.tx_buffer_size = 1024;
    dte.dte_buffer_size = 4096;  // CMUX reassembles PPP frames of up to ~1.5 KB here
    esp_modem_dce_config_t dce = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_TRACKER_APN);
    // The A7670 speaks the same AT dialect as the SIM7600.
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce, s_ppp);
    if (!s_dce) {
        ESP_LOGE(TAG, "esp_modem init failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < 30; i++) {
        if (modem_at("AT", NULL, 0, 1000) == ESP_OK) {
            modem_at("ATE0", NULL, 0, 1000);      // no command echo
            modem_at("AT+CMEE=2", NULL, 0, 1000); // verbose "+CME ERROR: <text>"
            char buf[256];
            // Log the model: this board's A7670G-LLSE has no GNSS (the L76K does that).
            if (modem_at("AT+SIMCOMATI", buf, sizeof(buf), 2000) == ESP_OK ||
                modem_at("ATI", buf, sizeof(buf), 2000) == ESP_OK) {
                ESP_LOGI(TAG, "modem ready:\n%s", buf);
            }
            s_ready = true;
            xTaskCreate(data_task, "modem_data", 4096, NULL, 4, NULL);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGE(TAG, "modem did not respond to AT");
    return ESP_ERR_TIMEOUT;
}
