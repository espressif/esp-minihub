/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_module.h"
#include "minihub_ble_mesh.h"
#include "ble_mesh_map.h"
#include "minihub_product_params.h"
#include "ble_mesh_adapter.h"
#include "ble_mesh_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "minihub_core.h"
#include "sdkconfig.h"

static const char *TAG = "adapter_ble_mesh";

static const cJSON *s_config;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Parse even-length hex string into bytes. Returns length or -1. */
static int parse_hex_bytes(const char *hex, uint8_t *out, size_t out_cap)
{
    if (!hex || !out) {
        return -1;
    }
    while (*hex == ' ' || *hex == '\t') {
        hex++;
    }
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }
    size_t n = strlen(hex);
    if (n == 0 || (n & 1) || (n / 2) > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

static bool parse_model_id(const cJSON *item, uint16_t *model_id)
{
    if (!model_id) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *model_id = (uint16_t)item->valuedouble;
        return true;
    }
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const char *s = item->valuestring;
    if (strcmp(s, "gen_onoff_srv") == 0 || strcmp(s, "onoff") == 0) {
        *model_id = 0x1000; /* Generic OnOff Server */
        return true;
    }
    if (strcmp(s, "gen_level_srv") == 0 || strcmp(s, "level") == 0) {
        *model_id = 0x1002;
        return true;
    }
    if (strcmp(s, "light_lightness_srv") == 0 || strcmp(s, "lightness") == 0) {
        *model_id = 0x1300;
        return true;
    }
    if (strcmp(s, "light_ctl_srv") == 0 || strcmp(s, "ctl") == 0) {
        *model_id = 0x1303;
        return true;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (end == s || v > 0xFFFFUL) {
        return false;
    }
    *model_id = (uint16_t)v;
    return true;
}

static void apply_provision_policy(const cJSON *config)
{
    minihub_bm_provision_policy_t policy = {0};
    const cJSON *prov = cJSON_GetObjectItem(config, "provision");
    const cJSON *filters = NULL;
    const cJSON *bind_models = NULL;
    const cJSON *disabled = NULL;

    if (cJSON_IsObject(prov)) {
        filters = cJSON_GetObjectItem(prov, "uuid_filters");
        bind_models = cJSON_GetObjectItem(prov, "bind_models");
        disabled = cJSON_GetObjectItem(prov, "uuid_match_disabled");
    }
    /* Flat keys also accepted for pack inject convenience. */
    if (!filters) {
        filters = cJSON_GetObjectItem(config, "uuid_filters");
    }
    if (!bind_models) {
        bind_models = cJSON_GetObjectItem(config, "bind_models");
    }

    if (cJSON_IsTrue(disabled)) {
        policy.accept_all_uuid = true;
    } else if (cJSON_IsArray(filters)) {
        if (cJSON_GetArraySize(filters) == 0) {
            policy.accept_all_uuid = true;
        } else {
            cJSON *f = NULL;
            cJSON_ArrayForEach(f, filters) {
                if (policy.filter_count >= MINIHUB_BM_UUID_FILTER_MAX) {
                    break;
                }
                const cJSON *prefix = cJSON_GetObjectItem(f, "prefix_hex");
                if (!prefix) {
                    prefix = cJSON_GetObjectItem(f, "hex");
                }
                if (!cJSON_IsString(prefix)) {
                    continue;
                }
                minihub_bm_uuid_filter_t *slot = &policy.filters[policy.filter_count];
                int len = parse_hex_bytes(prefix->valuestring, slot->bytes,
                                          MINIHUB_BM_UUID_FILTER_BYTES_MAX);
                if (len <= 0) {
                    ESP_LOGW(TAG, "skip bad uuid filter: %s", prefix->valuestring);
                    continue;
                }
                slot->len = (uint8_t)len;
                const cJSON *offset = cJSON_GetObjectItem(f, "offset");
                slot->offset = cJSON_IsNumber(offset) ? (uint8_t)offset->valuedouble : 0;
                if ((size_t)slot->offset + slot->len > 16) {
                    ESP_LOGW(TAG, "uuid filter offset+len exceeds 16");
                    continue;
                }
                policy.filter_count++;
            }
            /* All filter entries invalid → fall open rather than block every UUID. */
            if (policy.filter_count == 0) {
                policy.accept_all_uuid = true;
            }
        }
    } else {
        /* No uuid_filters in adapters.json / packs → accept any unprov device.
         * Packs (e.g. esp_ble_mesh_onoff_server) may inject prefix filters later. */
        policy.accept_all_uuid = true;
    }

    if (cJSON_IsArray(bind_models)) {
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, bind_models) {
            if (policy.bind_model_count >= MINIHUB_BM_BIND_MODEL_MAX) {
                break;
            }
            const cJSON *mid = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "model_id") : m;
            const cJSON *name = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "name") : NULL;
            uint16_t model_id = 0;
            bool ok = parse_model_id(mid, &model_id);
            if (!ok && cJSON_IsString(name)) {
                ok = parse_model_id(name, &model_id);
            }
            if (!ok) {
                ESP_LOGW(TAG, "skip unknown bind model");
                continue;
            }
            /* Record for AppKey bind; Composition ∩ policy chooses element at runtime. */
            if (model_id != 0x1000 && model_id != 0x1002 && model_id != 0x1300 &&
                model_id != 0x1303) {
                ESP_LOGW(TAG, "bind model 0x%04x: AppKey bind only (no Hub client yet)",
                         model_id);
            }
            minihub_bm_bind_model_t *bm = &policy.bind_models[policy.bind_model_count];
            bm->model_id = model_id;
            bm->company_id = MINIHUB_BM_CID_SIG;
            const cJSON *cid = cJSON_IsObject(m) ? cJSON_GetObjectItem(m, "company_id") : NULL;
            if (cJSON_IsNumber(cid)) {
                bm->company_id = (uint16_t)cid->valuedouble;
            } else if (cJSON_IsString(cid) && cid->valuestring) {
                char *end = NULL;
                unsigned long v = strtoul(cid->valuestring, &end, 0);
                if (end != cid->valuestring && v <= 0xFFFFUL) {
                    bm->company_id = (uint16_t)v;
                }
            }
            policy.bind_model_count++;
        }
    }

    if (policy.filter_count > 0 || policy.accept_all_uuid || policy.bind_model_count > 0) {
        esp_err_t err = minihub_ble_mesh_set_provision_policy(&policy);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "set provision policy: %s", esp_err_to_name(err));
        }
    }
}

static const char *default_value_for_type(const char *type)
{
    if (type && strcmp(type, "sensor") == 0) {
        return "22.0";
    }
    return "off";
}

static bool type_supports_onoff(const char *type)
{
    return type && (strcmp(type, "light") == 0 || strcmp(type, "switch") == 0);
}

static const char *cid_manufacturer(uint16_t cid)
{
    if (cid == 0x02E5) {
        return "Espressif";
    }
    return NULL;
}

/* Merge Composition identity without replacing owner / ota / params. */
static void apply_identity_props(const minihub_ble_mesh_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    if (dev->uuid_hex[0]) {
        cJSON_AddStringToObject(patch, "uuid", dev->uuid_hex);
    }
    if (dev->cid) {
        const char *name = cid_manufacturer(dev->cid);
        if (name) {
            cJSON_AddStringToObject(patch, "manufacturer", name);
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%04X", (unsigned)dev->cid);
            cJSON_AddStringToObject(patch, "manufacturer", buf);
        }
        char pid[8];
        snprintf(pid, sizeof(pid), "0x%04X", (unsigned)dev->pid);
        cJSON_AddStringToObject(patch, "model", pid);
        char vid[8];
        snprintf(vid, sizeof(vid), "0x%04X", (unsigned)dev->vid);
        cJSON_AddStringToObject(patch, "sw_version", vid);
    }
    if (!patch->child) {
        cJSON_Delete(patch);
        return;
    }
    (void)minihub_point_merge_props(dev->point_id, NULL, patch);
    cJSON_Delete(patch);
}

static void register_point_from_dev(const minihub_ble_mesh_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    minihub_point_t point = {0};
    strncpy(point.point_id, dev->point_id, sizeof(point.point_id) - 1);
    strncpy(point.type, dev->type, sizeof(point.type) - 1);
    strncpy(point.adapter, "ble_mesh", sizeof(point.adapter) - 1);
    strncpy(point.value, default_value_for_type(dev->type), sizeof(point.value) - 1);
    point.online = true;
    minihub_point_register(&point);
    apply_identity_props(dev);

    minihub_pp_bm_param_t params[8];
    size_t n = minihub_pp_ble_mesh_list_params(dev->uuid_hex, params, 8);
    if (n > 0) {
        ESP_LOGI(TAG, "product_params matched %s n=%u", dev->point_id, (unsigned)n);
    }
}

static void on_bm_device(const minihub_ble_mesh_device_t *dev, void *ctx)
{
    (void)ctx;
    register_point_from_dev(dev);
}

static void try_product_params_attr(const minihub_ble_mesh_device_t *dev,
                                    const minihub_ble_mesh_attr_evt_t *evt)
{
    if (!dev || !evt || !dev->uuid_hex[0]) {
        return;
    }
    uint16_t model_id = 0;
    const char *field = NULL;
    double raw = 0;
    switch (evt->kind) {
    case MINIHUB_BM_ATTR_ONOFF:
        model_id = 0x1000;
        field = "onoff";
        raw = evt->u.onoff ? 1.0 : 0.0;
        break;
    case MINIHUB_BM_ATTR_LEVEL:
        model_id = 0x1002;
        field = "level";
        raw = (double)evt->u.level;
        break;
    case MINIHUB_BM_ATTR_LIGHTNESS:
        model_id = 0x1300;
        field = "lightness";
        raw = (double)evt->u.lightness;
        break;
    case MINIHUB_BM_ATTR_CTL:
        model_id = 0x1303;
        field = "temperature";
        raw = (double)evt->u.ctl.temperature;
        (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0,
                                        "lightness", (double)evt->u.ctl.lightness);
        break;
    default:
        return;
    }
    (void)minihub_pp_ble_mesh_apply(dev->point_id, dev->uuid_hex, model_id, MINIHUB_BM_CID_SIG, 0, field,
                                    raw);
}

static void on_bm_attr(const minihub_ble_mesh_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }
    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_unicast(evt->unicast);
    if (!dev) {
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_ONOFF) {
        minihub_point_set(dev->point_id, evt->u.onoff ? "on" : "off", NULL);
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_LIGHTNESS) {
        int pct = (int)((evt->u.lightness * 100u) / 65535u);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        minihub_point_set(dev->point_id, buf, NULL);
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_LEVEL) {
        /* Map signed level to 0..100 brightness-ish for Hub value. */
        int pct = (int)(((int)evt->u.level + 32768) * 100 / 65535);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        minihub_point_set(dev->point_id, buf, NULL);
        try_product_params_attr(dev, evt);
        return;
    }
    if (evt->kind == MINIHUB_BM_ATTR_CTL) {
        int pct = (int)((evt->u.ctl.lightness * 100u) / 65535u);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", pct);
        cJSON *params = cJSON_CreateObject();
        if (params) {
            cJSON_AddNumberToObject(params, "temperature", evt->u.ctl.temperature);
            cJSON_AddNumberToObject(params, "lightness", evt->u.ctl.lightness);
            minihub_point_merge_params(dev->point_id, buf, params);
            cJSON_Delete(params);
        } else {
            minihub_point_set(dev->point_id, buf, NULL);
        }
        try_product_params_attr(dev, evt);
    }
}

static esp_err_t apply_config_binds(const cJSON *config)
{
    if (!config) {
        return ESP_OK;
    }
    const cJSON *devices = cJSON_GetObjectItem(config, "devices");
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *uuid = cJSON_GetObjectItem(item, "uuid");
        const cJSON *unicast = cJSON_GetObjectItem(item, "unicast");
        const cJSON *elem_num = cJSON_GetObjectItem(item, "elem_num");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }
        minihub_ble_mesh_device_t dev = {0};
        strncpy(dev.point_id, point_id->valuestring, sizeof(dev.point_id) - 1);
        (void)minihub_bm_normalize_point_id(dev.point_id, sizeof(dev.point_id));
        strncpy(dev.type, type->valuestring, sizeof(dev.type) - 1);
        if (cJSON_IsString(uuid)) {
            strncpy(dev.uuid_hex, uuid->valuestring, sizeof(dev.uuid_hex) - 1);
        }
        if (cJSON_IsNumber(unicast)) {
            dev.unicast = (uint16_t)unicast->valuedouble;
        }
        if (cJSON_IsNumber(elem_num)) {
            dev.elem_num = (uint8_t)elem_num->valuedouble;
        } else {
            dev.elem_num = 1;
        }
        if (strcmp(dev.type, "sensor") == 0) {
            continue; /* Sensor path not in MVP */
        }
        dev.caps = MINIHUB_BM_CAP_ONOFF;
        (void)minihub_ble_mesh_upsert_device(&dev);
    }
    return ESP_OK;
}

static esp_err_t ble_mesh_adapter_init(const cJSON *config)
{
    esp_err_t err = minihub_adapter_check_core_api(1, 1);
    if (err != ESP_OK) {
        return err;
    }
    s_config = config;
    (void)minihub_product_params_init();
    (void)minihub_ble_mesh_init();
    apply_provision_policy(config);
    apply_config_binds(config);
    (void)minihub_module_register_rpc("ble_mesh", ble_mesh_module_rpc);
    return ESP_OK;
}

static void ble_mesh_register_mapped_points(void)
{
    minihub_ble_mesh_device_t list[MINIHUB_BM_MAX_DEVICES];
    size_t n = 0;
    if (minihub_ble_mesh_list_devices(list, MINIHUB_BM_MAX_DEVICES, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            register_point_from_dev(&list[i]);
        }
    }

#if !CONFIG_MINIHUB_BLE_MESH_SDK
    if (n == 0) {
        minihub_ble_mesh_device_t light = {0};
        strncpy(light.point_id, "ble_mesh.ble_mesh_light", sizeof(light.point_id) - 1);
        strncpy(light.type, "light", sizeof(light.type) - 1);
        light.caps = MINIHUB_BM_CAP_ONOFF;
        light.elem_num = 1;
        register_point_from_dev(&light);

        minihub_ble_mesh_device_t sw = {0};
        strncpy(sw.point_id, "ble_mesh.ble_mesh_plug", sizeof(sw.point_id) - 1);
        strncpy(sw.type, "switch", sizeof(sw.type) - 1);
        sw.caps = MINIHUB_BM_CAP_ONOFF;
        sw.elem_num = 1;
        register_point_from_dev(&sw);
        n = 2;
    }
#endif

    if (minihub_ble_mesh_running()) {
        ESP_LOGI(TAG, "ble_mesh adapter started (%u devices)", (unsigned)n);
    } else {
        ESP_LOGW(TAG, "ble_mesh adapter up but stack not running (%u mapped devices)", (unsigned)n);
    }
}

static esp_err_t ble_mesh_adapter_start(void)
{
    minihub_ble_mesh_set_callbacks(on_bm_device, on_bm_attr, NULL);

#if CONFIG_MINIHUB_BLE_MESH_SDK
    /* Host (main) sequences Zigbee-before-BLE when both are enabled; start here immediately. */
    minihub_heap_log();
    esp_err_t err = minihub_ble_mesh_start();
    minihub_heap_log();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Host stack start: %s", esp_err_to_name(err));
    }
    ble_mesh_register_mapped_points();
#else
    ESP_LOGI(TAG, "simulation mode (CONFIG_MINIHUB_BLE_MESH_SDK=n)");
    ble_mesh_register_mapped_points();
#endif

    (void)s_config;
    return ESP_OK;
}

static esp_err_t ble_mesh_adapter_stop(void)
{
    return minihub_ble_mesh_stop();
}

static esp_err_t ble_mesh_adapter_poll(void)
{
    return ESP_OK;
}

static esp_err_t ble_mesh_adapter_handle_command(const minihub_command_t *cmd)
{
    if (cmd && strcmp(cmd->cmd, "set_prop") == 0) {
        const cJSON *prop = cmd->params ? cJSON_GetObjectItem(cmd->params, "prop") : NULL;
        const cJSON *value = cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL;
        if (!cJSON_IsString(prop) || !value) {
            return ESP_ERR_INVALID_ARG;
        }
        /* v1: validate access + local echo; opcode write lands later. */
        return minihub_pp_set_prop(cmd->point_id, prop->valuestring, value, NULL, NULL);
    }

    const minihub_ble_mesh_device_t *dev = minihub_ble_mesh_find_by_point(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(cmd->cmd, "ota") == 0 || strcmp(cmd->cmd, "start_ota") == 0) {
        const char *path = NULL;
        const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "path") : NULL;
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "file_path") : NULL;
        }
        if (!cJSON_IsString(pj) || !pj->valuestring) {
            pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "url") : NULL;
        }
        if (cJSON_IsString(pj) && pj->valuestring) {
            path = pj->valuestring;
        }
        return minihub_ble_mesh_ota_offer(cmd->point_id, path);
    }

#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (dev->unicast == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(cmd->cmd, "set_brightness") == 0) {
        int bright = 50;
        if (cmd->params) {
            const cJSON *v = cJSON_GetObjectItem(cmd->params, "brightness");
            if (cJSON_IsNumber(v)) {
                bright = v->valueint;
            }
        }
        if (bright < 0) {
            bright = 0;
        }
        if (bright > 100) {
            bright = 100;
        }
        uint16_t lightness = (uint16_t)((bright * 65535) / 100);
        esp_err_t err = ESP_ERR_NOT_SUPPORTED;
        if (dev->caps & MINIHUB_BM_CAP_LIGHTNESS) {
            err = minihub_ble_mesh_lightness(dev->unicast, lightness);
        } else if (dev->caps & MINIHUB_BM_CAP_LEVEL) {
            int16_t level = (int16_t)((bright * 65535 / 100) - 32768);
            err = minihub_ble_mesh_level(dev->unicast, level);
        }
        if (err == ESP_OK) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", bright);
            minihub_point_set(cmd->point_id, buf, NULL);
        }
        return err;
    }

    if (strcmp(cmd->cmd, "set_color_temp") == 0 || strcmp(cmd->cmd, "set_ct") == 0) {
        if (!(dev->caps & MINIHUB_BM_CAP_CTL)) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        int temp = 4000;
        int bright = 100;
        if (cmd->params) {
            const cJSON *t = cJSON_GetObjectItem(cmd->params, "temperature");
            const cJSON *b = cJSON_GetObjectItem(cmd->params, "brightness");
            if (cJSON_IsNumber(t)) {
                temp = t->valueint;
            }
            if (cJSON_IsNumber(b)) {
                bright = b->valueint;
            }
        }
        if (bright < 0) {
            bright = 0;
        }
        if (bright > 100) {
            bright = 100;
        }
        uint16_t lightness = (uint16_t)((bright * 65535) / 100);
        esp_err_t err = minihub_ble_mesh_ctl(dev->unicast, lightness, (uint16_t)temp, 0);
        if (err == ESP_OK) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", bright);
            cJSON *params = cJSON_CreateObject();
            if (params) {
                cJSON_AddNumberToObject(params, "temperature", temp);
                minihub_point_merge_params(cmd->point_id, buf, params);
                cJSON_Delete(params);
            } else {
                minihub_point_set(cmd->point_id, buf, NULL);
            }
        }
        return err;
    }
#endif

    if (!type_supports_onoff(dev->type) && !(dev->caps & MINIHUB_BM_CAP_ONOFF)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

#if CONFIG_MINIHUB_BLE_MESH_SDK
    minihub_ble_mesh_onoff_cmd_t bm_cmd;
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_ON;
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_OFF;
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        bm_cmd = MINIHUB_BM_ONOFF_TOGGLE;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (dev->unicast == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = minihub_ble_mesh_onoff(dev->unicast, bm_cmd);
    if (err != ESP_OK) {
        return err;
    }
#endif

    if (strcmp(cmd->cmd, "turn_on") == 0) {
        return minihub_point_set(cmd->point_id, "on", NULL);
    }
    if (strcmp(cmd->cmd, "turn_off") == 0) {
        return minihub_point_set(cmd->point_id, "off", NULL);
    }
    if (strcmp(cmd->cmd, "toggle") == 0) {
        minihub_point_t snap = {0};
        minihub_point_get_snapshot(cmd->point_id, &snap);
        const char *next = (strcmp(snap.value, "on") == 0) ? "off" : "on";
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return minihub_point_set(cmd->point_id, next, NULL);
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const minihub_adapter_t ble_mesh_adapter = {
    .name = "ble_mesh",
    .version = "0.2.0",
    .init = ble_mesh_adapter_init,
    .start = ble_mesh_adapter_start,
    .stop = ble_mesh_adapter_stop,
    .poll = ble_mesh_adapter_poll,
    .handle_command = ble_mesh_adapter_handle_command,
};

REGISTER_ADAPTER(ble_mesh_adapter);
