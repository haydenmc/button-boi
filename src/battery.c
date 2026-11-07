#include "battery.h"

#include "sdkconfig.h"

#if CONFIG_BB_BATTERY_SENSE

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

// The divider pin sees VBAT * R_BOTTOM / (R_TOP + R_BOTTOM): ~1.12 V at a full
// 4.2 V on the TinyS3, which fits the 2.5 dB range (~0-1.25 V).
#define VBAT_ATTEN ADC_ATTEN_DB_2_5
#define SAMPLES 16
#define DISCARD 4 // the divider is high-impedance (~600 k); let the sample cap settle

uint16_t battery_read_mv(void)
{
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(CONFIG_BB_VBAT_GPIO, &unit, &chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin", CONFIG_BB_VBAT_GPIO);
        return 0;
    }

    adc_oneshot_unit_handle_t adc = NULL;
    adc_cali_handle_t cali = NULL;
    uint16_t result = 0;

    const adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit};
    const adc_oneshot_chan_cfg_t chan_cfg = {.atten = VBAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = unit, .chan = chan, .atten = VBAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK ||
        adc_oneshot_config_channel(adc, chan, &chan_cfg) != ESP_OK ||
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed");
        goto out;
    }

    int sum = 0, mv = 0;
    for (int i = 0; i < DISCARD + SAMPLES; i++) {
        if (adc_oneshot_get_calibrated_result(adc, cali, chan, &mv) != ESP_OK) {
            goto out;
        }
        if (i >= DISCARD) {
            sum += mv;
        }
    }
    const uint32_t pin_mv = sum / SAMPLES;
    result = (uint16_t)(pin_mv * (CONFIG_BB_VBAT_R_TOP_KOHM + CONFIG_BB_VBAT_R_BOTTOM_KOHM) /
                        CONFIG_BB_VBAT_R_BOTTOM_KOHM);
    ESP_LOGI(TAG, "VBAT %u mV (pin %lu mV)", result, (unsigned long)pin_mv);

out:
    if (cali) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    if (adc) {
        adc_oneshot_del_unit(adc);
    }
    return result;
}

bool battery_usb_present(void)
{
#if CONFIG_BB_VBUS_GPIO >= 0
    // Externally divided (3k3 / 2k on TinyS3), so no internal pulls.
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << CONFIG_BB_VBUS_GPIO, .mode = GPIO_MODE_INPUT};
    gpio_config(&cfg);
    return gpio_get_level(CONFIG_BB_VBUS_GPIO) == 1;
#else
    return false;
#endif
}

#else // !CONFIG_BB_BATTERY_SENSE

uint16_t battery_read_mv(void) { return 0; }
bool battery_usb_present(void) { return false; }

#endif
