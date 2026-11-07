#include "button.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

static const char *TAG = "button";

#define BTN_GPIO ((gpio_num_t)CONFIG_BB_BUTTON_GPIO)
#ifdef CONFIG_BB_BUTTON_ACTIVE_LOW
#define BTN_ACTIVE_LEVEL 0
#else
#define BTN_ACTIVE_LEVEL 1
#endif

void button_init(void)
{
    // After a deep-sleep wake the pin is still routed to the RTC mux.
    if (rtc_gpio_is_valid_gpio(BTN_GPIO)) {
        rtc_gpio_deinit(BTN_GPIO);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_BB_BUTTON_INTERNAL_PULL
        .pull_up_en = BTN_ACTIVE_LEVEL == 0,
        .pull_down_en = BTN_ACTIVE_LEVEL == 1,
#endif
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

bool button_pressed(void)
{
    return gpio_get_level(BTN_GPIO) == BTN_ACTIVE_LEVEL;
}

void button_enable_wake(void)
{
    if (!rtc_gpio_is_valid_gpio(BTN_GPIO)) {
        ESP_LOGE(TAG, "GPIO%d is not an RTC GPIO; cannot wake from deep sleep", BTN_GPIO);
        return;
    }
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
        1ULL << BTN_GPIO, BTN_ACTIVE_LEVEL ? ESP_EXT1_WAKEUP_ANY_HIGH : ESP_EXT1_WAKEUP_ANY_LOW));
#if CONFIG_BB_BUTTON_INTERNAL_PULL
    // Internal pulls in deep sleep need the RTC peripheral domain powered.
    ESP_ERROR_CHECK(esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
    if (BTN_ACTIVE_LEVEL == 0) {
        rtc_gpio_pulldown_dis(BTN_GPIO);
        rtc_gpio_pullup_en(BTN_GPIO);
    } else {
        rtc_gpio_pullup_dis(BTN_GPIO);
        rtc_gpio_pulldown_en(BTN_GPIO);
    }
#else
    rtc_gpio_pullup_dis(BTN_GPIO);
    rtc_gpio_pulldown_dis(BTN_GPIO);
#endif
}
