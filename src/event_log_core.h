// Append-only ring log of fixed 16-byte records on NOR flash.
//
// Pure C with no ESP-IDF dependencies so it can be unit-tested on the host
// (see test/test_event_log). Flash access goes through evlog_flash_t.
//
// Layout: the region is split into 4 KB erase sectors. Records are written
// sequentially; when the write head crosses into a sector that still holds
// old data, that whole sector is erased first (oldest data is overwritten).
// Each record carries a monotonically increasing sequence number and a CRC-8,
// so the head can be recovered by scanning after power loss, and torn writes
// are detected and skipped.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVLOG_RECORD_SIZE 16u
#define EVLOG_SECTOR_SIZE 4096u
#define EVLOG_RECORDS_PER_SECTOR (EVLOG_SECTOR_SIZE / EVLOG_RECORD_SIZE)

// Record types (low nibble of type_flags).
enum {
    EVLOG_TYPE_BOOT = 1,      // power-on / reset. t = device clock, aux = reset reason
    EVLOG_TYPE_PRESS = 2,     // button press.     t = seconds, aux = milliseconds
    EVLOG_TYPE_TIME_SYNC = 3, // clock set.        t = new epoch s, aux = device clock s just before
    EVLOG_TYPE_ERASE = 4,     // log was erased.   t = device clock
};
#define EVLOG_TYPE_MASK 0x0Fu
// Flag: t is Unix epoch seconds. If clear, t is seconds since this session's
// power-on. For TIME_SYNC it describes the *previous* clock (aux).
#define EVLOG_FLAG_TIME_VALID 0x80u

typedef struct __attribute__((packed)) {
    uint32_t seq;  // 1.., never 0 or 0xFFFFFFFF
    uint32_t t;
    uint32_t aux;
    uint16_t session; // increments on every power-on / reset (not deep-sleep wake)
    uint8_t type_flags;
    uint8_t crc; // CRC-8 (poly 0x07) over the preceding 15 bytes
} evlog_record_t;

_Static_assert(sizeof(evlog_record_t) == EVLOG_RECORD_SIZE, "record size");

// Flash backend. Functions return 0 on success. `write` has NOR semantics
// (bits can only be cleared); `erase_sector` sets a 4 KB sector to 0xFF.
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    int (*erase_sector)(void *ctx, uint32_t off);
    void *ctx;
    uint32_t size; // bytes; multiple of EVLOG_SECTOR_SIZE, at least 2 sectors
} evlog_flash_t;

// Everything needed to resume without rescanning (cached in RTC RAM on ESP32).
typedef struct {
    uint32_t head;      // byte offset of the next slot to write
    uint32_t next_seq;  // sequence number of the next record
    uint32_t first_seq; // oldest valid record present (0 if empty)
    uint32_t count;     // number of valid records present
    uint16_t last_session;
    uint8_t has_session; // last_session is meaningful
    uint8_t _pad;
} evlog_state_t;

typedef struct {
    evlog_flash_t flash;
    evlog_state_t st;
} evlog_t;

typedef struct {
    uint32_t pos;       // next offset to read
    uint32_t remaining; // bytes left to visit
} evlog_iter_t;

uint8_t evlog_crc8(const void *data, size_t len);
bool evlog_record_valid(const evlog_record_t *r);

// Rebuild state by scanning the whole region. Returns 0 on success.
int evlog_scan(evlog_t *log);

// Cheap consistency check of a cached log->st against flash.
bool evlog_state_plausible(evlog_t *log);

// Append a record. `out` (optional) receives the written record.
int evlog_append(evlog_t *log, uint8_t type_flags, uint32_t t, uint32_t aux,
                 uint16_t session, evlog_record_t *out);

// Erase every sector. Sequence numbering and session continue.
int evlog_erase_all(evlog_t *log);

// Iterate valid records oldest → newest.
void evlog_iter_init(const evlog_t *log, evlog_iter_t *it);
// Returns 1 and fills *out when a record is found, 0 at end, <0 on error.
int evlog_iter_next(evlog_t *log, evlog_iter_t *it, evlog_record_t *out);

static inline uint32_t evlog_capacity(const evlog_t *log)
{
    // One sector is always being recycled, so it can't be counted on.
    return (log->flash.size / EVLOG_SECTOR_SIZE - 1) * EVLOG_RECORDS_PER_SECTOR;
}

#ifdef __cplusplus
}
#endif
