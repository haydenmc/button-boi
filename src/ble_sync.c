#include "ble_sync.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "battery.h"
#include "event_log.h"
#include "timekeep.h"

static const char *TAG = "ble";

#define PROTO_VERSION 1
#define OP_DUMP 0x01
#define OP_ERASE 0x02
#define DUMP_END_MARKER 0xFF
#define MAX_RECORDS_PER_NOTIFY 15

// Common-profile ATT error codes (Core Supplement Part B 1.2) not defined by NimBLE.
#define ATT_ERR_CCCD_IMPROPER 0xFD
#define ATT_ERR_IN_PROGRESS 0xFE

// 6b1f00XX-3c5e-4a7d-9b2e-8f1c0d2a7e51, little-endian byte order.
#define BB_UUID(x) \
    BLE_UUID128_INIT(0x51, 0x7e, 0x2a, 0x0d, 0x1c, 0x8f, 0x2e, 0x9b, 0x7d, 0x4a, 0x5e, 0x3c, (x), 0x00, 0x1f, 0x6b)

static const ble_uuid128_t s_svc_uuid = BB_UUID(0x01);
static const ble_uuid128_t s_info_uuid = BB_UUID(0x02);
static const ble_uuid128_t s_time_uuid = BB_UUID(0x03);
static const ble_uuid128_t s_ctrl_uuid = BB_UUID(0x04);
static const ble_uuid128_t s_data_uuid = BB_UUID(0x05);

static uint16_t s_data_handle;
static uint8_t s_own_addr_type;
static char s_name[32];

static volatile bool s_active;
static volatile bool s_connected;
static volatile bool s_subscribed;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile TaskHandle_t s_dump_task;
static uint16_t s_battery_mv; // sampled once per sync window, before the radio starts

typedef struct __attribute__((packed)) {
    uint8_t proto;
    uint8_t flags;
    uint16_t session;
    uint64_t device_time_ms;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t count;
    uint32_t capacity;
    uint16_t battery_mv; // 0 = unknown
    uint8_t power_flags; // bit0 = USB power present
} info_payload_t;

typedef struct __attribute__((packed)) {
    uint64_t device_time_ms;
    uint8_t valid;
} time_payload_t;

static void advertise(void);

// ---------------------------------------------------------------- dump ----

static int notify(const void *data, size_t len)
{
    for (int tries = 0; tries < 300; tries++) {
        if (!s_connected || !s_subscribed) {
            return -1;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            // Consumes om regardless of outcome.
            int rc = ble_gatts_notify_custom(s_conn, s_data_handle, om);
            if (rc == 0) {
                return 0;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notify failed rc=%d", rc);
                return rc;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // controller buffers full; back off
    }
    return -1;
}

static void dump_task(void *arg)
{
    const uint32_t from_seq = (uint32_t)(uintptr_t)arg;
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t per = mtu > 3 ? (mtu - 3) / EVLOG_RECORD_SIZE : 1;
    if (per < 1) {
        per = 1;
    }
    if (per > MAX_RECORDS_PER_NOTIFY) {
        per = MAX_RECORDS_PER_NOTIFY;
    }
    ESP_LOGI(TAG, "dump from seq %lu (mtu %u, %u rec/notify)", (unsigned long)from_seq, mtu,
             (unsigned)per);

    static uint8_t buf[MAX_RECORDS_PER_NOTIFY * EVLOG_RECORD_SIZE];
    evlog_iter_t it;
    evlog_record_t rec;
    uint32_t sent = 0;
    bool ok = true;
    event_log_iter_init(&it);
    for (;;) {
        size_t n = 0;
        int rc;
        while (n < per && (rc = event_log_iter_next(&it, from_seq, &rec)) == 1) {
            memcpy(buf + n * EVLOG_RECORD_SIZE, &rec, EVLOG_RECORD_SIZE);
            n++;
        }
        if (n == 0) {
            break;
        }
        if (notify(buf, n * EVLOG_RECORD_SIZE) != 0) {
            ok = false;
            break;
        }
        sent += n;
    }
    if (ok) {
        uint8_t end[5] = {DUMP_END_MARKER};
        memcpy(&end[1], &sent, sizeof(sent));
        notify(end, sizeof(end));
        ESP_LOGI(TAG, "dump done: %lu records", (unsigned long)sent);
    }
    s_dump_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------- GATT ----

static uint64_t now_ms(void)
{
    struct timeval tv;
    timekeep_now(&tv);
    return timeval_to_ms(&tv);
}

static int read_flat(struct os_mbuf *om, void *dst, uint16_t min, uint16_t max, uint16_t *len)
{
    uint16_t l = OS_MBUF_PKTLEN(om);
    if (l < min || l > max) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(om, dst, max, len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return 0;
}

static int info_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    event_log_info_t li;
    event_log_get_info(&li);
    info_payload_t p = {
        .proto = PROTO_VERSION,
        .flags = timekeep_is_valid() ? 1 : 0,
        .session = timekeep_session(),
        .device_time_ms = now_ms(),
        .first_seq = li.first_seq,
        .last_seq = li.last_seq,
        .count = li.count,
        .capacity = li.capacity,
        .battery_mv = s_battery_mv,
        .power_flags = battery_usb_present() ? 1 : 0,
    };
    return os_mbuf_append(ctxt->om, &p, sizeof(p)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int time_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        time_payload_t p = {.device_time_ms = now_ms(), .valid = timekeep_is_valid()};
        return os_mbuf_append(ctxt->om, &p, sizeof(p)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint64_t epoch_ms;
    uint16_t len;
    int rc = read_flat(ctxt->om, &epoch_ms, sizeof(epoch_ms), sizeof(epoch_ms), &len);
    if (rc) {
        return rc;
    }
    if (epoch_ms < 1577836800000ull) { // reject anything before 2020
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    const bool was_valid = timekeep_is_valid();
    struct timeval before;
    timekeep_set_epoch_ms(epoch_ms, &before);
    event_log_append(EVLOG_TYPE_TIME_SYNC | (was_valid ? EVLOG_FLAG_TIME_VALID : 0),
                     (uint32_t)(epoch_ms / 1000u), (uint32_t)before.tv_sec, timekeep_session());
    return 0;
}

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[8];
    uint16_t len;
    int rc = read_flat(ctxt->om, buf, 1, sizeof(buf), &len);
    if (rc) {
        return rc;
    }
    if (s_dump_task) {
        return ATT_ERR_IN_PROGRESS;
    }
    switch (buf[0]) {
    case OP_DUMP: {
        if (len != 5) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (!s_subscribed) {
            return ATT_ERR_CCCD_IMPROPER;
        }
        uint32_t from;
        memcpy(&from, &buf[1], sizeof(from));
        TaskHandle_t h;
        if (xTaskCreate(dump_task, "dump", 4096, (void *)(uintptr_t)from, 5, &h) != pdPASS) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        s_dump_task = h;
        return 0;
    }
    case OP_ERASE: {
        if (len != 5 || memcmp(&buf[1], "WIPE", 4) != 0) {
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        ESP_LOGW(TAG, "erasing log");
        if (event_log_erase() != ESP_OK) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        struct timeval tv;
        timekeep_now(&tv);
        event_log_append(EVLOG_TYPE_ERASE | (timekeep_is_valid() ? EVLOG_FLAG_TIME_VALID : 0),
                         (uint32_t)tv.tv_sec, 0, timekeep_session());
        return 0;
    }
    default:
        return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    }
}

static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED; // notify only
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &s_info_uuid.u, .access_cb = info_access, .flags = BLE_GATT_CHR_F_READ},
            {.uuid = &s_time_uuid.u,
             .access_cb = time_access,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE},
            {.uuid = &s_ctrl_uuid.u, .access_cb = ctrl_access, .flags = BLE_GATT_CHR_F_WRITE},
            {.uuid = &s_data_uuid.u,
             .access_cb = data_access,
             .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_data_handle},
            {0},
        },
    },
    {0},
};

// ----------------------------------------------------------------- GAP ----

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_connected = false;
        s_subscribed = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_data_handle) {
            s_subscribed = ev->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %u", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_active || s_connected) {
        return;
    }
    struct ble_hs_adv_fields adv = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &s_svc_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .name = (const uint8_t *)s_name,
        .name_len = strlen(s_name),
        .name_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(200),
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    snprintf(s_name, sizeof(s_name), "%s-%02X%02X", CONFIG_BB_DEVICE_NAME, addr[1], addr[0]);
    ble_svc_gap_device_name_set(s_name);
    ESP_LOGI(TAG, "advertising as %s", s_name);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run(); // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ----------------------------------------------------------------- API ----

esp_err_t ble_sync_start(void)
{
    s_battery_mv = battery_read_mv();

    esp_err_t err = nvs_flash_init(); // PHY calibration data lives in NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(err));
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt registration rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    s_active = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void ble_sync_stop(void)
{
    s_active = false;
    ble_gap_adv_stop();
    if (s_connected) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 50 && s_connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    for (int i = 0; i < 100 && s_dump_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    ESP_LOGI(TAG, "stopped");
}

bool ble_sync_connected(void)
{
    return s_connected;
}
