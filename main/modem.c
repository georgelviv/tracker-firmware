#include "modem.h"

#include <string.h>
#include "board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "modem";
static SemaphoreHandle_t s_lock;
static bool s_ready;  // answered AT after power-up

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

esp_err_t modem_at(const char *cmd, char *resp, size_t resp_len, int timeout_ms)
{
    char local[256];
    if (!resp) {
        resp = local;
        resp_len = sizeof(local);
    }
    resp[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uart_flush_input(MODEM_UART_NUM);
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    size_t len = 0;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline && len < resp_len - 1) {
        int n = uart_read_bytes(MODEM_UART_NUM, (uint8_t *)resp + len,
                                resp_len - 1 - len, pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        len += n;
        resp[len] = '\0';
        if (strstr(resp, "\r\nOK\r\n") || strncmp(resp, "OK\r\n", 4) == 0) {
            ret = ESP_OK;
            break;
        }
        if (strstr(resp, "ERROR")) {
            ret = ESP_FAIL;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGD(TAG, "%s -> %s", cmd, resp);
    return ret;
}

bool modem_is_ready(void)
{
    return s_ready;
}

esp_err_t modem_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = MODEM_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART_NUM, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "powering on modem");
    power_sequence();

    for (int i = 0; i < 30; i++) {
        if (modem_at("AT", NULL, 0, 1000) == ESP_OK) {
            modem_at("ATE0", NULL, 0, 1000);     // no command echo
            modem_at("AT+CMEE=2", NULL, 0, 1000); // verbose "+CME ERROR: <text>"
            char buf[256];
            // Log the model: this board's A7670G-LLSE has no GNSS (the L76K does that).
            if (modem_at("AT+SIMCOMATI", buf, sizeof(buf), 2000) == ESP_OK ||
                modem_at("ATI", buf, sizeof(buf), 2000) == ESP_OK) {
                ESP_LOGI(TAG, "modem ready:\n%s", buf);
            }
            s_ready = true;
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "modem did not respond to AT");
    return ESP_ERR_TIMEOUT;
}
