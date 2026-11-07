// Host tests for the flash ring log: `pio test -e native`
#include <string.h>
#include <unity.h>

#include "event_log_core.h"

#define N_SECTORS 4
#define FLASH_SIZE (N_SECTORS * EVLOG_SECTOR_SIZE)

static uint8_t flash[FLASH_SIZE];
static int erase_count;

static int f_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(off + len <= FLASH_SIZE);
    memcpy(buf, flash + off, len);
    return 0;
}

// NOR semantics: programming can only clear bits.
static int f_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(off + len <= FLASH_SIZE);
    const uint8_t *src = buf;
    for (size_t i = 0; i < len; i++) {
        flash[off + i] &= src[i];
    }
    return 0;
}

static int f_erase(void *ctx, uint32_t off)
{
    (void)ctx;
    TEST_ASSERT_EQUAL_UINT32(0, off % EVLOG_SECTOR_SIZE);
    memset(flash + off, 0xFF, EVLOG_SECTOR_SIZE);
    erase_count++;
    return 0;
}

static evlog_t make_log(void)
{
    evlog_t log = {.flash = {.read = f_read, .write = f_write, .erase_sector = f_erase, .size = FLASH_SIZE}};
    return log;
}

void setUp(void)
{
    memset(flash, 0xFF, sizeof(flash));
    erase_count = 0;
}

void tearDown(void) {}

static void append_n(evlog_t *log, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT(0, evlog_append(log, EVLOG_TYPE_PRESS, 1000 + log->st.next_seq, 0, 7, NULL));
    }
}

// Walk the log, checking order, and return how many records were seen.
static uint32_t walk(evlog_t *log, uint32_t *first, uint32_t *last)
{
    evlog_iter_t it;
    evlog_record_t r;
    uint32_t n = 0, prev = 0;
    evlog_iter_init(log, &it);
    int rc;
    while ((rc = evlog_iter_next(log, &it, &r)) == 1) {
        if (n == 0 && first) {
            *first = r.seq;
        }
        TEST_ASSERT_TRUE_MESSAGE(r.seq > prev, "records out of order");
        prev = r.seq;
        n++;
    }
    TEST_ASSERT_EQUAL_INT(0, rc);
    if (last) {
        *last = prev;
    }
    return n;
}

static void test_empty(void)
{
    evlog_t log = make_log();
    TEST_ASSERT_EQUAL_INT(0, evlog_scan(&log));
    TEST_ASSERT_EQUAL_UINT32(0, log.st.count);
    TEST_ASSERT_EQUAL_UINT32(1, log.st.next_seq);
    TEST_ASSERT_EQUAL_UINT32(0, log.st.head);
    TEST_ASSERT_FALSE(log.st.has_session);
    TEST_ASSERT_EQUAL_UINT32(0, walk(&log, NULL, NULL));
}

static void test_append_and_rescan(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, 10);
    evlog_state_t before = log.st;

    evlog_t again = make_log();
    TEST_ASSERT_EQUAL_INT(0, evlog_scan(&again));
    TEST_ASSERT_EQUAL_MEMORY(&before, &again.st, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(10, again.st.count);
    TEST_ASSERT_EQUAL_UINT32(1, again.st.first_seq);
    TEST_ASSERT_EQUAL_UINT32(11, again.st.next_seq);
    TEST_ASSERT_EQUAL_UINT16(7, again.st.last_session);
    TEST_ASSERT_TRUE(evlog_state_plausible(&again));

    uint32_t first, last;
    TEST_ASSERT_EQUAL_UINT32(10, walk(&again, &first, &last));
    TEST_ASSERT_EQUAL_UINT32(1, first);
    TEST_ASSERT_EQUAL_UINT32(10, last);
}

static void test_record_contents(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    evlog_record_t w;
    TEST_ASSERT_EQUAL_INT(0, evlog_append(&log, EVLOG_TYPE_PRESS | EVLOG_FLAG_TIME_VALID, 1762560000, 123, 42, &w));
    evlog_iter_t it;
    evlog_record_t r;
    evlog_iter_init(&log, &it);
    TEST_ASSERT_EQUAL_INT(1, evlog_iter_next(&log, &it, &r));
    TEST_ASSERT_EQUAL_MEMORY(&w, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT32(1762560000, r.t);
    TEST_ASSERT_EQUAL_UINT32(123, r.aux);
    TEST_ASSERT_EQUAL_UINT16(42, r.session);
    TEST_ASSERT_EQUAL_UINT8(EVLOG_TYPE_PRESS | EVLOG_FLAG_TIME_VALID, r.type_flags);
}

static void test_exact_sector_fill(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, EVLOG_RECORDS_PER_SECTOR);
    TEST_ASSERT_EQUAL_UINT32(EVLOG_SECTOR_SIZE, log.st.head);

    evlog_t again = make_log();
    evlog_scan(&again);
    TEST_ASSERT_EQUAL_UINT32(EVLOG_SECTOR_SIZE, again.st.head);
    TEST_ASSERT_TRUE(evlog_state_plausible(&again));
    append_n(&again, 1);
    TEST_ASSERT_EQUAL_UINT32(EVLOG_RECORDS_PER_SECTOR + 1, walk(&again, NULL, NULL));
}

static void test_wraparound(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    const uint32_t total = N_SECTORS * EVLOG_RECORDS_PER_SECTOR * 2 + 37;
    append_n(&log, total);

    uint32_t first, last;
    uint32_t n = walk(&log, &first, &last);
    TEST_ASSERT_EQUAL_UINT32(log.st.count, n);
    TEST_ASSERT_EQUAL_UINT32(log.st.first_seq, first);
    TEST_ASSERT_EQUAL_UINT32(total, last);
    TEST_ASSERT_EQUAL_UINT32(total - first + 1, n); // contiguous
    TEST_ASSERT_TRUE(n >= evlog_capacity(&log));

    evlog_t again = make_log();
    TEST_ASSERT_EQUAL_INT(0, evlog_scan(&again));
    TEST_ASSERT_EQUAL_MEMORY(&log.st, &again.st, sizeof(log.st));
}

static void test_torn_write_skipped(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, 5);
    // Simulate power loss mid-write of record 6: a few bytes programmed.
    const uint8_t partial[6] = {0x06, 0x00, 0x00, 0x00, 0x12, 0x34};
    f_write(NULL, 5 * EVLOG_RECORD_SIZE, partial, sizeof(partial));

    evlog_t again = make_log();
    TEST_ASSERT_EQUAL_INT(0, evlog_scan(&again));
    TEST_ASSERT_EQUAL_UINT32(5, again.st.count);
    TEST_ASSERT_EQUAL_UINT32(6, again.st.next_seq);
    TEST_ASSERT_EQUAL_UINT32(6 * EVLOG_RECORD_SIZE, again.st.head); // corrupt slot skipped
    append_n(&again, 3);
    uint32_t last;
    TEST_ASSERT_EQUAL_UINT32(8, walk(&again, NULL, &last));
    TEST_ASSERT_EQUAL_UINT32(8, last);
}

static void test_torn_first_record_of_sector(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, EVLOG_RECORDS_PER_SECTOR);
    const uint8_t partial[3] = {0x01, 0x01, 0x00};
    f_write(NULL, EVLOG_SECTOR_SIZE, partial, sizeof(partial));

    evlog_t again = make_log();
    evlog_scan(&again);
    TEST_ASSERT_EQUAL_UINT32(EVLOG_SECTOR_SIZE, again.st.head);
    append_n(&again, 1); // must erase the dirty sector first
    TEST_ASSERT_EQUAL_UINT32(EVLOG_RECORDS_PER_SECTOR + 1, walk(&again, NULL, NULL));
}

static void test_plausible_detects_stale_cache(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, 3);
    evlog_state_t cached = log.st;
    append_n(&log, 1); // flash moved on without the cache

    evlog_t stale = make_log();
    stale.st = cached;
    TEST_ASSERT_FALSE(evlog_state_plausible(&stale));
    stale.st.head = 12345; // misaligned / garbage
    TEST_ASSERT_FALSE(evlog_state_plausible(&stale));
}

static void test_erase_all(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, 300);
    TEST_ASSERT_EQUAL_INT(0, evlog_erase_all(&log));
    TEST_ASSERT_EQUAL_UINT32(0, log.st.count);
    TEST_ASSERT_EQUAL_UINT32(0, walk(&log, NULL, NULL));
    // Sequence numbers keep increasing after an erase.
    TEST_ASSERT_EQUAL_INT(0, evlog_append(&log, EVLOG_TYPE_ERASE, 0, 0, 7, NULL));
    TEST_ASSERT_EQUAL_UINT32(301, log.st.first_seq);
    TEST_ASSERT_EQUAL_UINT32(1, walk(&log, NULL, NULL));
}

static void test_no_erase_on_blank_flash(void)
{
    evlog_t log = make_log();
    evlog_scan(&log);
    append_n(&log, (N_SECTORS - 1) * EVLOG_RECORDS_PER_SECTOR + 1);
    TEST_ASSERT_EQUAL_INT(0, erase_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_append_and_rescan);
    RUN_TEST(test_record_contents);
    RUN_TEST(test_exact_sector_fill);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_torn_write_skipped);
    RUN_TEST(test_torn_first_record_of_sector);
    RUN_TEST(test_plausible_detects_stale_cache);
    RUN_TEST(test_erase_all);
    RUN_TEST(test_no_erase_on_blank_flash);
    return UNITY_END();
}
