// Wall-clock and session bookkeeping.
//
// The ESP32-S3 RTC timer keeps system time running through deep sleep, but
// it resets on power loss. Until a client sets the clock over BLE, time is
// "invalid" and records hold seconds-since-power-on for the current session.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>

// Returns true when RTC state was lost (power-on or reset): a new session
// must be started with timekeep_begin_session().
bool timekeep_init(void);
void timekeep_begin_session(uint16_t session);

uint16_t timekeep_session(void);
bool timekeep_is_valid(void);
void timekeep_now(struct timeval *tv);

// Set wall-clock time. `before` receives the device clock just before.
void timekeep_set_epoch_ms(uint64_t epoch_ms, struct timeval *before);

static inline uint64_t timeval_to_ms(const struct timeval *tv)
{
    return (uint64_t)tv->tv_sec * 1000u + (uint64_t)tv->tv_usec / 1000u;
}
