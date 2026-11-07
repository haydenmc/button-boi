#include "event_log.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "evlog";

#define EVLOG_PARTITION_SUBTYPE 0x40
#define RTC_CACHE_MAGIC 0xB0B1E701u

// Survives deep sleep; re-initialised on power-on.
static RTC_DATA_ATTR struct {
    uint32_t magic;
    uint32_t part_size;
    evlog_state_t st;
} s_rtc_cache;

static evlog_t s_log;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static int p_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    return esp_partition_read(ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return esp_partition_write(ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int p_erase(void *ctx, uint32_t off)
{
    return esp_partition_erase_range(ctx, off, EVLOG_SECTOR_SIZE) == ESP_OK ? 0 : -1;
}

static void save_cache(void)
{
    s_rtc_cache.st = s_log.st;
    s_rtc_cache.part_size = s_log.flash.size;
    s_rtc_cache.magic = RTC_CACHE_MAGIC;
}

esp_err_t event_log_open(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, EVLOG_PARTITION_SUBTYPE, "evlog");
    if (!part) {
        ESP_LOGE(TAG, "evlog partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    s_log = (evlog_t){
        .flash = {
            .read = p_read,
            .write = p_write,
            .erase_sector = p_erase,
            .ctx = (void *)part,
            .size = part->size - part->size % EVLOG_SECTOR_SIZE,
        },
    };

    if (s_rtc_cache.magic == RTC_CACHE_MAGIC && s_rtc_cache.part_size == s_log.flash.size) {
        s_log.st = s_rtc_cache.st;
        if (evlog_state_plausible(&s_log)) {
            ESP_LOGD(TAG, "state restored from RTC cache");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "RTC cache stale, rescanning");
    }

    if (evlog_scan(&s_log) != 0) {
        ESP_LOGE(TAG, "scan failed");
        s_rtc_cache.magic = 0;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "scanned: %lu records (seq %lu..%lu), capacity %lu",
             (unsigned long)s_log.st.count, (unsigned long)s_log.st.first_seq,
             (unsigned long)(s_log.st.next_seq - 1), (unsigned long)evlog_capacity(&s_log));
    save_cache();
    return ESP_OK;
}

esp_err_t event_log_append(uint8_t type_flags, uint32_t t, uint32_t aux, uint16_t session)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    evlog_record_t r;
    int rc = evlog_append(&s_log, type_flags, t, aux, session, &r);
    save_cache();
    xSemaphoreGive(s_lock);
    if (rc != 0) {
        ESP_LOGE(TAG, "append failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "#%lu type=%u flags=0x%02x t=%lu aux=%lu session=%u", (unsigned long)r.seq,
             r.type_flags & EVLOG_TYPE_MASK, r.type_flags & ~EVLOG_TYPE_MASK, (unsigned long)r.t,
             (unsigned long)r.aux, r.session);
    return ESP_OK;
}

esp_err_t event_log_erase(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int rc = evlog_erase_all(&s_log);
    save_cache();
    xSemaphoreGive(s_lock);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void event_log_get_info(event_log_info_t *info)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    info->first_seq = s_log.st.first_seq;
    info->last_seq = s_log.st.count ? s_log.st.next_seq - 1 : 0;
    info->count = s_log.st.count;
    info->capacity = evlog_capacity(&s_log);
    info->last_session = s_log.st.last_session;
    info->has_session = s_log.st.has_session;
    xSemaphoreGive(s_lock);
}

void event_log_iter_init(evlog_iter_t *it)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    evlog_iter_init(&s_log, it);
    xSemaphoreGive(s_lock);
}

int event_log_iter_next(evlog_iter_t *it, uint32_t from_seq, evlog_record_t *out)
{
    int rc;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    do {
        rc = evlog_iter_next(&s_log, it, out);
    } while (rc == 1 && out->seq < from_seq);
    xSemaphoreGive(s_lock);
    return rc;
}
