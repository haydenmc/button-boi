#include "led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BB_LED_WS2812
#include "led_strip.h"
#endif

static bool s_ready;
static led_color_t s_current = LED_OFF;

// Configure a pin as a driven output; release any deep-sleep hold first so
// the level doesn't glitch (set the level, then drop the hold).
static __attribute__((unused)) void drive_pin(int gpio, int level)
{
    if (gpio < 0) {
        return;
    }
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)gpio, level);
    gpio_hold_dis((gpio_num_t)gpio);
}

static __attribute__((unused)) void hold_pin(int gpio, int level)
{
    if (gpio < 0) {
        return;
    }
    gpio_hold_dis((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)gpio, level);
    gpio_hold_en((gpio_num_t)gpio);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

#if CONFIG_BB_LED_WS2812
static const char *TAG = "led";
static led_strip_handle_t s_strip;

static void hw_init(void)
{
    drive_pin(CONFIG_BB_WS2812_GPIO, 0);
    drive_pin(CONFIG_BB_WS2812_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2)); // let the LDO come up
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = CONFIG_BB_WS2812_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed");
        s_strip = NULL;
    }
}

static void hw_set(led_color_t c)
{
    if (!s_strip) {
        return;
    }
    const uint32_t b = CONFIG_BB_WS2812_BRIGHTNESS;
    uint32_t r = 0, g = 0, bl = 0;
    switch (c) {
    case LED_GREEN: g = b; break;
    case LED_BLUE: bl = b; break;
    case LED_CYAN: g = b / 2; bl = b / 2; break;
    case LED_RED: r = b; break;
    case LED_WHITE: r = g = bl = b / 3; break;
    case LED_OFF: break;
    }
    led_strip_set_pixel(s_strip, 0, r, g, bl);
    led_strip_refresh(s_strip);
}

static void hw_sleep(void)
{
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    hold_pin(CONFIG_BB_WS2812_GPIO, 0);
    hold_pin(CONFIG_BB_WS2812_PWR_GPIO, 0);
}

#elif CONFIG_BB_LED_GPIO
#ifdef CONFIG_BB_LED_ACTIVE_HIGH
#define LED_ON_LEVEL 1
#else
#define LED_ON_LEVEL 0
#endif
static void hw_init(void) { drive_pin(CONFIG_BB_LED_GPIO_NUM, !LED_ON_LEVEL); }
static void hw_set(led_color_t c)
{
    gpio_set_level((gpio_num_t)CONFIG_BB_LED_GPIO_NUM, c != LED_OFF ? LED_ON_LEVEL : !LED_ON_LEVEL);
}
static void hw_sleep(void) { hold_pin(CONFIG_BB_LED_GPIO_NUM, !LED_ON_LEVEL); }

#else
static void hw_init(void) {}
static void hw_set(led_color_t c) { (void)c; }
static void hw_sleep(void) {}
#endif

void led_set(led_color_t c)
{
    if (!s_ready) {
        if (c == LED_OFF) {
            return; // don't power up the LED just to turn it off
        }
        hw_init();
        s_ready = true;
    } else if (c == s_current) {
        return;
    }
    hw_set(c);
    s_current = c;
}

void led_flash(led_color_t c, int ms)
{
    led_set(c);
    vTaskDelay(pdMS_TO_TICKS(ms));
    led_set(LED_OFF);
}

void led_prepare_sleep(void)
{
    if (s_ready) {
        hw_set(LED_OFF);
    }
    hw_sleep();
    s_ready = false;
    s_current = LED_OFF;
}
