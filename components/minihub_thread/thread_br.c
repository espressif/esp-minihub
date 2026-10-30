/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_thread.h"
#include "minihub_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif
#include "openthread/link.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/dns_client.h"
#include "openthread/thread.h"

static const char *TAG = "minihub_thread";

static char s_last_error[128];
static bool s_started;
static bool s_br_ready;
static bool s_eventfd_ok;
static SemaphoreHandle_t s_lock;

static void set_error(const char *msg)
{
    if (!msg) {
        s_last_error[0] = '\0';
        return;
    }
    strncpy(s_last_error, msg, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static void hex_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    static const char *hex = "0123456789abcdef";
    size_t need = in_len * 2 + 1;
    if (out_len < need) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[in[i] & 0xf];
    }
    out[in_len * 2] = '\0';
}

static esp_err_t hex_decode(const char *hex, uint8_t *out, size_t out_max, size_t *out_len)
{
    if (!hex || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(hex);
    if (n % 2 != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t bytes = n / 2;
    if (bytes > out_max) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < bytes; i++) {
        unsigned int v = 0;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)v;
    }
    *out_len = bytes;
    return ESP_OK;
}

static cJSON *thread_cfg_object(bool create)
{
    cJSON *root = minihub_config_get_adapters();
    if (!root) {
        return NULL;
    }
    cJSON *adapters = cJSON_GetObjectItem(root, "adapters");
    if (!cJSON_IsObject(adapters)) {
        if (!create) {
            return NULL;
        }
        adapters = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "adapters", adapters);
    }
    cJSON *thread = cJSON_GetObjectItem(adapters, "thread");
    if (!cJSON_IsObject(thread) && create) {
        thread = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters, "thread", thread);
        cJSON_AddBoolToObject(thread, "enabled", false);
    }
    return cJSON_IsObject(thread) ? thread : NULL;
}

static bool config_enabled(void)
{
    cJSON *thread = thread_cfg_object(false);
    if (!thread) {
        return false;
    }
    const cJSON *en = cJSON_GetObjectItem(thread, "enabled");
    return cJSON_IsTrue(en);
}

static esp_err_t persist_enabled(bool enabled)
{
    cJSON *thread = thread_cfg_object(true);
    if (!thread) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(thread, "enabled");
    cJSON_AddBoolToObject(thread, "enabled", enabled);
    return minihub_config_save_adapters(minihub_config_get_adapters());
}

static esp_err_t persist_dataset_hex(const char *hex)
{
    cJSON *thread = thread_cfg_object(true);
    if (!thread) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(thread, "dataset");
    if (hex && hex[0]) {
        cJSON_AddStringToObject(thread, "dataset", hex);
    } else {
        cJSON_AddNullToObject(thread, "dataset");
    }
    return minihub_config_save_adapters(minihub_config_get_adapters());
}

static esp_err_t load_dataset_from_config(otOperationalDatasetTlvs *tlvs, bool *have)
{
    *have = false;
    cJSON *thread = thread_cfg_object(false);
    if (!thread) {
        return ESP_OK;
    }
    const cJSON *ds = cJSON_GetObjectItem(thread, "dataset");
    if (!cJSON_IsString(ds) || !ds->valuestring || !ds->valuestring[0]) {
        return ESP_OK;
    }
    size_t len = 0;
    esp_err_t err = hex_decode(ds->valuestring, tlvs->mTlvs, sizeof(tlvs->mTlvs), &len);
    if (err != ESP_OK) {
        set_error("invalid adapters.thread.dataset hex");
        return err;
    }
    tlvs->mLength = (uint8_t)len;
    *have = true;
    return ESP_OK;
}

static esp_netif_t *pick_backbone_netif(void)
{
    esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (eth && esp_netif_is_netif_up(eth)) {
        return eth;
    }
    esp_netif_t *wifi = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (wifi && esp_netif_is_netif_up(wifi)) {
        return wifi;
    }
    return eth ? eth : wifi;
}

static const char *role_str(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED:
        return "disabled";
    case OT_DEVICE_ROLE_DETACHED:
        return "detached";
    case OT_DEVICE_ROLE_CHILD:
        return "child";
    case OT_DEVICE_ROLE_ROUTER:
        return "router";
    case OT_DEVICE_ROLE_LEADER:
        return "leader";
    default:
        return "unknown";
    }
}

static esp_err_t ensure_eventfd(void)
{
    if (s_eventfd_ok) {
        return ESP_OK;
    }
    esp_vfs_eventfd_config_t cfg = {
        .max_fds = 4,
    };
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already registered by another component. */
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        s_eventfd_ok = true;
    }
    return err;
}



static esp_err_t apply_or_form_dataset(otOperationalDatasetTlvs *from_cfg, bool have_cfg)
{
    otInstance *inst = esp_openthread_get_instance();
    if (!inst) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError oerr;
    if (have_cfg) {
        oerr = otDatasetSetActiveTlvs(inst, from_cfg);
    } else {
        otOperationalDataset dataset;
        memset(&dataset, 0, sizeof(dataset));
        oerr = otDatasetCreateNewNetwork(inst, &dataset);
        if (oerr == OT_ERROR_NONE) {
            oerr = otDatasetSetActive(inst, &dataset);
        }
    }
    if (oerr == OT_ERROR_NONE) {
        oerr = otIp6SetEnabled(inst, true);
    }
    if (oerr == OT_ERROR_NONE) {
        oerr = otThreadSetEnabled(inst, true);
    }
    esp_openthread_lock_release();

    if (oerr != OT_ERROR_NONE) {
        set_error("dataset/thread enable failed");
        return ESP_FAIL;
    }

    otOperationalDatasetTlvs active = {0};
    esp_openthread_lock_acquire(portMAX_DELAY);
    oerr = otDatasetGetActiveTlvs(inst, &active);
    esp_openthread_lock_release();
    if (oerr == OT_ERROR_NONE && active.mLength > 0) {
        char *hex = calloc(1, (size_t)active.mLength * 2 + 1);
        if (hex) {
            hex_encode(active.mTlvs, active.mLength, hex, (size_t)active.mLength * 2 + 1);
            (void)persist_dataset_hex(hex);
            free(hex);
        }
    }
    return ESP_OK;
}

static void configure_local_dns_client(void)
{
#if CONFIG_OPENTHREAD_DNS_CLIENT
    /* CHIP FindOperational uses OT DNS client. Stock default server is
     * 2001:4860:4860::8888 — UDP send fails with lwIP ERR_RTE on Thread.
     * Point DNS at this BR's local DNS-SD server (SRP table) via ML-EID:53. */
    otInstance *inst = esp_openthread_get_instance();
    const otIp6Address *ml = inst ? otThreadGetMeshLocalEid(inst) : NULL;
    if (inst && ml) {
        otDnsQueryConfig cfg = {0};
        const otDnsQueryConfig *cur = otDnsClientGetDefaultConfig(inst);
        if (cur) {
            cfg = *cur;
        }
        cfg.mServerSockAddr.mAddress = *ml;
        cfg.mServerSockAddr.mPort = 53;
        cfg.mRecursionFlag = OT_DNS_FLAG_NO_RECURSION;
        if (cfg.mResponseTimeout < 8000) {
            cfg.mResponseTimeout = 8000;
        }
        if (cfg.mMaxTxAttempts < 5) {
            cfg.mMaxTxAttempts = 5;
        }
        otDnsClientSetDefaultConfig(inst, &cfg);
        ESP_LOGI(TAG, "DNS client -> local DNSSD (ML-EID:53)");
    } else {
        ESP_LOGW(TAG, "DNS client: no ML-EID yet");
    }
#endif
}

static esp_err_t configure_br_after_ot(void)
{
    esp_netif_t *backbone = pick_backbone_netif();
    if (!backbone) {
        set_error("no backbone netif (Wi-Fi/ETH)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_openthread_set_backbone_netif(backbone);

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_openthread_border_router_init();
    esp_openthread_lock_release();
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already inited (reload re-enable / adopt path). */
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        set_error("border_router_init failed");
        return err;
    }

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    (void)esp_coex_wifi_i154_enable();
#endif

    otOperationalDatasetTlvs tlvs = {0};
    bool have = false;
    err = load_dataset_from_config(&tlvs, &have);
    if (err != ESP_OK) {
        return err;
    }
    err = apply_or_form_dataset(have ? &tlvs : NULL, have);
    if (err != ESP_OK) {
        return err;
    }

    configure_local_dns_client();

    s_br_ready = true;
    set_error(NULL);
    ESP_LOGI(TAG, "OpenThread Border Router started");
    return ESP_OK;
}

static esp_err_t start_stack(void)
{
    if (s_started && s_br_ready) {
        return ESP_OK;
    }

    /* Hub reload can leave the OT platform up after a failed stop (s_started
     * stays true). Do not call esp_openthread_start again — that spawns
     * ot_task_worker → ESP_ERROR_CHECK(esp_openthread_init) → abort.
     *
     * Note: esp_openthread_get_instance() is ALWAYS non-NULL (static Instance),
     * so it must not be used as a "already running" probe. */
    if (s_started) {
        ESP_LOGW(TAG, "re-binding Border Router to existing OpenThread platform");
        return configure_br_after_ot();
    }

    esp_err_t err = ensure_eventfd();
    if (err != ESP_OK) {
        set_error("eventfd register failed");
        return err;
    }

    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config =
            {
                .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
                .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
                .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
            },
    };
    err = esp_openthread_start(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Platform already up (e.g. stop cleared our flag incompletely). */
        ESP_LOGW(TAG, "esp_openthread_start: already running — adopting");
        s_started = true;
        return configure_br_after_ot();
    }
    if (err != ESP_OK) {
        set_error("esp_openthread_start failed");
        return err;
    }
    s_started = true;
    return configure_br_after_ot();
}

esp_err_t minihub_thread_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!config_enabled()) {
        ESP_LOGI(TAG, "Thread BR skipped (adapters.thread.enabled=false)");
        set_error(NULL);
        return ESP_OK;
    }
    return minihub_thread_start();
}

bool minihub_thread_ready(void)
{
    return s_br_ready;
}

esp_err_t minihub_thread_start(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t err = start_stack();
    if (err == ESP_OK) {
        (void)persist_enabled(true);
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

esp_err_t minihub_thread_stop(void)
{
    if (!s_started) {
        (void)persist_enabled(false);
        return ESP_OK;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *inst = esp_openthread_get_instance();
    if (inst) {
        (void)otThreadSetEnabled(inst, false);
        (void)otIp6SetEnabled(inst, false);
    }
    (void)esp_openthread_border_router_deinit();
    esp_openthread_lock_release();

    esp_err_t err = esp_openthread_stop();
    (void)persist_enabled(false);
    if (err != ESP_OK) {
        set_error("esp_openthread_stop failed");
        /* Keep s_started so the next start() adopts instead of double-init abort. */
        s_started = true;
        ESP_LOGW(TAG, "stop failed (%s) — leaving stack marked running", esp_err_to_name(err));
    } else {
        s_started = false;
        s_br_ready = false;
        set_error(NULL);
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

cJSON *minihub_thread_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ready", s_br_ready);
    cJSON_AddBoolToObject(root, "enabled_in_firmware", true);
    cJSON_AddBoolToObject(root, "config_enabled", config_enabled());
    cJSON_AddBoolToObject(root, "border_router", true);

    const char *role = "disabled";
    int channel = -1;
    bool dataset_set = false;
    if (s_started) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstance *inst = esp_openthread_get_instance();
        if (inst) {
            role = role_str(otThreadGetDeviceRole(inst));
            channel = otLinkGetChannel(inst);
            otOperationalDatasetTlvs tlvs = {0};
            if (otDatasetGetActiveTlvs(inst, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0) {
                dataset_set = true;
            }
        }
        esp_openthread_lock_release();
    }
    cJSON_AddStringToObject(root, "role", role);
    if (channel >= 0) {
        cJSON_AddNumberToObject(root, "channel", channel);
    } else {
        cJSON_AddNullToObject(root, "channel");
    }
    cJSON_AddBoolToObject(root, "dataset_set", dataset_set);
    if (s_last_error[0]) {
        cJSON_AddStringToObject(root, "last_error", s_last_error);
    } else {
        cJSON_AddNullToObject(root, "last_error");
    }
    return root;
}

const char *minihub_thread_last_error(void)
{
    return s_last_error;
}

esp_err_t minihub_thread_dataset_set(const uint8_t *tlvs, size_t len)
{
    if (!s_started) {
        esp_err_t err = minihub_thread_start();
        if (err != ESP_OK) {
            return err;
        }
    }

    otOperationalDatasetTlvs ds = {0};
    bool have = false;
    if (tlvs && len > 0) {
        if (len > sizeof(ds.mTlvs)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(ds.mTlvs, tlvs, len);
        ds.mLength = (uint8_t)len;
        have = true;
    }
    return apply_or_form_dataset(have ? &ds : NULL, have);
}

esp_err_t minihub_thread_dataset_get(uint8_t **out_tlvs, size_t *out_len)
{
    if (!out_tlvs || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_tlvs = NULL;
    *out_len = 0;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    otOperationalDatasetTlvs tlvs = {0};
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError oerr = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &tlvs);
    esp_openthread_lock_release();
    if (oerr != OT_ERROR_NONE || tlvs.mLength == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *buf = malloc(tlvs.mLength);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, tlvs.mTlvs, tlvs.mLength);
    *out_tlvs = buf;
    *out_len = tlvs.mLength;
    return ESP_OK;
}

esp_err_t minihub_thread_dataset_copy(uint8_t *buf, size_t buf_len, size_t *out_len)
{
    if (!buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *tmp = NULL;
    size_t len = 0;
    esp_err_t err = minihub_thread_dataset_get(&tmp, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len > buf_len) {
        free(tmp);
        *out_len = len;
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, tmp, len);
    *out_len = len;
    free(tmp);
    return ESP_OK;
}
