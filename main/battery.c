#include "battery.h"

#include <stdbool.h>
#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define BAT_ADC_CHANNEL ADC_CHANNEL_7  // GPIO35 = ADC1 channel 7
#define BAT_DIVIDER     2              // two equal resistors on the board
#define BAT_SAMPLES     16

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

esp_err_t battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan);
    if (err != ESP_OK) {
        return err;
    }
    // Line-fitting calibration uses the per-chip reference voltage from eFuse.
    adc_cali_line_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration; voltage will be less accurate");
        s_cali = NULL;
    }
    ESP_LOGI(TAG, "battery %d mV", battery_read_mv());
    return ESP_OK;
}

int battery_read_mv(void)
{
    if (!s_adc) {
        return -1;
    }
    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            sum += mv;
        } else {
            sum += raw * 3300 / 4095;  // rough fallback
        }
        n++;
    }
    return n ? sum / n * BAT_DIVIDER : -1;
}
