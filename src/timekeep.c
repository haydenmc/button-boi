#include "timekeep.h"

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "time";

#define RTC_MAGIC 0x71AE0001u

static RTC_DATA_ATTR struct {
    uint32_t magic;
    uint16_t session;
    bool valid;
} s_rtc;

bool timekeep_init(void)
{
    return s_rtc.magic != RTC_MAGIC;
}

void timekeep_begin_session(uint16_t session)
{
    s_rtc.session = session;
    s_rtc.valid = false;
    s_rtc.magic = RTC_MAGIC;
}

uint16_t timekeep_session(void)
{
    return s_rtc.session;
}

bool timekeep_is_valid(void)
{
    return s_rtc.valid;
}

void timekeep_now(struct timeval *tv)
{
    gettimeofday(tv, NULL);
}

void timekeep_set_epoch_ms(uint64_t epoch_ms, struct timeval *before)
{
    gettimeofday(before, NULL);
    struct timeval tv = {
        .tv_sec = (time_t)(epoch_ms / 1000u),
        .tv_usec = (suseconds_t)((epoch_ms % 1000u) * 1000u),
    };
    settimeofday(&tv, NULL);
    s_rtc.valid = true;
    ESP_LOGI(TAG, "clock set: %lld -> %lld (delta %lld ms)", (long long)before->tv_sec,
             (long long)tv.tv_sec, (long long)(epoch_ms - timeval_to_ms(before)));
}
