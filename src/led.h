// Indicator LED: none, plain GPIO, or WS2812 RGB (selected in Kconfig).
// A plain LED lights for any colour other than LED_OFF.
#pragma once

typedef enum {
    LED_OFF = 0,
    LED_GREEN, // press recorded
    LED_BLUE,  // sync mode / advertising
    LED_CYAN,  // client connected
    LED_RED,   // error
    LED_WHITE, // power-on
} led_color_t;

void led_set(led_color_t c);
void led_flash(led_color_t c, int ms);
// Turn the LED (and its power rail) off and hold the pins through deep sleep.
void led_prepare_sleep(void);
