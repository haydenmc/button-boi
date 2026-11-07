// button-boi: press a button, log a timestamp, go back to deep sleep.
//
//   short press  → wake, append PRESS record to flash, green flash, sleep
//   long press   → BLE sync window (advertise; client sets clock / dumps log)
//   power-on     → start a new session, append BOOT record, sleep
//
// Everything runs from app_main on each wake; deep sleep resets the CPU, so
// state that must survive lives in flash (the log) or RTC RAM (caches).
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ble_sync.h"
#include "button.h"
#include "event_log.h"
#include "led.h"
#include "timekeep.h"

static const char *TAG = "main";

typedef enum {
    WAKE_POWER_ON,
    WAKE_BUTTON,
    WAKE_TIMER, // re-check after sleeping with the button stuck down
} wake_t;

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void record(uint8_t type, const struct timeval *tv, uint32_t aux)
{
    uint8_t flags = timekeep_is_valid() ? EVLOG_FLAG_TIME_VALID : 0;
    if (event_log_append(type | flags, (uint32_t)tv->tv_sec, aux, timekeep_session()) != ESP_OK) {
        led_flash(LED_RED, 500);
    }
}

static void record_press(const struct timeval *tv)
{
    record(EVLOG_TYPE_PRESS, tv, (uint32_t)(tv->tv_usec / 1000));
}

// When the current press started (uptime ms). 0 after a deep-sleep wake, so
// the boot latency counts towards the long-press time.
static int64_t s_press_start_ms;

// Wait until the button is released or the long-press time has elapsed.
// Returns true for a long press.
static bool wait_short_or_long(void)
{
    while (button_pressed()) {
        if (uptime_ms() - s_press_start_ms >= CONFIG_BB_LONG_PRESS_MS) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

static void run_sync_mode(void)
{
    ESP_LOGI(TAG, "sync mode for %d s", CONFIG_BB_SYNC_WINDOW_S);
    if (ble_sync_start() != ESP_OK) {
        led_flash(LED_RED, 1000);
        return;
    }
    const int64_t start = uptime_ms();
    int64_t deadline = start + CONFIG_BB_SYNC_WINDOW_S * 1000LL;

    // The hold that started sync mode must be released before presses count.
    bool was_down = button_pressed();
    bool armed = !was_down;
    int64_t down_since = uptime_ms();
    struct timeval press_tv = {0};

    for (;;) {
        const int64_t now = uptime_ms();
        const bool connected = ble_sync_connected();
        if (connected) {
            const int64_t idle_end = now + CONFIG_BB_SYNC_IDLE_AFTER_DISCONNECT_S * 1000LL;
            if (idle_end > deadline) {
                deadline = idle_end;
            }
        }
        if (now >= deadline || now - start >= CONFIG_BB_SYNC_MAX_S * 1000LL) {
            break;
        }

        led_set(connected ? LED_CYAN : ((now / 500) % 2 ? LED_BLUE : LED_OFF));

        // Presses while syncing are still logged; a long press ends sync early.
        const bool down = button_pressed();
        if (down && !was_down) {
            down_since = now;
            timekeep_now(&press_tv);
        } else if (!down && was_down) {
            const int64_t held = now - down_since;
            if (armed && held >= CONFIG_BB_DEBOUNCE_MS && held < CONFIG_BB_LONG_PRESS_MS) {
                record_press(&press_tv);
                led_flash(LED_GREEN, 150);
            }
            armed = true;
        } else if (down && armed && now - down_since >= CONFIG_BB_LONG_PRESS_MS) {
            ESP_LOGI(TAG, "long press: leaving sync mode");
            break;
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ble_sync_stop();
    led_set(LED_OFF);
}

static void handle_wake(wake_t wake, const struct timeval *wake_tv)
{
    switch (wake) {
    case WAKE_POWER_ON:
        led_flash(LED_WHITE, 100);
        break;
    case WAKE_BUTTON:
        // The press that woke us may already be over by the time we run; the
        // EXT1 wake itself is the evidence of a press, so it always counts
        // unless it turns into a long press.
        if (wait_short_or_long()) {
            led_set(LED_BLUE); // tell the user they can let go
            run_sync_mode();
        } else {
            record_press(wake_tv);
            led_flash(LED_GREEN, 150);
        }
        break;
    case WAKE_TIMER:
        break;
    }
}

// Deep sleep until the next press. Only returns in BB_DEBUG_NO_SLEEP builds.
static wake_t sleep_until_next_event(void)
{
    led_prepare_sleep();

#if CONFIG_BB_DEBUG_NO_SLEEP
    ESP_LOGI(TAG, "(debug) would deep sleep now; polling button");
    while (button_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (!button_pressed()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_press_start_ms = uptime_ms();
    return WAKE_BUTTON;
#else
    // EXT1 is level-triggered: if the button is still down we would wake
    // immediately. Give it a moment, then fall back to a timer wake.
    for (int i = 0; i < 200 && button_pressed(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (button_pressed()) {
        ESP_LOGW(TAG, "button held; sleeping %d s on timer", CONFIG_BB_STUCK_BUTTON_RETRY_S);
        esp_sleep_enable_timer_wakeup(CONFIG_BB_STUCK_BUTTON_RETRY_S * 1000000ULL);
    } else {
        button_enable_wake();
    }
    ESP_LOGI(TAG, "deep sleep (awake %lld ms)", uptime_ms());
    esp_deep_sleep_start();
#endif
}

static wake_t wake_reason(void)
{
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & BIT(ESP_SLEEP_WAKEUP_EXT1)) {
        return WAKE_BUTTON;
    }
    if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
        // Woken to re-check a stuck button. If it is now released, there is
        // nothing to record; a new press will wake us via EXT1.
        return WAKE_TIMER;
    }
    return WAKE_POWER_ON;
}

void app_main(void)
{
    // Capture the press time before anything else.
    struct timeval wake_tv;
    timekeep_now(&wake_tv);

    button_init();
    wake_t wake = wake_reason();

    if (event_log_open() != ESP_OK) {
        led_flash(LED_RED, 2000);
    }

    if (timekeep_init()) {
        // RTC RAM was lost: power-on or reset. Start a new session.
        event_log_info_t li;
        event_log_get_info(&li);
        const uint16_t session = li.has_session ? (uint16_t)(li.last_session + 1) : 1;
        timekeep_begin_session(session);
        timekeep_now(&wake_tv);
        record(EVLOG_TYPE_BOOT, &wake_tv, (uint32_t)esp_reset_reason());
        ESP_LOGI(TAG, "new session %u", session);
        if (wake != WAKE_BUTTON) {
            wake = WAKE_POWER_ON;
        }
    }

    for (;;) {
        handle_wake(wake, &wake_tv);
        wake = sleep_until_next_event();
        timekeep_now(&wake_tv);
    }
}
