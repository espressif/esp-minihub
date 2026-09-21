/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_ble_mesh.h"
#include "ble_mesh_map.h"
#include "ble_mesh_nvs.h"
#include "ble_mesh_stack.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "minihub_core.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_ble_mesh";

static minihub_ble_mesh_device_t s_devices[MINIHUB_BM_MAX_DEVICES];
static size_t s_device_count;
static minihub_ble_mesh_device_cb_t s_user_device_cb;
static minihub_ble_mesh_attr_cb_t s_user_attr_cb;
static void *s_user_ctx;
static bool s_inited;

static void on_stack_device(const minihub_ble_mesh_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev) {
        return;
    }
    esp_err_t err = minihub_bm_nvs_upsert(dev, s_devices, &s_device_count, MINIHUB_BM_MAX_DEVICES);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upsert failed: %s", esp_err_to_name(err));
    }
    if (s_user_device_cb) {
        s_user_device_cb(dev, s_user_ctx);
    }
}

static void on_stack_attr(const minihub_ble_mesh_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (s_user_attr_cb && evt) {
        s_user_attr_cb(evt, s_user_ctx);
    }
}

static void restore_stack_nodes_from_map(void)
{
#if CONFIG_MINIHUB_BLE_MESH_SDK
    size_t restored = 0;
    for (size_t i = 0; i < s_device_count; i++) {
        const minihub_ble_mesh_device_t *d = &s_devices[i];
        if (!d->unicast) {
            continue;
        }
        uint8_t uuid[16] = {0};
        if (d->uuid_hex[0] && minihub_bm_hex_to_uuid(d->uuid_hex, uuid) != 0) {
            ESP_LOGW(TAG, "bad uuid for %s — restore by unicast only", d->point_id);
            memset(uuid, 0, sizeof(uuid));
        }
        esp_err_t err = ble_mesh_stack_restore_node(uuid, d->unicast, d->elem_num, d->caps);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore %s (0x%04x): %s", d->point_id, d->unicast, esp_err_to_name(err));
            continue;
        }
        restored++;
    }
    if (restored > 0) {
        ESP_LOGI(TAG, "restored %u node(s) into control cache", (unsigned)restored);
    }
#endif
}

static void on_stack_ready(void *ctx)
{
    (void)ctx;
    restore_stack_nodes_from_map();
    ESP_LOGI(TAG, "stack ready — %u mapped devices", (unsigned)s_device_count);
}

esp_err_t minihub_ble_mesh_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    esp_err_t err = minihub_bm_nvs_load(s_devices, MINIHUB_BM_MAX_DEVICES, &s_device_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load: %s", esp_err_to_name(err));
        s_device_count = 0;
    }
    ble_mesh_stack_set_hooks(on_stack_device, on_stack_attr, on_stack_ready, NULL);
    s_inited = true;
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_set_provision_policy(const minihub_bm_provision_policy_t *policy)
{
    return ble_mesh_stack_set_provision_policy(policy);
}

esp_err_t minihub_ble_mesh_start(void)
{
    esp_err_t err = minihub_ble_mesh_init();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_MINIHUB_BLE_MESH_SDK
    err = ble_mesh_stack_start();
    if (err != ESP_OK) {
        return err;
    }
    /* Soft re-enable skips on_ready; always rehydrate RAM node cache from NVS. */
    restore_stack_nodes_from_map();
    return ESP_OK;
#else
    ESP_LOGI(TAG, "SDK disabled (CONFIG_MINIHUB_BLE_MESH_SDK=n)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_ble_mesh_stop(void)
{
    return ble_mesh_stack_stop();
}

esp_err_t minihub_ble_mesh_permit_provision(int seconds)
{
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (seconds > 0 && !ble_mesh_stack_running()) {
        esp_err_t err = minihub_ble_mesh_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "permit_provision: stack start failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ble_mesh_stack_permit_provision(seconds);
#else
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool minihub_ble_mesh_permit_provision_active(void)
{
    return ble_mesh_stack_permit_provision_active();
}

bool minihub_ble_mesh_provisioning_busy(void)
{
    return ble_mesh_stack_rf_critical();
}

esp_err_t minihub_ble_mesh_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd)
{
    return ble_mesh_stack_onoff(unicast, cmd);
}

esp_err_t minihub_ble_mesh_level(uint16_t unicast, int16_t level)
{
    return ble_mesh_stack_level(unicast, level);
}

esp_err_t minihub_ble_mesh_lightness(uint16_t unicast, uint16_t lightness)
{
    return ble_mesh_stack_lightness(unicast, lightness);
}

esp_err_t minihub_ble_mesh_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                               int16_t delta_uv)
{
    return ble_mesh_stack_ctl(unicast, lightness, temperature, delta_uv);
}

esp_err_t minihub_ble_mesh_set_callbacks(minihub_ble_mesh_device_cb_t on_device,
                                         minihub_ble_mesh_attr_cb_t on_attr,
                                         void *ctx)
{
    s_user_device_cb = on_device;
    s_user_attr_cb = on_attr;
    s_user_ctx = ctx;
    return ESP_OK;
}

esp_err_t minihub_ble_mesh_list_devices(minihub_ble_mesh_device_t *out, size_t cap, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = s_device_count < cap ? s_device_count : cap;
    memcpy(out, s_devices, n * sizeof(minihub_ble_mesh_device_t));
    *count = n;
    return ESP_OK;
}

bool minihub_ble_mesh_running(void)
{
    return ble_mesh_stack_running();
}

esp_err_t minihub_ble_mesh_upsert_device(const minihub_ble_mesh_device_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)minihub_ble_mesh_init();
    return minihub_bm_nvs_upsert(dev, s_devices, &s_device_count, MINIHUB_BM_MAX_DEVICES);
}

esp_err_t minihub_ble_mesh_remove_device(const char *point_id)
{
    if (!point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)minihub_ble_mesh_init();

    uint16_t unicast = 0;
    const char *slash = strrchr(point_id, '/');
    const char *short_id = slash ? slash + 1 : point_id;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            unicast = s_devices[i].unicast;
            break;
        }
        const char *ts = strrchr(s_devices[i].point_id, '/');
        const char *tshort = ts ? ts + 1 : s_devices[i].point_id;
        if (short_id[0] && strcmp(tshort, short_id) == 0) {
            unicast = s_devices[i].unicast;
            break;
        }
    }
    /* Orphan UI points (in registry, not in NVS) — parse bm_XXXX from point_id. */
    if (unicast == 0) {
        unsigned u = 0;
        if (sscanf(short_id, "bm_%x", &u) == 1 && u > 0 && u <= 0x7fff) {
            unicast = (uint16_t)u;
        }
    }

    esp_err_t err = minihub_bm_nvs_remove(point_id, s_devices, &s_device_count);
    if (err == ESP_ERR_NOT_FOUND) {
        /* Ghost: Hub still lists the point after node lost provision / NVS diverge. */
        ESP_LOGW(TAG, "map miss for %s — clearing orphan point/stack entry", point_id);
        err = ESP_OK;
    } else if (err != ESP_OK) {
        return err;
    }
    if (unicast) {
        esp_err_t del = ble_mesh_stack_delete_node(unicast);
        if (del != ESP_OK) {
            ESP_LOGW(TAG, "stack delete 0x%04x: %s", unicast, esp_err_to_name(del));
        }
    }
    esp_err_t ureg = minihub_point_unregister(point_id);
    if (ureg != ESP_OK && ureg != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "point unregister %s: %s", point_id, esp_err_to_name(ureg));
    }
    /* Also drop alternate type prefix for same unicast (light/bm_X vs switch/bm_X). */
    if (unicast) {
        char alt[MINIHUB_POINT_ID_MAX];
        const char *alts[] = {"switch", "light"};
        for (size_t a = 0; a < sizeof(alts) / sizeof(alts[0]); a++) {
            if (minihub_bm_format_point_id(alt, sizeof(alt), alts[a], unicast) == 0 &&
                strcmp(alt, point_id) != 0) {
                (void)minihub_bm_nvs_remove(alt, s_devices, &s_device_count);
                (void)minihub_point_unregister(alt);
            }
        }
    }
    ESP_LOGI(TAG, "removed device %s", point_id);
    return ESP_OK;
}

const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_point(const char *point_id)
{
    if (!point_id) {
        return NULL;
    }
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_unicast(uint16_t unicast)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].unicast == unicast) {
            return &s_devices[i];
        }
        if (s_devices[i].unicast && s_devices[i].elem_num > 1 &&
            unicast >= s_devices[i].unicast &&
            unicast < (uint16_t)(s_devices[i].unicast + s_devices[i].elem_num)) {
            return &s_devices[i];
        }
    }
    return NULL;
}

esp_err_t minihub_ble_mesh_ota_offer(const char *point_id, const char *file_path)
{
    if (!point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point(point_id);
    if (!dev) {
        ESP_LOGW(TAG, "ota_offer: device not found (%s)", point_id);
        return ESP_ERR_NOT_FOUND;
    }
    /* Mesh Firmware Update / BLOB models are not wired. Plugins may still
     * publish props.ota on this point_id; air transport lands later. */
    ESP_LOGW(TAG,
             "ota_offer: BLE Mesh firmware update not implemented yet "
             "(point=%s unicast=0x%04x path=%s) — returning NOT_SUPPORTED",
             dev->point_id, dev->unicast, file_path ? file_path : "(none)");
    return ESP_ERR_NOT_SUPPORTED;
}
