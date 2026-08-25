/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_ble_gatt.h"

#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !CONFIG_BT_NIMBLE_ENABLED || !CONFIG_BT_NIMBLE_GATT_CLIENT

esp_err_t minihub_ble_gatt_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t minihub_ble_gatt_deinit(void) { return ESP_OK; }
bool minihub_ble_gatt_ready(void) { return false; }
esp_err_t minihub_ble_gatt_scan_start(uint32_t d, uint16_t u)
{
    (void)d; (void)u;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_scan_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t minihub_ble_gatt_scan_get(minihub_ble_gatt_adv_t *o, size_t m, size_t *c)
{
    (void)o; (void)m;
    if (c) {
        *c = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_connect(const uint8_t a[6], uint8_t t, uint32_t ms)
{
    (void)a; (void)t; (void)ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_disconnect(void) { return ESP_ERR_NOT_SUPPORTED; }
bool minihub_ble_gatt_is_connected(void) { return false; }
uint16_t minihub_ble_gatt_conn_handle(void) { return 0xffff; }
esp_err_t minihub_ble_gatt_discover_svc(uint16_t u, uint32_t ms)
{
    (void)u; (void)ms;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_find_char(uint16_t u, uint16_t *v, uint16_t *c)
{
    (void)u; (void)v; (void)c;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_subscribe(uint16_t h, minihub_ble_gatt_notify_cb_t cb, void *ctx)
{
    (void)h; (void)cb; (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_write(uint16_t h, const uint8_t *d, size_t n, bool nr)
{
    (void)h; (void)d; (void)n; (void)nr;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_ble_gatt_wait_notify(uint16_t a, uint8_t *b, size_t bl, size_t *ol,
                                       uint16_t *oa, uint32_t ms)
{
    (void)a; (void)b; (void)bl; (void)ol; (void)oa; (void)ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_GATT_CLIENT */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble_gatt";

#define NOTIFY_Q_LEN   8
#define NOTIFY_MAX     256
#define GATT_CHAR_SLOTS 16

typedef struct {
    uint16_t attr_handle;
    uint16_t len;
    uint8_t data[NOTIFY_MAX];
} notify_item_t;

typedef struct {
    uint16_t uuid16;
    uint16_t val_handle;
    uint16_t cccd_handle;
} char_entry_t;

static bool s_inited;
static bool s_hs_synced;
static bool s_scanning;
static uint16_t s_scan_filter_uuid;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sync_sem;
static SemaphoreHandle_t s_op_sem;
static QueueHandle_t s_notify_q;
static int s_op_status;

static minihub_ble_gatt_adv_t s_advs[MINIHUB_BLE_GATT_ADV_MAX];
static size_t s_adv_count;

static uint16_t s_disc_svc_start;
static uint16_t s_disc_svc_end;
static char_entry_t s_chars[GATT_CHAR_SLOTS];
static size_t s_char_count;
static uint16_t s_pending_char_uuid;
static uint16_t s_pending_char_val;
static uint16_t s_pending_char_cccd;

static minihub_ble_gatt_notify_cb_t s_notify_cb;
static void *s_notify_ctx;

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    s_hs_synced = true;
    (void)ble_hs_util_ensure_addr(0);
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
}

static void adv_upsert(const minihub_ble_gatt_adv_t *adv)
{
    for (size_t i = 0; i < s_adv_count; i++) {
        if (memcmp(s_advs[i].addr, adv->addr, 6) == 0) {
            s_advs[i] = *adv;
            return;
        }
    }
    if (s_adv_count < MINIHUB_BLE_GATT_ADV_MAX) {
        s_advs[s_adv_count++] = *adv;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        minihub_ble_gatt_adv_t adv = {0};
        memcpy(adv.addr, event->disc.addr.val, 6);
        adv.addr_type = event->disc.addr.type;
        adv.rssi = event->disc.rssi;

        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                size_t n = fields.name_len;
                if (n >= sizeof(adv.name)) {
                    n = sizeof(adv.name) - 1;
                }
                memcpy(adv.name, fields.name, n);
                adv.name[n] = '\0';
            }
            if (fields.uuids16 != NULL) {
                for (int i = 0; i < fields.num_uuids16; i++) {
                    uint16_t u = ble_uuid_u16(&fields.uuids16[i].u);
                    if (s_scan_filter_uuid == 0 || u == s_scan_filter_uuid) {
                        adv.has_uuid16 = true;
                        adv.uuid16 = u;
                        break;
                    }
                }
            }
            if (fields.svc_data_uuid16 != NULL && fields.svc_data_uuid16_len >= 2) {
                uint16_t su = (uint16_t)fields.svc_data_uuid16[0] |
                              ((uint16_t)fields.svc_data_uuid16[1] << 8);
                if (s_scan_filter_uuid == 0 || su == s_scan_filter_uuid || adv.has_uuid16) {
                    adv.has_svc_data = true;
                    adv.svc_data_uuid16 = su;
                    size_t n = fields.svc_data_uuid16_len;
                    if (n > sizeof(adv.svc_data)) {
                        n = sizeof(adv.svc_data);
                    }
                    memcpy(adv.svc_data, fields.svc_data_uuid16, n);
                    adv.svc_data_len = (uint8_t)n;
                    if (!adv.has_uuid16 &&
                        (s_scan_filter_uuid == 0 || su == s_scan_filter_uuid)) {
                        adv.has_uuid16 = true;
                        adv.uuid16 = su;
                    }
                }
            }
            if (fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
                adv.has_mfg_data = true;
                adv.mfg_company = (uint16_t)fields.mfg_data[0] |
                                  ((uint16_t)fields.mfg_data[1] << 8);
                size_t n = fields.mfg_data_len;
                if (n > sizeof(adv.mfg_data)) {
                    n = sizeof(adv.mfg_data);
                }
                memcpy(adv.mfg_data, fields.mfg_data, n);
                adv.mfg_data_len = (uint8_t)n;
            }
        }
        if (s_scan_filter_uuid != 0 && !adv.has_uuid16 &&
            !(adv.has_svc_data && adv.svc_data_uuid16 == s_scan_filter_uuid)) {
            return 0;
        }
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            adv_upsert(&adv);
            xSemaphoreGive(s_lock);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_op_status = 0;
        } else {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_op_status = event->connect.status;
        }
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        notify_item_t item = {0};
        item.attr_handle = event->notify_rx.attr_handle;
        uint16_t om_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (om_len > NOTIFY_MAX) {
            om_len = NOTIFY_MAX;
        }
        item.len = om_len;
        os_mbuf_copydata(event->notify_rx.om, 0, om_len, item.data);
        if (s_notify_cb) {
            s_notify_cb(event->notify_rx.conn_handle, item.attr_handle,
                        item.data, item.len, s_notify_ctx);
        }
        if (s_notify_q) {
            (void)xQueueSend(s_notify_q, &item, 0);
        }
        return 0;
    }
    default:
        return 0;
    }
}

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        s_op_status = 0;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return 0;
    }
    if (error->status != 0) {
        s_op_status = error->status;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return error->status;
    }
    if (service) {
        s_disc_svc_start = service->start_handle;
        s_disc_svc_end = service->end_handle;
    }
    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        s_op_status = 0;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return 0;
    }
    if (error->status != 0) {
        s_op_status = error->status;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return error->status;
    }
    if (chr && s_char_count < GATT_CHAR_SLOTS) {
        char_entry_t *e = &s_chars[s_char_count++];
        e->uuid16 = ble_uuid_u16(&chr->uuid.u);
        e->val_handle = chr->val_handle;
        e->cccd_handle = 0;
        if (e->uuid16 == s_pending_char_uuid) {
            s_pending_char_val = e->val_handle;
        }
    }
    return 0;
}

static int disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        s_op_status = 0;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return 0;
    }
    if (error->status != 0) {
        s_op_status = error->status;
        if (s_op_sem) {
            xSemaphoreGive(s_op_sem);
        }
        return error->status;
    }
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
        for (size_t i = 0; i < s_char_count; i++) {
            if (s_chars[i].val_handle == chr_val_handle) {
                s_chars[i].cccd_handle = dsc->handle;
                if (chr_val_handle == s_pending_char_val) {
                    s_pending_char_cccd = dsc->handle;
                }
                break;
            }
        }
    }
    return 0;
}

esp_err_t minihub_ble_gatt_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_sync_sem) {
        s_sync_sem = xSemaphoreCreateBinary();
    }
    if (!s_op_sem) {
        s_op_sem = xSemaphoreCreateBinary();
    }
    if (!s_notify_q) {
        s_notify_q = xQueueCreate(NOTIFY_Q_LEN, sizeof(notify_item_t));
    }
    if (!s_lock || !s_sync_sem || !s_op_sem || !s_notify_q) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = nimble_port_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nimble already initialized");
        s_hs_synced = true;
        s_inited = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = NULL;
    nimble_port_freertos_init(host_task);

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "NimBLE sync timeout");
    }
    s_inited = true;
    ESP_LOGI(TAG, "GATT client ready");
    return ESP_OK;
}

esp_err_t minihub_ble_gatt_deinit(void)
{
    (void)minihub_ble_gatt_scan_stop();
    (void)minihub_ble_gatt_disconnect();
    /* Keep NimBLE up — other stacks may share the host. */
    return ESP_OK;
}

bool minihub_ble_gatt_ready(void)
{
    return s_inited && s_hs_synced;
}

esp_err_t minihub_ble_gatt_scan_start(uint32_t duration_ms, uint16_t filter_uuid16)
{
    if (!minihub_ble_gatt_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_scanning) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        s_adv_count = 0;
        memset(s_advs, 0, sizeof(s_advs));
        xSemaphoreGive(s_lock);
    }
    s_scan_filter_uuid = filter_uuid16;

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return ESP_FAIL;
    }

    struct ble_gap_disc_params params = {
        .filter_duplicates = 1,
        .passive = 1,
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
    };
    int32_t to = duration_ms ? (int32_t)duration_ms : BLE_HS_FOREVER;
    rc = ble_gap_disc(own_addr_type, to, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return ESP_FAIL;
    }
    s_scanning = true;
    return ESP_OK;
}

esp_err_t minihub_ble_gatt_scan_stop(void)
{
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }
    return ESP_OK;
}

esp_err_t minihub_ble_gatt_scan_get(minihub_ble_gatt_adv_t *out, size_t max, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    size_t n = s_adv_count < max ? s_adv_count : max;
    memcpy(out, s_advs, n * sizeof(minihub_ble_gatt_adv_t));
    *count = n;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t minihub_ble_gatt_connect(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (!minihub_ble_gatt_ready() || !addr) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)minihub_ble_gatt_scan_stop();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) {
    }

    ble_addr_t peer = {
        .type = addr_type,
    };
    memcpy(peer.val, addr, 6);

    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s_op_status = -1;
    rc = ble_gap_connect(own_addr_type, &peer, timeout_ms ? (int32_t)timeout_ms : 20000,
                         NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms ? timeout_ms + 1000 : 25000)) != pdTRUE) {
        (void)ble_gap_conn_cancel();
        return ESP_ERR_TIMEOUT;
    }
    return (s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_op_status == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t minihub_ble_gatt_disconnect(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_OK;
    }
    int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool minihub_ble_gatt_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

uint16_t minihub_ble_gatt_conn_handle(void)
{
    return s_conn_handle;
}

esp_err_t minihub_ble_gatt_discover_svc(uint16_t svc_uuid16, uint32_t timeout_ms)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) {
    }
    s_disc_svc_start = 0;
    s_disc_svc_end = 0;
    s_char_count = 0;
    memset(s_chars, 0, sizeof(s_chars));
    s_op_status = -1;

    ble_uuid16_t uuid = BLE_UUID16_INIT(svc_uuid16);
    int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &uuid.u, disc_svc_cb, NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms ? timeout_ms : 10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_op_status != 0 || s_disc_svc_start == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) {
    }
    s_op_status = -1;
    rc = ble_gattc_disc_all_chrs(s_conn_handle, s_disc_svc_start, s_disc_svc_end,
                                 disc_chr_cb, NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms ? timeout_ms : 10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_op_status != 0) {
        return ESP_FAIL;
    }

    while (xSemaphoreTake(s_op_sem, 0) == pdTRUE) {
    }
    s_op_status = -1;
    rc = ble_gattc_disc_all_dscs(s_conn_handle, s_disc_svc_start, s_disc_svc_end,
                                 disc_dsc_cb, NULL);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_op_sem, pdMS_TO_TICKS(timeout_ms ? timeout_ms : 10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_op_status == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t minihub_ble_gatt_find_char(uint16_t char_uuid16, uint16_t *val_handle, uint16_t *cccd_handle)
{
    for (size_t i = 0; i < s_char_count; i++) {
        if (s_chars[i].uuid16 == char_uuid16) {
            if (val_handle) {
                *val_handle = s_chars[i].val_handle;
            }
            if (cccd_handle) {
                *cccd_handle = s_chars[i].cccd_handle;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_ble_gatt_subscribe(uint16_t cccd_handle,
                                     minihub_ble_gatt_notify_cb_t cb,
                                     void *ctx)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || cccd_handle == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_notify_cb = cb;
    s_notify_ctx = ctx;
    uint8_t value[2] = {0x01, 0x00}; /* notifications */
    int rc = ble_gattc_write_flat(s_conn_handle, cccd_handle, value, sizeof(value), NULL, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t minihub_ble_gatt_write(uint16_t val_handle, const uint8_t *data, size_t len, bool no_rsp)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int rc;
    if (no_rsp) {
        rc = ble_gattc_write_no_rsp_flat(s_conn_handle, val_handle, data, len);
    } else {
        rc = ble_gattc_write_flat(s_conn_handle, val_handle, data, len, NULL, NULL);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t minihub_ble_gatt_wait_notify(uint16_t attr_handle,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len,
                                       uint16_t *out_attr,
                                       uint32_t timeout_ms)
{
    if (!buf || !out_len || buf_len == 0 || !s_notify_q) {
        return ESP_ERR_INVALID_ARG;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms ? timeout_ms : 5000);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        notify_item_t item;
        if (xQueueReceive(s_notify_q, &item, deadline - now) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (attr_handle != 0 && item.attr_handle != attr_handle) {
            continue;
        }
        size_t n = item.len < buf_len ? item.len : buf_len;
        memcpy(buf, item.data, n);
        *out_len = n;
        if (out_attr) {
            *out_attr = item.attr_handle;
        }
        return ESP_OK;
    }
}

#endif /* CONFIG_BT_NIMBLE_ENABLED && CONFIG_BT_NIMBLE_GATT_CLIENT */
