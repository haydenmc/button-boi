// BLE "sync" GATT server (NimBLE). Only runs during a sync window.
//
// Service 6b1f0001-3c5e-4a7d-9b2e-8f1c0d2a7e51. All integers little-endian.
//
//   Info    6b1f0002-…  read     u8 proto=1, u8 flags (bit0 = time valid),
//                                u16 session, u64 device_time_ms,
//                                u32 first_seq, u32 last_seq, u32 count,
//                                u32 capacity                    (28 bytes)
//   Time    6b1f0003-…  read     u64 device_time_ms, u8 valid
//                       write    u64 epoch_ms → sets clock, logs TIME_SYNC
//   Control 6b1f0004-…  write    0x01 u32 from_seq  → stream records ≥ from_seq
//                                0x02 "WIPE"        → erase the log
//   Data    6b1f0005-…  notify   N × 16-byte evlog_record_t, then a final
//                                5-byte {0xFF, u32 records_sent}
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t ble_sync_start(void);
void ble_sync_stop(void);
bool ble_sync_connected(void);
