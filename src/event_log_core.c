#include "event_log_core.h"

#include <string.h>

#define BLANK_SEQ 0xFFFFFFFFu
#define CHUNK_RECORDS 16u // records read per flash access during scans

uint8_t evlog_crc8(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint8_t crc = 0;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool evlog_record_valid(const evlog_record_t *r)
{
    if (r->seq == 0 || r->seq == BLANK_SEQ) {
        return false;
    }
    return evlog_crc8(r, offsetof(evlog_record_t, crc)) == r->crc;
}

static bool record_blank(const evlog_record_t *r)
{
    const uint8_t *p = (const uint8_t *)r;
    for (size_t i = 0; i < sizeof(*r); i++) {
        if (p[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static uint32_t n_sectors(const evlog_t *log)
{
    return log->flash.size / EVLOG_SECTOR_SIZE;
}

static int read_records(evlog_t *log, uint32_t off, evlog_record_t *buf, uint32_t n)
{
    return log->flash.read(log->flash.ctx, off, buf, n * EVLOG_RECORD_SIZE);
}

static int read_record(evlog_t *log, uint32_t off, evlog_record_t *r)
{
    return read_records(log, off, r, 1);
}

// Count valid records in the sector at sec_off; report whether it is blank.
static int sector_stats(evlog_t *log, uint32_t sec_off, uint32_t *valid, bool *blank)
{
    evlog_record_t buf[CHUNK_RECORDS];
    *valid = 0;
    *blank = true;
    for (uint32_t i = 0; i < EVLOG_RECORDS_PER_SECTOR; i += CHUNK_RECORDS) {
        if (read_records(log, sec_off + i * EVLOG_RECORD_SIZE, buf, CHUNK_RECORDS)) {
            return -1;
        }
        for (uint32_t j = 0; j < CHUNK_RECORDS; j++) {
            if (!record_blank(&buf[j])) {
                *blank = false;
            }
            if (evlog_record_valid(&buf[j])) {
                (*valid)++;
            }
        }
    }
    return 0;
}

int evlog_scan(evlog_t *log)
{
    evlog_state_t *st = &log->st;
    const uint32_t ns = n_sectors(log);
    if (ns < 2 || log->flash.size % EVLOG_SECTOR_SIZE) {
        return -1;
    }
    memset(st, 0, sizeof(*st));

    uint32_t max_seq = 0, min_seq = BLANK_SEQ, count = 0;
    uint32_t head_sec = 0;
    evlog_record_t buf[CHUNK_RECORDS];

    // Pass 1: count records, find the oldest/newest and the newest's sector.
    for (uint32_t off = 0; off < log->flash.size; off += CHUNK_RECORDS * EVLOG_RECORD_SIZE) {
        if (read_records(log, off, buf, CHUNK_RECORDS)) {
            return -1;
        }
        for (uint32_t j = 0; j < CHUNK_RECORDS; j++) {
            const evlog_record_t *r = &buf[j];
            if (!evlog_record_valid(r)) {
                continue;
            }
            count++;
            if (r->seq < min_seq) {
                min_seq = r->seq;
            }
            if (r->seq > max_seq) {
                max_seq = r->seq;
                head_sec = (off + j * EVLOG_RECORD_SIZE) / EVLOG_SECTOR_SIZE;
                st->last_session = r->session;
                st->has_session = 1;
            }
        }
    }

    if (count == 0) {
        st->head = 0;
        st->next_seq = 1;
        return 0;
    }

    // Pass 2: head = slot after the last non-blank slot in the newest sector.
    // Non-blank-but-invalid slots (torn writes) are skipped, never rewritten.
    const uint32_t sec_off = head_sec * EVLOG_SECTOR_SIZE;
    uint32_t last_used = 0;
    for (uint32_t i = 0; i < EVLOG_RECORDS_PER_SECTOR; i += CHUNK_RECORDS) {
        if (read_records(log, sec_off + i * EVLOG_RECORD_SIZE, buf, CHUNK_RECORDS)) {
            return -1;
        }
        for (uint32_t j = 0; j < CHUNK_RECORDS; j++) {
            if (!record_blank(&buf[j])) {
                last_used = i + j;
            }
        }
    }
    st->head = (sec_off + (last_used + 1) * EVLOG_RECORD_SIZE) % log->flash.size;
    st->next_seq = max_seq + 1;
    st->first_seq = min_seq;
    st->count = count;
    return 0;
}

bool evlog_state_plausible(evlog_t *log)
{
    const evlog_state_t *st = &log->st;
    if (st->head >= log->flash.size || st->head % EVLOG_RECORD_SIZE || st->next_seq == 0 ||
        st->next_seq == BLANK_SEQ || st->count == 0 || st->count > st->next_seq) {
        return false;
    }
    evlog_record_t r;
    // The slot at head must be writable, unless head sits on a sector boundary
    // (that sector gets erased on the next append).
    if (st->head % EVLOG_SECTOR_SIZE) {
        if (read_record(log, st->head, &r) || !record_blank(&r)) {
            return false;
        }
    }
    // The slot before head should be the newest record.
    const uint32_t prev = (st->head + log->flash.size - EVLOG_RECORD_SIZE) % log->flash.size;
    if (read_record(log, prev, &r) || !evlog_record_valid(&r) || r.seq != st->next_seq - 1) {
        return false;
    }
    return true;
}

// Called when head is at a sector boundary: make that sector writable.
static int recycle_sector(evlog_t *log)
{
    evlog_state_t *st = &log->st;
    uint32_t lost;
    bool blank;
    if (sector_stats(log, st->head, &lost, &blank)) {
        return -1;
    }
    if (blank) {
        return 0;
    }
    if (log->flash.erase_sector(log->flash.ctx, st->head)) {
        return -1;
    }
    st->count = lost > st->count ? 0 : st->count - lost;
    st->first_seq = 0;
    if (st->count == 0) {
        return 0;
    }
    // New oldest record: first valid one after the erased sector.
    const uint32_t ns = n_sectors(log);
    const uint32_t sec = st->head / EVLOG_SECTOR_SIZE;
    for (uint32_t k = 1; k < ns && !st->first_seq; k++) {
        const uint32_t s_off = ((sec + k) % ns) * EVLOG_SECTOR_SIZE;
        for (uint32_t i = 0; i < EVLOG_RECORDS_PER_SECTOR; i++) {
            evlog_record_t r;
            if (read_record(log, s_off + i * EVLOG_RECORD_SIZE, &r)) {
                return -1;
            }
            if (evlog_record_valid(&r)) {
                st->first_seq = r.seq;
                break;
            }
        }
    }
    return 0;
}

int evlog_append(evlog_t *log, uint8_t type_flags, uint32_t t, uint32_t aux,
                 uint16_t session, evlog_record_t *out)
{
    evlog_state_t *st = &log->st;
    if (st->next_seq == 0 || st->next_seq == BLANK_SEQ) {
        return -1;
    }
    if (st->head % EVLOG_SECTOR_SIZE == 0 && recycle_sector(log)) {
        return -1;
    }

    evlog_record_t r = {
        .seq = st->next_seq,
        .t = t,
        .aux = aux,
        .session = session,
        .type_flags = type_flags,
    };
    r.crc = evlog_crc8(&r, offsetof(evlog_record_t, crc));

    const uint32_t off = st->head;
    // Advance even if the write fails: a partially-programmed slot can't be reused.
    st->head = (st->head + EVLOG_RECORD_SIZE) % log->flash.size;
    st->next_seq++;

    evlog_record_t check;
    if (log->flash.write(log->flash.ctx, off, &r, sizeof(r)) || read_record(log, off, &check) ||
        memcmp(&check, &r, sizeof(r)) != 0) {
        return -1;
    }

    if (st->count == 0) {
        st->first_seq = r.seq;
    }
    st->count++;
    st->last_session = session;
    st->has_session = 1;
    if (out) {
        *out = r;
    }
    return 0;
}

int evlog_erase_all(evlog_t *log)
{
    for (uint32_t off = 0; off < log->flash.size; off += EVLOG_SECTOR_SIZE) {
        if (log->flash.erase_sector(log->flash.ctx, off)) {
            return -1;
        }
    }
    log->st.head = 0;
    log->st.first_seq = 0;
    log->st.count = 0;
    return 0;
}

void evlog_iter_init(const evlog_t *log, evlog_iter_t *it)
{
    // Oldest data starts at the sector following the one holding the newest
    // record (which is the sector at head when head is on a boundary).
    const uint32_t size = log->flash.size;
    const uint32_t newest = (log->st.head + size - EVLOG_RECORD_SIZE) % size;
    const uint32_t newest_sec = newest / EVLOG_SECTOR_SIZE;
    it->pos = ((newest_sec + 1) % n_sectors(log)) * EVLOG_SECTOR_SIZE;
    it->remaining = log->st.count ? size : 0;
}

int evlog_iter_next(evlog_t *log, evlog_iter_t *it, evlog_record_t *out)
{
    while (it->remaining) {
        evlog_record_t r;
        if (read_record(log, it->pos, &r)) {
            return -1;
        }
        it->pos = (it->pos + EVLOG_RECORD_SIZE) % log->flash.size;
        it->remaining -= EVLOG_RECORD_SIZE;
        if (evlog_record_valid(&r)) {
            *out = r;
            return 1;
        }
    }
    return 0;
}
