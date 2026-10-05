/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_device_widget.h"
#include "minihub_module.h"
#include "minihub_zigbee.h"
#include "minihub_product_params.h"
#include "zigbee_cluster_map.h"
#include "zigbee_map.h"
#include "zigbee_rpc.h"
#include "zigbee_stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "adapter_zigbee";

static const cJSON *s_config;
static float s_last_humidity = -1.0f;

static const char *default_value_for_dev(const minihub_zigbee_device_t *dev)
{
    return minihub_zb_map_default_summary(dev);
}

static bool type_supports_onoff(const char *type)
{
    return minihub_zb_map_type_has_onoff(type);
}

static void json_to_str(const cJSON *v, char *buf, size_t n)
{
    buf[0] = '\0';
    if (!v || n == 0) {
        return;
    }
    if (cJSON_IsBool(v)) {
        snprintf(buf, n, "%s", cJSON_IsTrue(v) ? "on" : "off");
    } else if (cJSON_IsNumber(v)) {
        if (v->valuedouble == (double)v->valueint) {
            snprintf(buf, n, "%d", v->valueint);
        } else {
            snprintf(buf, n, "%g", v->valuedouble);
        }
    } else if (cJSON_IsString(v) && v->valuestring) {
        strncpy(buf, v->valuestring, n - 1);
        buf[n - 1] = '\0';
    }
}

static bool power_str_on(const char *s)
{
    return s && (strcmp(s, "on") == 0 || strcmp(s, "ON") == 0 || strcmp(s, "1") == 0 ||
                 strcmp(s, "true") == 0 || strcmp(s, "TRUE") == 0);
}

static void mark_param_access_r(cJSON *ent)
{
    if (!cJSON_IsObject(ent)) {
        return;
    }
    cJSON_DeleteItemFromObject(ent, "access");
    cJSON_AddStringToObject(ent, "access", "R");
}

static bool param_access_readonly(const char *name)
{
    const minihub_zb_map_attr_t *a = minihub_zb_map_attr_by_param(name);
    return a && minihub_zb_map_access_is_readonly(a->access);
}

static cJSON *ensure_param_shell(cJSON *params, const char *name, const char *init_value)
{
    if (!params || !name || !name[0]) {
        return NULL;
    }
    cJSON *ent = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!cJSON_IsObject(ent)) {
        cJSON_DeleteItemFromObject(params, name);
        ent = cJSON_CreateObject();
        if (!ent) {
            return NULL;
        }
        cJSON_AddItemToObject(params, name, ent);
        cJSON_AddStringToObject(ent, "value", init_value ? init_value : "");
        if (param_access_readonly(name)) {
            mark_param_access_r(ent);
        }
    } else if (param_access_readonly(name)) {
        mark_param_access_r(ent);
    }
    return ent;
}

static void set_param_value_str(cJSON *params, const char *name, const char *val)
{
    cJSON *ent = ensure_param_shell(params, name, val);
    if (!ent || !val) {
        return;
    }
    cJSON_DeleteItemFromObject(ent, "value");
    cJSON_AddStringToObject(ent, "value", val);
}

static void set_param_value_number(cJSON *params, const char *name, double val)
{
    cJSON *ent = ensure_param_shell(params, name, "");
    if (!ent) {
        return;
    }
    cJSON_DeleteItemFromObject(ent, "value");
    cJSON_AddNumberToObject(ent, "value", val);
}

static void set_param_value_bool(cJSON *params, const char *name, bool val)
{
    cJSON *ent = ensure_param_shell(params, name, "");
    if (!ent) {
        return;
    }
    cJSON_DeleteItemFromObject(ent, "value");
    cJSON_AddBoolToObject(ent, "value", val);
}

/**
 * Seed props.params = type-catalog params ∩ attr-map params for device caps.
 * Catalog supplies the type's full param list; map restricts to Zigbee-wired ones.
 * When catalog is missing (e.g. sensor), seed from the attr map alone.
 */
static cJSON *seed_params_for_dev(const minihub_zigbee_device_t *dev, const char *summary)
{
    cJSON *params = cJSON_CreateObject();
    if (!params || !dev) {
        cJSON_Delete(params);
        return NULL;
    }

    char mapped[MINIHUB_ZB_MAP_MAX_ATTRS][MINIHUB_ZB_MAP_PARAM_MAX];
    size_t mapped_n = minihub_zb_map_params_for_caps(dev->caps, mapped, MINIHUB_ZB_MAP_MAX_ATTRS);

    const minihub_device_widget_device_t *dw =
        (dev->type[0]) ? minihub_device_widget_get_or_fetch(NULL, dev->type) : NULL;

    if (dw && dw->entries && dw->count > 0) {
        for (size_t i = 0; i < dw->count; i++) {
            const char *pname = dw->entries[i].parameter;
            if (!pname || !pname[0]) {
                continue;
            }
            bool in_map = false;
            for (size_t m = 0; m < mapped_n; m++) {
                if (strcmp(mapped[m], pname) == 0) {
                    in_map = true;
                    break;
                }
            }
            if (!in_map) {
                continue;
            }
            const char *init = "";
            const minihub_zb_map_attr_t *a = minihub_zb_map_attr_by_param(pname);
            if (a && a->summary && summary && summary[0]) {
                init = summary;
            }
            (void)ensure_param_shell(params, pname, init);
        }
    }

    /* Ensure every mapped param exists (sensor types may lack catalog rows). */
    for (size_t m = 0; m < mapped_n; m++) {
        const char *pname = mapped[m];
        if (cJSON_GetObjectItemCaseSensitive(params, pname)) {
            continue;
        }
        const char *init = "";
        const minihub_zb_map_attr_t *a = minihub_zb_map_attr_by_param(pname);
        if (a && a->summary && summary && summary[0]) {
            init = summary;
        }
        (void)ensure_param_shell(params, pname, init);
    }

    if (type_supports_onoff(dev->type) && (dev->caps & MINIHUB_ZB_CAP_ONOFF)) {
        const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param("power");
        const char *pname = ctrl ? ctrl->param : "power";
        (void)ensure_param_shell(params, pname, summary ? summary : "off");
    }

    return params;
}

static esp_err_t publish_params(const char *point_id, const char *summary, cJSON *params)
{
    if (!point_id || !point_id[0] || !params) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Merge params only — keep plugin extras (ota / ieee / manufacturer / …). */
    return minihub_point_merge_params(point_id, summary ? summary : "", params);
}

static void format_ieee_hex(uint64_t ieee, char *out, size_t out_len)
{
    if (!out || out_len < 3) {
        return;
    }
    snprintf(out, out_len, "0x%016llx", (unsigned long long)ieee);
}

/** Top-level identity on the Host point (not catalog params / Devices sliders). */
static void apply_identity_props(const minihub_zigbee_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    char ieee_s[24];
    format_ieee_hex(dev->ieee, ieee_s, sizeof(ieee_s));
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    cJSON_AddStringToObject(patch, "ieee", ieee_s);
    (void)minihub_point_merge_props(dev->point_id, NULL, patch);
    cJSON_Delete(patch);
}

static esp_err_t snapshot_params(const char *point_id, char *summary, size_t summary_len,
                                 cJSON **out_params)
{
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (summary && summary_len) {
        strncpy(summary, snap.value, summary_len - 1);
        summary[summary_len - 1] = '\0';
    }
    cJSON *params = NULL;
    if (snap.props) {
        cJSON *p = cJSON_GetObjectItem(snap.props, "params");
        if (cJSON_IsObject(p)) {
            params = cJSON_Duplicate(p, true);
        }
        cJSON_Delete(snap.props);
    }
    if (!params) {
        params = cJSON_CreateObject();
    }
    if (!params) {
        return ESP_ERR_NO_MEM;
    }
    *out_params = params;
    return ESP_OK;
}

static void register_point_from_dev(const minihub_zigbee_device_t *dev)
{
    if (!dev || !dev->point_id[0]) {
        return;
    }
    /* Keep live props.params across rediscovery / start re-register. */
    const minihub_point_t *existing = minihub_point_get(dev->point_id);
    if (existing && existing->props) {
        const cJSON *p = cJSON_GetObjectItem(existing->props, "params");
        if (cJSON_IsObject(p) && p->child != NULL) {
            apply_identity_props(dev);
            return;
        }
    }

    const char *summary = default_value_for_dev(dev);
    cJSON *params = seed_params_for_dev(dev, summary);
    if (!params) {
        ESP_LOGW(TAG, "seed params failed for %s", dev->point_id);
        return;
    }

    minihub_point_t point = {0};
    strncpy(point.point_id, dev->point_id, sizeof(point.point_id) - 1);
    strncpy(point.type, dev->type, sizeof(point.type) - 1);
    strncpy(point.adapter, "zigbee", sizeof(point.adapter) - 1);
    strncpy(point.value, summary, sizeof(point.value) - 1);
    point.online = true;
    point.props = cJSON_CreateObject();
    if (point.props) {
        char ieee_s[24];
        format_ieee_hex(dev->ieee, ieee_s, sizeof(ieee_s));
        cJSON_AddStringToObject(point.props, "ieee", ieee_s);
        cJSON_AddItemToObject(point.props, "params", params);
        params = NULL;
    } else {
        cJSON_Delete(params);
    }
    esp_err_t err = minihub_point_register(&point);
    if (point.props) {
        cJSON_Delete(point.props);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register %s: %s", dev->point_id, esp_err_to_name(err));
    }
}

static void on_zb_device(const minihub_zigbee_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev) {
        return;
    }
    ESP_LOGI(TAG, "map device %s type=%s short=0x%04x ep=%u caps=0x%02x", dev->point_id, dev->type,
             dev->short_addr, dev->endpoint, dev->caps);
    register_point_from_dev(dev);
}

static const char *param_for_report(const char *report)
{
    const minihub_zb_map_attr_t *a = minihub_zb_map_attr_by_report(report);
    return (a && a->param[0]) ? a->param : NULL;
}

static void apply_contact_report(cJSON *params, const minihub_zigbee_attr_evt_t *evt, char *summary,
                                 size_t summary_len)
{
    const minihub_zb_map_attr_t *list[8];
    size_t n = minihub_zb_map_attrs_by_report("contact", list, 8);
    const char *open_closed = evt->u.contact.open ? "open" : "closed";
    for (size_t i = 0; i < n; i++) {
        const char *field = list[i]->field[0] ? list[i]->field : "open_closed";
        if (strcmp(field, "open_closed") == 0) {
            set_param_value_str(params, list[i]->param, open_closed);
            if (list[i]->summary && summary && summary_len) {
                strncpy(summary, open_closed, summary_len - 1);
                summary[summary_len - 1] = '\0';
            }
        } else if (strcmp(field, "zone_status") == 0) {
            set_param_value_number(params, list[i]->param, evt->u.contact.zone_status);
        } else if (strcmp(field, "tamper") == 0) {
            if (evt->u.contact.tamper) {
                set_param_value_bool(params, list[i]->param, true);
            }
        } else if (strcmp(field, "battery_low") == 0) {
            if (evt->u.contact.battery_low) {
                set_param_value_bool(params, list[i]->param, true);
            }
        }
    }
}

static void on_zb_attr(const minihub_zigbee_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (!evt) {
        return;
    }
    const minihub_zigbee_device_t *dev =
        minihub_zigbee_find_by_addr(evt->short_addr, evt->endpoint);
    if (!dev) {
        ESP_LOGW(TAG, "attr evt short=0x%04x ep=%u kind=%d — no mapped device", evt->short_addr,
                 evt->endpoint, (int)evt->kind);
        return;
    }

    char summary[MINIHUB_VALUE_MAX];
    cJSON *params = NULL;
    if (snapshot_params(dev->point_id, summary, sizeof(summary), &params) != ESP_OK) {
        params = seed_params_for_dev(dev, default_value_for_dev(dev));
        strncpy(summary, default_value_for_dev(dev), sizeof(summary) - 1);
        summary[sizeof(summary) - 1] = '\0';
        if (!params) {
            return;
        }
    }

    const char *report = minihub_zb_map_report_name_for_kind(evt->kind);
    if (!report) {
        cJSON_Delete(params);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_CONTACT) {
        ESP_LOGI(TAG, "point %s ← contact status=0x%04x", dev->point_id, evt->u.contact.zone_status);
        apply_contact_report(params, evt, summary, sizeof(summary));
        (void)publish_params(dev->point_id, summary, params);
        cJSON_Delete(params);
        return;
    }

    const char *param = param_for_report(report);
    if (!param) {
        ESP_LOGD(TAG, "no attr-map param for report=%s", report);
        cJSON_Delete(params);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_ONOFF) {
        const char *v = evt->u.onoff ? "on" : "off";
        ESP_LOGI(TAG, "point %s ← %s %s", dev->point_id, param, v);
        set_param_value_str(params, param, v);
        strncpy(summary, v, sizeof(summary) - 1);
        (void)publish_params(dev->point_id, summary, params);
        cJSON_Delete(params);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_HUMIDITY_PCT) {
        s_last_humidity = evt->u.humidity_pct;
        ESP_LOGI(TAG, "point %s ← %s %.1f", dev->point_id, param, (double)evt->u.humidity_pct);
        set_param_value_number(params, param, evt->u.humidity_pct);
        (void)publish_params(dev->point_id, summary[0] ? summary : "0", params);
        cJSON_Delete(params);
        return;
    }

    if (evt->kind == MINIHUB_ZB_ATTR_TEMP_C) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", (double)evt->u.temp_c);
        ESP_LOGI(TAG, "point %s ← %s %s", dev->point_id, param, buf);
        set_param_value_str(params, param, buf);
        const char *hum_param = param_for_report("humidity_pct");
        if (hum_param && s_last_humidity >= 0.0f) {
            set_param_value_number(params, hum_param, s_last_humidity);
        }
        strncpy(summary, buf, sizeof(summary) - 1);
        (void)publish_params(dev->point_id, summary, params);
        cJSON_Delete(params);
        return;
    }

    /* Metering / electrical — require matching CAP (still derived from map clusters). */
    if (evt->kind == MINIHUB_ZB_ATTR_POWER_W || evt->kind == MINIHUB_ZB_ATTR_VOLTAGE_V ||
        evt->kind == MINIHUB_ZB_ATTR_CURRENT_A) {
        if (!(dev->caps & MINIHUB_ZB_CAP_ELECTRICAL)) {
            cJSON_Delete(params);
            return;
        }
    }
    if (evt->kind == MINIHUB_ZB_ATTR_ENERGY_WH) {
        if (!(dev->caps & MINIHUB_ZB_CAP_ENERGY)) {
            cJSON_Delete(params);
            return;
        }
    }

    double prop_val = 0.0;
    if (evt->kind == MINIHUB_ZB_ATTR_POWER_W) {
        prop_val = evt->u.power_w;
    } else if (evt->kind == MINIHUB_ZB_ATTR_VOLTAGE_V) {
        prop_val = evt->u.voltage_v;
    } else if (evt->kind == MINIHUB_ZB_ATTR_CURRENT_A) {
        prop_val = evt->u.current_a;
    } else if (evt->kind == MINIHUB_ZB_ATTR_ENERGY_WH) {
        prop_val = evt->u.energy_wh;
    } else {
        cJSON_Delete(params);
        return;
    }

    ESP_LOGI(TAG, "point %s ← %s %.3f", dev->point_id, param, prop_val);
    set_param_value_number(params, param, prop_val);
    (void)publish_params(dev->point_id, summary[0] ? summary : "off", params);
    cJSON_Delete(params);
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
        const cJSON *ieee = cJSON_GetObjectItem(item, "ieee");
        const cJSON *short_addr = cJSON_GetObjectItem(item, "short_addr");
        const cJSON *endpoint = cJSON_GetObjectItem(item, "endpoint");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }
        minihub_zigbee_device_t dev = {0};
        strncpy(dev.point_id, point_id->valuestring, sizeof(dev.point_id) - 1);
        (void)minihub_zb_normalize_point_id(dev.point_id, sizeof(dev.point_id));
        strncpy(dev.type, type->valuestring, sizeof(dev.type) - 1);
        if (cJSON_IsString(ieee)) {
            (void)minihub_zb_parse_ieee(ieee->valuestring, &dev.ieee);
        }
        if (cJSON_IsNumber(short_addr)) {
            dev.short_addr = (uint16_t)short_addr->valuedouble;
        }
        if (cJSON_IsNumber(endpoint)) {
            dev.endpoint = (uint8_t)endpoint->valuedouble;
        } else {
            dev.endpoint = 1;
        }
        if (strcmp(dev.type, "sensor") == 0) {
            const cJSON *kind = cJSON_GetObjectItem(item, "kind");
            if (cJSON_IsString(kind) && strcmp(kind->valuestring, "contact") == 0) {
                dev.caps = MINIHUB_ZB_CAP_IAS_ZONE;
            } else if (cJSON_IsNumber(cJSON_GetObjectItem(item, "caps"))) {
                dev.caps = (uint8_t)cJSON_GetObjectItem(item, "caps")->valuedouble;
            } else {
                dev.caps = MINIHUB_ZB_CAP_TEMP | MINIHUB_ZB_CAP_HUMIDITY;
            }
        } else {
            dev.caps = MINIHUB_ZB_CAP_ONOFF;
        }
        (void)minihub_zigbee_upsert_device(&dev);
    }
    return ESP_OK;
}

static esp_err_t zigbee_adapter_init(const cJSON *config)
{
    esp_err_t err = minihub_adapter_check_core_api(1, 1);
    if (err != ESP_OK) {
        return err;
    }
    s_config = config;
    (void)minihub_product_params_init();
    (void)minihub_zb_cluster_map_init();
    (void)minihub_zigbee_init();
    apply_config_binds(config);
    (void)minihub_module_register_rpc("zigbee", zigbee_module_rpc);
    return ESP_OK;
}

static esp_err_t zigbee_adapter_start(void)
{
    minihub_zigbee_set_callbacks(on_zb_device, on_zb_attr, NULL);

#if CONFIG_MINIHUB_ZIGBEE_SDK
    esp_err_t err = minihub_zigbee_start();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Host stack start: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "simulation mode (CONFIG_MINIHUB_ZIGBEE_SDK=n)");
#endif

    minihub_zigbee_device_t list[MINIHUB_ZB_MAX_DEVICES];
    size_t n = 0;
    if (minihub_zigbee_list_devices(list, MINIHUB_ZB_MAX_DEVICES, &n) == ESP_OK) {
        for (size_t i = 0; i < n; i++) {
            register_point_from_dev(&list[i]);
        }
    }

    ESP_LOGI(TAG, "adapter started (%u mapped devices)", (unsigned)n);
    (void)s_config;
    return ESP_OK;
}

static esp_err_t zigbee_adapter_stop(void)
{
    return minihub_zigbee_stop();
}

static esp_err_t zigbee_adapter_poll(void)
{
    return ESP_OK;
}

static esp_err_t apply_power_local(const char *point_id, const char *on_off)
{
    char summary[MINIHUB_VALUE_MAX];
    cJSON *params = NULL;
    if (snapshot_params(point_id, summary, sizeof(summary), &params) != ESP_OK) {
        params = cJSON_CreateObject();
        if (!params) {
            return ESP_ERR_NO_MEM;
        }
    }
    const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param("power");
    const char *pname = ctrl ? ctrl->param : "power";
    set_param_value_str(params, pname, on_off);
    esp_err_t err = publish_params(point_id, on_off, params);
    cJSON_Delete(params);
    return err;
}

static esp_err_t resolve_power_target(const minihub_command_t *cmd, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!cmd || out_len < 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        strncpy(out, "on", out_len - 1);
        return ESP_OK;
    }
    if (strcmp(cmd->cmd, "turn_off") == 0) {
        strncpy(out, "off", out_len - 1);
        return ESP_OK;
    }

    const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "parameter") : NULL;
    const bool has_param = cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0];

    /* Bare toggle (no Devices-page parameter): flip from current summary/power. */
    if (strcmp(cmd->cmd, "toggle") == 0 && !has_param) {
        char summary[MINIHUB_VALUE_MAX];
        cJSON *params = NULL;
        bool on = false;
        const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param("power");
        const char *pname = ctrl ? ctrl->param : "power";
        if (snapshot_params(cmd->point_id, summary, sizeof(summary), &params) == ESP_OK) {
            const cJSON *power = cJSON_GetObjectItemCaseSensitive(params, pname);
            const cJSON *pv = power ? cJSON_GetObjectItemCaseSensitive(power, "value") : NULL;
            if (cJSON_IsString(pv) && pv->valuestring) {
                on = power_str_on(pv->valuestring);
            } else if (cJSON_IsBool(pv)) {
                on = cJSON_IsTrue(pv);
            } else {
                on = power_str_on(summary);
            }
            cJSON_Delete(params);
        }
        strncpy(out, on ? "off" : "on", out_len - 1);
        return ESP_OK;
    }

    /* set / toggle with params.parameter (+ value) — Devices page / REST control */
    if (!cmd->params) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *param = has_param ? pj->valuestring : "power";
    const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param(param);
    if (!ctrl || ctrl->kind != MINIHUB_ZB_CTRL_ON_OFF) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char buf[MINIHUB_VALUE_MAX];
    json_to_str(cJSON_GetObjectItem(cmd->params, "value"), buf, sizeof(buf));
    if (buf[0] == '\0') {
        if (strcmp(cmd->cmd, "toggle") == 0) {
            minihub_command_t bare = *cmd;
            bare.params = NULL;
            return resolve_power_target(&bare, out, out_len);
        }
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(out, power_str_on(buf) ? "on" : "off", out_len - 1);
    return ESP_OK;
}

static esp_err_t handle_mapped_control(const minihub_command_t *cmd, const minihub_zigbee_device_t *dev,
                                       const minihub_zb_map_control_t *ctrl)
{
    if (!cmd || !ctrl) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctrl->kind == MINIHUB_ZB_CTRL_MOVE_TO_LEVEL) {
        if (!dev) {
            return ESP_ERR_NOT_FOUND;
        }
        char buf[MINIHUB_VALUE_MAX];
        json_to_str(cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL, buf, sizeof(buf));
        if (buf[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        int level_pct = atoi(buf);
        if (level_pct < 0) {
            level_pct = 0;
        }
        if (level_pct > 100) {
            level_pct = 100;
        }
        /* ZCL level is 0..254 */
        uint8_t level = (uint8_t)((level_pct * 254) / 100);
        esp_err_t err = zigbee_stack_move_to_level(dev->short_addr, dev->endpoint, level, 0);
        if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "control '%s' mapped to move_to_level but stack has no writer yet",
                     ctrl->param);
        }
        return err;
    }

    if (ctrl->kind == MINIHUB_ZB_CTRL_WRITE_ATTR) {
        if (!dev) {
            return ESP_ERR_NOT_FOUND;
        }
        const cJSON *value = cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL;
        if (!value) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t u8 = 0;
        uint16_t u16 = 0;
        int16_t s16 = 0;
        void *pval = &u8;
        uint16_t psz = 1;
        if (ctrl->zcl_type == 0x10 || ctrl->zcl_type == 0x20) {
            u8 = cJSON_IsBool(value) ? (uint8_t)cJSON_IsTrue(value) : (uint8_t)value->valuedouble;
            pval = &u8;
            psz = 1;
        } else if (ctrl->zcl_type == 0x21) {
            u16 = (uint16_t)value->valuedouble;
            pval = &u16;
            psz = 2;
        } else if (ctrl->zcl_type == 0x29) {
            s16 = (int16_t)value->valuedouble;
            pval = &s16;
            psz = 2;
        }
        return zigbee_stack_write_attr(dev->short_addr, dev->endpoint, ctrl->cluster, ctrl->attr,
                                       ctrl->zcl_type, pval, psz);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t zigbee_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!cmd || cmd->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
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
        /* Local LittleFS path (props.ota.file) is for the plugin/demo cache.
         * ZCL 0x0019 air transport is still stubbed in minihub_zigbee_ota_offer. */
        return minihub_zigbee_ota_offer(cmd->point_id, path);
    }

    if (strcmp(cmd->cmd, "set_prop") == 0) {
        const cJSON *prop = cmd->params ? cJSON_GetObjectItem(cmd->params, "prop") : NULL;
        const cJSON *value = cmd->params ? cJSON_GetObjectItem(cmd->params, "value") : NULL;
        if (!cJSON_IsString(prop) || !value) {
            return ESP_ERR_INVALID_ARG;
        }
        minihub_pp_zb_param_t zb = {0};
        bool is_zb = false;
        esp_err_t err = minihub_pp_set_prop(cmd->point_id, prop->valuestring, value, &zb, &is_zb);
        if (err != ESP_OK) {
            return err;
        }
        if (is_zb) {
            const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_point(cmd->point_id);
            if (!dev) {
                return ESP_ERR_NOT_FOUND;
            }
            uint8_t u8 = 0;
            uint16_t u16 = 0;
            int16_t s16 = 0;
            void *pval = &u8;
            uint16_t psz = 1;
            if (zb.zcl_type == 0x10 || zb.zcl_type == 0x20) {
                u8 = cJSON_IsBool(value) ? (uint8_t)cJSON_IsTrue(value) : (uint8_t)value->valuedouble;
                pval = &u8;
                psz = 1;
            } else if (zb.zcl_type == 0x21) {
                u16 = (uint16_t)value->valuedouble;
                pval = &u16;
                psz = 2;
            } else if (zb.zcl_type == 0x29) {
                s16 = (int16_t)value->valuedouble;
                pval = &s16;
                psz = 2;
            }
            return zigbee_stack_write_attr(dev->short_addr, dev->endpoint, zb.cluster, zb.attr_id,
                                           zb.zcl_type, pval, psz);
        }
        return ESP_OK;
    }

    const bool is_set_family =
        strcmp(cmd->cmd, "set") == 0 || strcmp(cmd->cmd, "toggle") == 0 ||
        strcmp(cmd->cmd, "turn_on") == 0 || strcmp(cmd->cmd, "turn_off") == 0;
    if (!is_set_family) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON *pj = cmd->params ? cJSON_GetObjectItem(cmd->params, "parameter") : NULL;
    const char *param_name = NULL;
    if (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0]) {
        param_name = pj->valuestring;
    }

    /* Non-on_off mapped controls (brightness / write_attr). */
    if (param_name) {
        const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param(param_name);
        if (ctrl && ctrl->kind != MINIHUB_ZB_CTRL_ON_OFF) {
            const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_point(cmd->point_id);
            return handle_mapped_control(cmd, dev, ctrl);
        }
        if (!ctrl) {
            ESP_LOGW(TAG, "parameter '%s' not in control map on %s", param_name, cmd->point_id);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    const minihub_zigbee_device_t *dev = minihub_zigbee_find_by_point(cmd->point_id);
#if !CONFIG_MINIHUB_ZIGBEE_SDK
    if (!dev) {
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(cmd->point_id, &snap) != ESP_OK) {
            return ESP_ERR_NOT_FOUND;
        }
        if (!type_supports_onoff(snap.type)) {
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        char target[8];
        esp_err_t r = resolve_power_target(cmd, target, sizeof(target));
        if (r != ESP_OK) {
            return r;
        }
        return apply_power_local(cmd->point_id, target);
    }
#endif
    if (!dev) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!type_supports_onoff(dev->type)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char target[8];
    esp_err_t r = resolve_power_target(cmd, target, sizeof(target));
    if (r != ESP_OK) {
        return r;
    }

    minihub_zigbee_onoff_cmd_t zb_cmd;
    if (strcmp(cmd->cmd, "toggle") == 0 &&
        (!cmd->params || !cJSON_GetObjectItem(cmd->params, "parameter"))) {
        zb_cmd = MINIHUB_ZB_ONOFF_TOGGLE;
    } else if (strcmp(target, "on") == 0) {
        zb_cmd = MINIHUB_ZB_ONOFF_ON;
    } else {
        zb_cmd = MINIHUB_ZB_ONOFF_OFF;
    }

#if CONFIG_MINIHUB_ZIGBEE_SDK
    {
        esp_err_t err = minihub_zigbee_onoff(dev->short_addr, dev->endpoint, zb_cmd);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif
    if (zb_cmd == MINIHUB_ZB_ONOFF_TOGGLE) {
        /* Optimistic flip after ZCL toggle */
        char summary[MINIHUB_VALUE_MAX];
        cJSON *params = NULL;
        if (snapshot_params(cmd->point_id, summary, sizeof(summary), &params) == ESP_OK) {
            const char *next = power_str_on(summary) ? "off" : "on";
            const minihub_zb_map_control_t *ctrl = minihub_zb_map_control_for_param("power");
            const char *pname = ctrl ? ctrl->param : "power";
            set_param_value_str(params, pname, next);
            esp_err_t err = publish_params(cmd->point_id, next, params);
            cJSON_Delete(params);
            return err;
        }
        cJSON_Delete(params);
        return apply_power_local(cmd->point_id, target);
    }
    return apply_power_local(cmd->point_id, target);
}

const minihub_adapter_t zigbee_adapter = {
    .name = "zigbee",
    .version = "0.5.0",
    .init = zigbee_adapter_init,
    .start = zigbee_adapter_start,
    .stop = zigbee_adapter_stop,
    .poll = zigbee_adapter_poll,
    .handle_command = zigbee_adapter_handle_command,
};

REGISTER_ADAPTER(zigbee_adapter);
