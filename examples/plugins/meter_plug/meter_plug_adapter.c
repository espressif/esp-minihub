/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * meter_plug L3 adapter: simulates an ONLINE metering outlet (switch + V/A/W).
 * Registers ONE point with type "switch" so DISPLAY comes from the Host
 * device-widget catalog — the plugin never ships labels, widgets, CSS, or SVG.
 *
 * devices.json is INSTANCE-only (embedded as DEVICE_DESC_JSON).
 * Model: one device = one minihub_point_t (point_id = sim.<device>).
 *
 * Demo metering: power toggle; when on, voltage≈220 V, current>0, power_w≈V*I;
 * when off, current/power near 0 (line voltage still present).
 */
#include "esp_log.h"
#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "minihub_core.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "device_desc.gen.h"   /* GENERATED from devices.json; defines DEVICE_DESC_JSON */

static const char *TAG = "meter_plug";

#define MP_ADAPTER "meter_plug"
#define MP_MANUFACTURER "Espressif"
#define MP_CATEGORY "Metering Outlet"

#define HOST_HAS(h, field) \
    ((h) && (h)->struct_size >= offsetof(minihub_module_host_api_t, field) + sizeof((h)->field))

/* Plausible simulated load when the outlet relay is on. */
#define MP_LINE_V     220.0
#define MP_LOAD_A     0.45

extern const minihub_module_host_api_t *g_meter_plug_host;

static char s_point_id[MINIHUB_POINT_ID_MAX];
static cJSON *s_params; /* object: name -> { value, access?, visible?, min?, ... } */

static void mp_json_to_point_str(const cJSON *v, char *buf, size_t n)
{
    buf[0] = '\0';
    if (!v) {
        return;
    }
    if (cJSON_IsBool(v)) {
        snprintf(buf, n, "%s", cJSON_IsTrue(v) ? "on" : "off");
    } else if (cJSON_IsNumber(v)) {
        /* Prefer decimal when JSON has a fractional part; else plain int. */
        if (v->valuedouble != (double)v->valueint) {
            snprintf(buf, n, "%.2f", v->valuedouble);
        } else {
            snprintf(buf, n, "%d", v->valueint);
        }
    } else if (cJSON_IsString(v)) {
        strncpy(buf, v->valuestring, n - 1);
        buf[n - 1] = '\0';
    }
}

static void mp_summary_value(char *buf, size_t n)
{
    buf[0] = '\0';
    if (!s_params) {
        return;
    }
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(s_params, "power");
    const cJSON *v = power ? cJSON_GetObjectItemCaseSensitive(power, "value") : NULL;
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(buf, v->valuestring, n - 1);
        buf[n - 1] = '\0';
    }
}

static bool mp_set_param_value(const char *name, const char *val_str)
{
    if (!name || !name[0] || !val_str || !s_params) {
        return false;
    }
    cJSON *ent = cJSON_GetObjectItemCaseSensitive(s_params, name);
    if (!cJSON_IsObject(ent)) {
        return false;
    }
    cJSON_DeleteItemFromObject(ent, "value");
    cJSON_AddStringToObject(ent, "value", val_str);
    return true;
}

/* Sync voltage / current / power_w from power on/off. */
static void mp_refresh_metering(void)
{
    char cur[MINIHUB_VALUE_MAX];
    mp_summary_value(cur, sizeof(cur));
    bool on = (strcmp(cur, "on") == 0);
    char buf[MINIHUB_VALUE_MAX];

    snprintf(buf, sizeof(buf), "%.1f", MP_LINE_V);
    mp_set_param_value("voltage_v", buf);

    if (on) {
        snprintf(buf, sizeof(buf), "%.2f", MP_LOAD_A);
        mp_set_param_value("current_a", buf);
        snprintf(buf, sizeof(buf), "%.0f", MP_LINE_V * MP_LOAD_A);
        mp_set_param_value("power_w", buf);
    } else {
        mp_set_param_value("current_a", "0.00");
        mp_set_param_value("power_w", "0");
    }
}

static esp_err_t mp_publish(void)
{
    if (!g_meter_plug_host || s_point_id[0] == '\0' || !s_params) {
        return ESP_ERR_INVALID_STATE;
    }
    char value[MINIHUB_VALUE_MAX];
    mp_summary_value(value, sizeof(value));

    cJSON *params_dup = cJSON_Duplicate(s_params, true);
    if (!params_dup) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (HOST_HAS(g_meter_plug_host, point_merge_params) && g_meter_plug_host->point_merge_params) {
        err = g_meter_plug_host->point_merge_params(s_point_id, value, params_dup);
        cJSON_Delete(params_dup);
    } else {
        cJSON *props = cJSON_CreateObject();
        if (!props) {
            cJSON_Delete(params_dup);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(props, "manufacturer", MP_MANUFACTURER);
        cJSON_AddStringToObject(props, "category", MP_CATEGORY);
        cJSON_AddItemToObject(props, "params", params_dup);
        err = g_meter_plug_host->point_set(s_point_id, value, props);
        cJSON_Delete(props);
    }
    return err;
}

static void mp_register_device(const char *dev, const char *typ)
{
    cJSON *root = cJSON_Parse(DEVICE_DESC_JSON);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse embedded device descriptor");
        return;
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsArray(params)) {
        ESP_LOGE(TAG, "device descriptor has no 'params' array");
        cJSON_Delete(root);
        return;
    }

    if (s_params) {
        cJSON_Delete(s_params);
        s_params = NULL;
    }
    s_params = cJSON_CreateObject();
    if (!s_params) {
        cJSON_Delete(root);
        return;
    }

    cJSON *p;
    cJSON_ArrayForEach(p, params) {
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

        char value[MINIHUB_VALUE_MAX];
        mp_json_to_point_str(cJSON_GetObjectItemCaseSensitive(p, "init"), value, sizeof(value));

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
        cJSON_AddItemToObject(s_params, name->valuestring, ent);
    }

    mp_refresh_metering();

    snprintf(s_point_id, sizeof(s_point_id), "sim.%s", dev);

    char summary[MINIHUB_VALUE_MAX];
    mp_summary_value(summary, sizeof(summary));

    minihub_point_t pt = {0};
    strncpy(pt.point_id, s_point_id, sizeof(pt.point_id) - 1);
    strncpy(pt.type, typ, sizeof(pt.type) - 1);
    strncpy(pt.adapter, MP_ADAPTER, sizeof(pt.adapter) - 1);
    strncpy(pt.value, summary, sizeof(pt.value) - 1);
    pt.online = true;
    pt.props = cJSON_CreateObject();
    if (pt.props) {
        cJSON_AddStringToObject(pt.props, "manufacturer", MP_MANUFACTURER);
        cJSON_AddStringToObject(pt.props, "category", MP_CATEGORY);
        cJSON *params_dup = cJSON_Duplicate(s_params, true);
        if (params_dup) {
            cJSON_AddItemToObject(pt.props, "params", params_dup);
        }
    }
    g_meter_plug_host->point_register(&pt);
    if (pt.props) {
        cJSON_Delete(pt.props);
        pt.props = NULL;
    }
    ESP_LOGI(TAG, "registered device point '%s' (type=%s, %d param(s))",
             s_point_id, typ, cJSON_GetArraySize(params));
    cJSON_Delete(root);
}

static esp_err_t mp_handle_command(const minihub_command_t *cmd)
{
    if (!cmd || cmd->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(cmd->point_id, s_point_id) != 0) {
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
    } else if (strcmp(cmd->cmd, "toggle") == 0) {
        param = "power";
        char cur[MINIHUB_VALUE_MAX];
        mp_summary_value(cur, sizeof(cur));
        strncpy(buf, (strcmp(cur, "on") == 0) ? "off" : "on", sizeof(buf) - 1);
    } else if (cmd->params) {
        const cJSON *pj = cJSON_GetObjectItem(cmd->params, "parameter");
        if (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0]) {
            param = pj->valuestring;
        }
        const cJSON *v = cJSON_GetObjectItem(cmd->params, "value");
        if (v) {
            mp_json_to_point_str(v, buf, sizeof(buf));
        }

        if (buf[0] == '\0' && strcmp(cmd->cmd, "set") == 0) {
            /* set with only parameter defaults to power if value already parsed above */
        }
        if (buf[0] == '\0' && strcmp(cmd->cmd, "toggle") == 0) {
            param = param ? param : "power";
            char cur[MINIHUB_VALUE_MAX];
            mp_summary_value(cur, sizeof(cur));
            strncpy(buf, (strcmp(cur, "on") == 0) ? "off" : "on", sizeof(buf) - 1);
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
    if (!mp_set_param_value(param, buf)) {
        ESP_LOGW(TAG, "unknown parameter '%s' on %s", param, cmd->point_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (strcmp(param, "power") == 0) {
        mp_refresh_metering();
    }
    esp_err_t err = mp_publish();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "control %s.%s -> %s", cmd->point_id, param, buf);
    } else {
        ESP_LOGW(TAG, "control %s.%s failed: %s", cmd->point_id, param, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t mp_init(const cJSON *config)
{
    (void)config;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

static esp_err_t mp_start(void)
{
    const char *dev = "meter_plug";
    const char *typ = "switch";
    cJSON *root = cJSON_Parse(DEVICE_DESC_JSON);
    if (!root) {
        ESP_LOGE(TAG, "failed to parse embedded device descriptor");
        return ESP_FAIL;
    }
    const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(device)) {
        dev = device->valuestring;
    }
    if (cJSON_IsString(type)) {
        typ = type->valuestring;
    }

    ESP_LOGI(TAG, "start: simulating metering outlet (device=%s, type=%s)", dev, typ);
    mp_register_device(dev, typ);
    g_meter_plug_host->command_register_adapter(MP_ADAPTER, mp_handle_command);
    cJSON_Delete(root);
    return ESP_OK;
}

const minihub_adapter_t meter_plug_adapter = {
    .name = "meter_plug",
    .version = "0.1.0",
    .init = mp_init,
    .start = mp_start,
};

#if !CONFIG_MINIHUB_DYNAMIC_MODULES
REGISTER_ADAPTER(meter_plug_adapter);
#endif
