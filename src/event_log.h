// ESP32 front-end for the flash ring log (event_log_core.*). Thread-safe.
#pragma once

#include "esp_err.h"
#include "event_log_core.h"

// Open the "evlog" partition. Restores state cached in RTC RAM when it is
// consistent with flash (fast path on deep-sleep wake), otherwise rescans.
esp_err_t event_log_open(void);

esp_err_t event_log_append(uint8_t type_flags, uint32_t t, uint32_t aux, uint16_t session);
esp_err_t event_log_erase(void);

// Snapshot of counters for the BLE Info characteristic.
typedef struct {
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t count;
    uint32_t capacity;
    uint16_t last_session;
    bool has_session;
} event_log_info_t;
void event_log_get_info(event_log_info_t *info);

// Iterate records with seq >= from_seq, oldest first.
// Returns 1 = record, 0 = end, <0 = error.
void event_log_iter_init(evlog_iter_t *it);
int event_log_iter_next(evlog_iter_t *it, uint32_t from_seq, evlog_record_t *out);
