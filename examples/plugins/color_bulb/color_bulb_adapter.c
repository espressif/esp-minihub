/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * color_bulb L3 adapter: simulates ONLINE color bulbs with no real hardware.
 * Each devices.json object is one point with type "light"; DISPLAY comes from
 * the Host catalog — the plugin never ships labels, widgets, CSS, or SVG.
 *
 * devices.json is packed beside the .so under the Store install dir. At start,
 * prefer Host module_read_file("devices.json"); fall back to embedded
 * DEVICE_DESC_JSON (static link / missing file / older Host).
 *
 * devices.json is an array of instances, or a single object:
 *   - device : endpoint id (second segment of point_id)
 *   - type   : catalog device_id (e.g. "light")
 *   - params : name, init; optional access / visible / min / max / step / unit / requires
 *
 * Model: one device = one minihub_point_t.
 *   point_id = sim.<device>
 *   command routing uses CB_ADAPTER (== module id), not the type string.
 */
#ifdef MINIHUB_PLUGIN_STANDALONE
#include "minihub_plugin_sdk.h"
#else
#include "esp_log.h"
#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "minihub_core.h"
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "device_desc.gen.h"

static const char *TAG = "color_bulb";

#define CB_ADAPTER "color_bulb"
#define CB_MANUFACTURER "Espressif"
#define CB_CATEGORY "Color Bulb"
#define CB_MAX_DEVICES 8

#define HOST_HAS(h, field) \
    ((h) && (h)->struct_size >= offsetof(minihub_module_host_api_t, field) + sizeof((h)->field))

extern const minihub_module_host_api_t *g_host;

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    cJSON *params;
} cb_dev_t;

static cb_dev_t s_devs[CB_MAX_DEVICES];
static size_t s_dev_count;

static void cb_json_to_point_str(const cJSON *v, char *buf, size_t n)
{
    buf[0] = '\0';
    if (!v) {
        return;
    }
    if (cJSON_IsBool(v)) {
        snprintf(buf, n, "%s", cJSON_IsTrue(v) ? "on" : "off");
    } else if (cJSON_IsNumber(v)) {
        snprintf(buf, n, "%d", v->valueint);
    } else if (cJSON_IsString(v)) {
        strncpy(buf, v->valuestring, n - 1);
        buf[n - 1] = '\0';
    }
}

static cb_dev_t *cb_find(const char *point_id)
{
    if (!point_id || point_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < s_dev_count; i++) {
        if (strcmp(s_devs[i].point_id, point_id) == 0) {
            return &s_devs[i];
        }
    }
    return NULL;
}

static void cb_summary_value(const cJSON *params, char *buf, size_t n)
{
    buf[0] = '\0';
    if (!params) {
        return;
    }
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(params, "power");
    const cJSON *v = power ? cJSON_GetObjectItemCaseSensitive(power, "value") : NULL;
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(buf, v->valuestring, n - 1);
        buf[n - 1] = '\0';
    }
}

static esp_err_t cb_publish(cb_dev_t *dev)
{
    if (!g_host || !dev || dev->point_id[0] == '\0' || !dev->params) {
        return ESP_ERR_INVALID_STATE;
    }
    char value[MINIHUB_VALUE_MAX];
    cb_summary_value(dev->params, value, sizeof(value));

    cJSON *params_dup = cJSON_Duplicate(dev->params, true);
    if (!params_dup) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (HOST_HAS(g_host, point_merge_params) && g_host->point_merge_params) {
        err = g_host->point_merge_params(dev->point_id, value, params_dup);
        cJSON_Delete(params_dup);
    } else {
        cJSON *props = cJSON_CreateObject();
        if (!props) {
            cJSON_Delete(params_dup);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(props, "manufacturer", CB_MANUFACTURER);
        cJSON_AddStringToObject(props, "category", CB_CATEGORY);
        cJSON_AddItemToObject(props, "params", params_dup);
        err = g_host->point_set(dev->point_id, value, props);
        cJSON_Delete(props);
    }
    return err;
}

static bool cb_set_param_value(cJSON *params, const char *name, const char *val_str)
{
    if (!name || !name[0] || !val_str || !params) {
        return false;
    }
    cJSON *ent = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsObject(ent)) {
        return false;
    }
    cJSON_DeleteItemFromObject(ent, "value");
    cJSON_AddStringToObject(ent, "value", val_str);
    return true;
}

static cJSON *cb_params_from_desc(const cJSON *params_arr)
{
    cJSON *out = cJSON_CreateObject();
    if (!out || !cJSON_IsArray(params_arr)) {
        cJSON_Delete(out);
        return NULL;
    }
    cJSON *p;
    cJSON_ArrayForEach(p, params_arr) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(p, "name");
        if (!cJSON_IsString(name) || !name->valuestring) {
            continue;
        }
        const cJSON *visible = cJSON_GetObjectItemCaseSensitive(p, "visible");
        bool hidden = cJSON_IsBool(visible) && cJSON_IsFalse(visible);
        const cJSON *access = cJSON_GetObjectItemCaseSensitive(p, "access");
        const char *access_str = cJSON_IsString(access) ? access->valuestring : NULL;
        const cJSON *jmin = cJSON_GetObjectItemCaseSensitive(p, "min");
        const cJSON *jmax = cJSON_GetObjectItemCaseSensitive(p, "max");
        const cJSON *jstep = cJSON_GetObjectItemCaseSensitive(p, "step");
        const cJSON *junit = cJSON_GetObjectItemCaseSensitive(p, "unit");
        const cJSON *jreq = cJSON_GetObjectItemCaseSensitive(p, "requires");

        char value[MINIHUB_VALUE_MAX];
        cb_json_to_point_str(cJSON_GetObjectItemCaseSensitive(p, "init"), value, sizeof(value));

        cJSON *ent = cJSON_CreateObject();
        if (!ent) {
            continue;
        }
        cJSON_AddStringToObject(ent, "value", value);
        if (hidden) {
            cJSON_AddBoolToObject(ent, "visible", false);
        }
        if (access_str) {
            cJSON_AddStringToObject(ent, "access", access_str);
        }
        if (cJSON_IsNumber(jmin)) {
            cJSON_AddNumberToObject(ent, "min", jmin->valuedouble);
        }
        if (cJSON_IsNumber(jmax)) {
            cJSON_AddNumberToObject(ent, "max", jmax->valuedouble);
        }
        if (cJSON_IsNumber(jstep)) {
            cJSON_AddNumberToObject(ent, "step", jstep->valuedouble);
        }
        if (cJSON_IsString(junit) && junit->valuestring && junit->valuestring[0]) {
            cJSON_AddStringToObject(ent, "unit", junit->valuestring);
        }
        if (cJSON_IsString(jreq) && jreq->valuestring && jreq->valuestring[0]) {
            cJSON_AddStringToObject(ent, "requires", jreq->valuestring);
        } else if (cJSON_IsObject(jreq)) {
            cJSON *dup = cJSON_Duplicate(jreq, true);
            if (dup) {
                cJSON_AddItemToObject(ent, "requires", dup);
            }
        }
        cJSON_AddItemToObject(out, name->valuestring, ent);
    }
    return out;
}

static esp_err_t cb_register_one(const cJSON *desc)
{
    if (!cJSON_IsObject(desc) || s_dev_count >= CB_MAX_DEVICES) {
        return ESP_ERR_NO_MEM;
    }
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(desc, "device");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(desc, "type");
    const cJSON *params_arr = cJSON_GetObjectItemCaseSensitive(desc, "params");
    if (!cJSON_IsString(device) || !device->valuestring || !device->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *typ = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : "light";

    cb_dev_t *slot = &s_devs[s_dev_count];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->point_id, sizeof(slot->point_id), "sim.%s", device->valuestring);
    slot->params = cb_params_from_desc(params_arr);
    if (!slot->params) {
        slot->point_id[0] = '\0';
        return ESP_ERR_NO_MEM;
    }

    char summary[MINIHUB_VALUE_MAX];
    cb_summary_value(slot->params, summary, sizeof(summary));

    minihub_point_t pt = {0};
    strncpy(pt.point_id, slot->point_id, sizeof(pt.point_id) - 1);
    strncpy(pt.type, typ, sizeof(pt.type) - 1);
    strncpy(pt.adapter, CB_ADAPTER, sizeof(pt.adapter) - 1);
    strncpy(pt.value, summary, sizeof(pt.value) - 1);
    pt.online = true;
    pt.props = cJSON_CreateObject();
    if (pt.props) {
        cJSON_AddStringToObject(pt.props, "manufacturer", CB_MANUFACTURER);
        cJSON_AddStringToObject(pt.props, "category", CB_CATEGORY);
        cJSON *params_dup = cJSON_Duplicate(slot->params, true);
        if (params_dup) {
            cJSON_AddItemToObject(pt.props, "params", params_dup);
        }
    }
    esp_err_t err = g_host->point_register(&pt);
    if (pt.props) {
        cJSON_Delete(pt.props);
    }
    if (err != ESP_OK) {
        cJSON_Delete(slot->params);
        slot->params = NULL;
        slot->point_id[0] = '\0';
        return err;
    }
    s_dev_count++;
    ESP_LOGI(TAG, "registered '%s' (type=%s)", slot->point_id, typ);
    return ESP_OK;
}

static esp_err_t cb_handle_command(const minihub_command_t *cmd)
{
    if (!cmd || cmd->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    cb_dev_t *dev = cb_find(cmd->point_id);
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *param = NULL;
    char buf[MINIHUB_VALUE_MAX];
    buf[0] = '\0';

    if (strcmp(cmd->cmd, "turn_on") == 0) {
        param = "power";
        strncpy(buf, "on", sizeof(buf) - 1);
    } else if (strcmp(cmd->cmd, "turn_off") == 0) {
        param = "power";
        strncpy(buf, "off", sizeof(buf) - 1);
    } else if (cmd->params) {
        const cJSON *pj = cJSON_GetObjectItem(cmd->params, "parameter");
        if (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0]) {
            param = pj->valuestring;
        }
        const cJSON *v = cJSON_GetObjectItem(cmd->params, "value");
        if (v) {
            cb_json_to_point_str(v, buf, sizeof(buf));
        }

        if (buf[0] == '\0') {
            if (strcmp(cmd->cmd, "set_brightness") == 0) {
                param = "brightness";
                cb_json_to_point_str(cJSON_GetObjectItem(cmd->params, "brightness"), buf, sizeof(buf));
            } else if (strcmp(cmd->cmd, "set_color_temp") == 0) {
                param = "cct";
                const cJSON *ct = cJSON_GetObjectItem(cmd->params, "color_temp");
                if (!ct) {
                    ct = cJSON_GetObjectItem(cmd->params, "temperature");
                }
                cb_json_to_point_str(ct, buf, sizeof(buf));
            } else if (strcmp(cmd->cmd, "toggle") == 0) {
                param = param ? param : "power";
                char cur[MINIHUB_VALUE_MAX];
                cb_summary_value(dev->params, cur, sizeof(cur));
                strncpy(buf, (strcmp(cur, "on") == 0) ? "off" : "on", sizeof(buf) - 1);
            }
        }
        if (!param) {
            param = "power";
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (!param || buf[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cb_set_param_value(dev->params, param, buf)) {
        ESP_LOGW(TAG, "unknown parameter '%s' on %s", param, cmd->point_id);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = cb_publish(dev);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "control %s.%s -> %s", cmd->point_id, param, buf);
    } else {
        ESP_LOGW(TAG, "control %s.%s failed: %s", cmd->point_id, param, esp_err_to_name(err));
    }
    return err;
}

static void cb_clear_devs(void)
{
    for (size_t i = 0; i < s_dev_count; i++) {
        if (s_devs[i].point_id[0] && g_host && g_host->point_unregister) {
            (void)g_host->point_unregister(s_devs[i].point_id);
        }
        if (s_devs[i].params) {
            cJSON_Delete(s_devs[i].params);
            s_devs[i].params = NULL;
        }
        s_devs[i].point_id[0] = '\0';
    }
    s_dev_count = 0;
}

static esp_err_t cb_init(const cJSON *config)
{
    (void)config;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

static bool cb_host_has_module_read_file(void)
{
    if (!g_host || !g_host->module_read_file) {
        return false;
    }
    const size_t need = offsetof(minihub_module_host_api_t, module_read_file) +
                        sizeof(g_host->module_read_file);
    return g_host->struct_size >= need;
}

/**
 * Prefer Store-unpacked devices.json via Host API; else embedded DEVICE_DESC_JSON.
 * @param[out] owned  set true when *json_str must be free()'d by caller
 */
static const char *cb_load_devices_json(bool *owned)
{
    *owned = false;
    if (cb_host_has_module_read_file()) {
        char *buf = NULL;
        size_t len = 0;
        esp_err_t err = g_host->module_read_file(CB_ADAPTER, "devices.json", &buf, &len);
        if (err == ESP_OK && buf && buf[0]) {
            ESP_LOGI(TAG, "devices.json from Store module dir (%u bytes)", (unsigned)len);
            *owned = true;
            return buf;
        }
        ESP_LOGW(TAG, "module_read_file(devices.json) failed (%s); using embed",
                 esp_err_to_name(err));
        free(buf);
    } else {
        ESP_LOGI(TAG, "Host has no module_read_file; using embedded devices.json");
    }
    return DEVICE_DESC_JSON;
}

static esp_err_t cb_start(void)
{
    cb_clear_devs();

    bool json_owned = false;
    const char *json_str = cb_load_devices_json(&json_owned);
    cJSON *root = cJSON_Parse(json_str);
    if (json_owned) {
        free((void *)json_str);
    }
    if (!root) {
        ESP_LOGE(TAG, "failed to parse device descriptor");
        return ESP_FAIL;
    }

    if (cJSON_IsArray(root)) {
        cJSON *item;
        cJSON_ArrayForEach(item, root) {
            esp_err_t err = cb_register_one(item);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "skip one descriptor: %s", esp_err_to_name(err));
            }
        }
    } else if (cJSON_IsObject(root)) {
        (void)cb_register_one(root);
    } else {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    if (s_dev_count == 0) {
        return ESP_FAIL;
    }
    g_host->command_register_adapter(CB_ADAPTER, cb_handle_command);
    ESP_LOGI(TAG, "start: %u simulated light(s)", (unsigned)s_dev_count);
    return ESP_OK;
}

static esp_err_t cb_stop(void)
{
    cb_clear_devs();
    return ESP_OK;
}

const minihub_adapter_t color_bulb_adapter = {
    .name = "color_bulb",
    .version = "0.1.0",
    .init = cb_init,
    .start = cb_start,
    .stop = cb_stop,
};

#if !CONFIG_MINIHUB_DYNAMIC_MODULES
REGISTER_ADAPTER(color_bulb_adapter);
#endif
