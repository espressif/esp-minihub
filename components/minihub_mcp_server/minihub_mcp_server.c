/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_mcp_server.h"

#include "cap_mcp_server.h"
#include "hub_api_gateway.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_hub.h"
#include "minihub_module.h"
#include "minihub_paths.h"
#include "minihub_rules.h"
#include "minihub_store.h"

#include "esp_log.h"
#include "esp_mcp_data.h"
#include "esp_mcp_property.h"
#include "esp_mcp_property_priv.h"
#include "esp_timer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "minihub_mcp";

static bool s_hub_ready;
static int64_t s_last_hub_reload_ms;

static esp_mcp_value_t mcp_return_internal_error(void)
{
    return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"INTERNAL\"}");
}

static esp_mcp_value_t mcp_return_json(cJSON *root)
{
    if (!root) {
        return mcp_return_internal_error();
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return mcp_return_internal_error();
    }

    esp_mcp_value_t result = esp_mcp_value_create_string(json_str);
    free(json_str);
    return result;
}

static esp_mcp_value_t mcp_error(const char *code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", code);
    return mcp_return_json(root);
}

static esp_mcp_value_t mcp_hub_not_ready(void)
{
    return mcp_error("HUB_NOT_READY");
}

static cJSON *point_to_mcp_json(const minihub_point_t *point)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "point_id", point->point_id);
    cJSON_AddStringToObject(obj, "type", point->type);
    cJSON_AddStringToObject(obj, "value", point->value);
    cJSON_AddStringToObject(obj, "adapter", point->adapter);
    cJSON_AddBoolToObject(obj, "online", point->online);
    return obj;
}

static bool point_matches_filters(const minihub_point_t *point, const char *type, const char *adapter)
{
    if (type && type[0] && strcmp(point->type, type) != 0) {
        return false;
    }
    if (adapter && adapter[0] && strcmp(point->adapter, adapter) != 0) {
        return false;
    }
    return true;
}

static esp_mcp_value_t tool_points_list(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *type = esp_mcp_property_list_get_property_string(properties, "type");
    const char *adapter = esp_mcp_property_list_get_property_string(properties, "adapter");

    minihub_point_t *points = NULL;
    size_t count = 0;
    esp_err_t err;

    if (type && type[0] && (!adapter || !adapter[0])) {
        err = minihub_point_list_by_type(type, &points, &count);
    } else if (adapter && adapter[0] && (!type || !type[0])) {
        err = minihub_point_list_by_adapter(adapter, &points, &count);
    } else {
        err = minihub_point_list(&points, &count);
    }

    if (err != ESP_OK) {
        return mcp_error("LIST_FAILED");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        minihub_point_list_free(points, count);
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return mcp_error("INTERNAL");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "points", arr);

    for (size_t i = 0; i < count; i++) {
        if (!point_matches_filters(&points[i], type, adapter)) {
            continue;
        }
        cJSON *item = point_to_mcp_json(&points[i]);
        if (item) {
            cJSON_AddItemToArray(arr, item);
        }
    }
    minihub_point_list_free(points, count);

    return mcp_return_json(root);
}

static esp_mcp_value_t tool_points_get(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *point_id = esp_mcp_property_list_get_property_string(properties, "point_id");
    if (!point_id || !point_id[0]) {
        return mcp_error("INVALID_ARG");
    }

    minihub_point_t snap = {0};
    esp_err_t err = minihub_point_get_snapshot(point_id, &snap);
    if (err != ESP_OK) {
        return mcp_error("NOT_FOUND");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *point = point_to_mcp_json(&snap);
    if (snap.props) {
        cJSON_Delete(snap.props);
        snap.props = NULL;
    }
    if (!root || !point) {
        cJSON_Delete(root);
        cJSON_Delete(point);
        return mcp_error("INTERNAL");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "point", point);
    return mcp_return_json(root);
}

static bool point_type_power_controllable(const char *type)
{
    static const char *const types[] = {
        "switch", "light", "cover", "camera", "appliance", "fridge",
        "water_heater", "water_purifier", "fan", "climate",
    };
    if (!type || !type[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcmp(type, types[i]) == 0) {
            return true;
        }
    }
    return false;
}

/** Map on/off style writes to adapter command.invoke (points.set alone is registry-only). */
static esp_err_t try_power_command_from_value(const char *point_id, const char *type,
                                              const char *value, bool *handled_out)
{
    char norm[16];
    size_t n = 0;

    if (handled_out) {
        *handled_out = false;
    }
    if (!point_id || !value || !point_type_power_controllable(type)) {
        return ESP_OK;
    }

    for (n = 0; value[n] && n + 1 < sizeof(norm); n++) {
        norm[n] = (char)tolower((unsigned char)value[n]);
    }
    norm[n] = '\0';

    const char *cmd = NULL;
    if (strcmp(norm, "on") == 0 || strcmp(norm, "true") == 0 || strcmp(norm, "1") == 0) {
        cmd = "turn_on";
    } else if (strcmp(norm, "off") == 0 || strcmp(norm, "false") == 0 || strcmp(norm, "0") == 0) {
        cmd = "turn_off";
    } else {
        return ESP_OK;
    }

    minihub_command_t command = {0};
    strncpy(command.point_id, point_id, sizeof(command.point_id) - 1);
    strncpy(command.cmd, cmd, sizeof(command.cmd) - 1);
    if (handled_out) {
        *handled_out = true;
    }
    return minihub_command_invoke(&command);
}

static const char *history_label_for_value(const char *value)
{
    if (!value || !value[0]) {
        return "event";
    }
    if (strcmp(value, "open") == 0) {
        return "open";
    }
    if (strcmp(value, "closed") == 0) {
        return "closed";
    }
    if (strcmp(value, "detected") == 0) {
        return "motion detected";
    }
    if (strcmp(value, "clear") == 0) {
        return "clear";
    }
    if (strcmp(value, "on") == 0) {
        return "on";
    }
    if (strcmp(value, "off") == 0) {
        return "off";
    }
    return value;
}

static bool history_near_duplicate(const cJSON *items, int64_t ts_ms, const char *label)
{
    if (!cJSON_IsArray(items) || !label) {
        return false;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        const cJSON *ts = cJSON_GetObjectItem(it, "ts_ms");
        const cJSON *lab = cJSON_GetObjectItem(it, "label");
        if (!cJSON_IsNumber(ts) || !cJSON_IsString(lab)) {
            continue;
        }
        int64_t other = (int64_t)ts->valuedouble;
        if (llabs(other - ts_ms) < 2000 && strcmp(lab->valuestring, label) == 0) {
            return true;
        }
    }
    return false;
}

static int history_item_cmp_desc(const void *a, const void *b)
{
    const cJSON *left = *(const cJSON *const *)a;
    const cJSON *right = *(const cJSON *const *)b;
    double lts = cJSON_GetObjectItem(left, "ts_ms")
                     ? cJSON_GetObjectItem(left, "ts_ms")->valuedouble
                     : 0.0;
    double rts = cJSON_GetObjectItem(right, "ts_ms")
                     ? cJSON_GetObjectItem(right, "ts_ms")->valuedouble
                     : 0.0;
    if (lts < rts) {
        return 1;
    }
    if (lts > rts) {
        return -1;
    }
    return 0;
}

static void history_sort_items_desc(cJSON *items)
{
    if (!cJSON_IsArray(items)) {
        return;
    }
    int n = cJSON_GetArraySize(items);
    if (n < 2) {
        return;
    }
    cJSON **ptrs = calloc((size_t)n, sizeof(*ptrs));
    if (!ptrs) {
        return;
    }
    for (int i = 0; i < n; i++) {
        ptrs[i] = cJSON_DetachItemFromArray(items, 0);
    }
    qsort(ptrs, (size_t)n, sizeof(*ptrs), history_item_cmp_desc);
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(items, ptrs[i]);
    }
    free(ptrs);
}

/**
 * Event log for a point: local Hub ring + optional module device_log
 * (same sources as Web log modal).
 */
static esp_mcp_value_t tool_points_history(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *point_id = esp_mcp_property_list_get_property_string(properties, "point_id");
    if (!point_id || !point_id[0]) {
        return mcp_error("INVALID_ARG");
    }

    size_t limit = 20;
    const char *limit_s = esp_mcp_property_list_get_property_string(properties, "limit");
    if (limit_s && limit_s[0]) {
        int v = atoi(limit_s);
        if (v > 0) {
            limit = (size_t)v;
        }
    }
    if (limit > 50) {
        limit = 50;
    }

    cJSON *local_wrap = minihub_point_history_json(point_id, limit);
    cJSON *local_items = local_wrap ? cJSON_GetObjectItem(local_wrap, "items") : NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        cJSON_Delete(local_wrap);
        return mcp_error("INTERNAL");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "point_id", point_id);
    cJSON_AddItemToObject(root, "items", items);

    if (cJSON_IsArray(local_items)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, local_items) {
            const cJSON *ts = cJSON_GetObjectItem(it, "ts_ms");
            const cJSON *oldv = cJSON_GetObjectItem(it, "old");
            const cJSON *newv = cJSON_GetObjectItem(it, "new");
            cJSON *row = cJSON_CreateObject();
            if (!row) {
                continue;
            }
            if (cJSON_IsNumber(ts)) {
                cJSON_AddNumberToObject(row, "ts_ms", ts->valuedouble);
            }
            if (cJSON_IsString(oldv)) {
                cJSON_AddStringToObject(row, "old", oldv->valuestring);
            }
            if (cJSON_IsString(newv)) {
                cJSON_AddStringToObject(row, "new", newv->valuestring);
                cJSON_AddStringToObject(row, "label", history_label_for_value(newv->valuestring));
            } else {
                cJSON_AddStringToObject(row, "label", "event");
            }
            cJSON_AddStringToObject(row, "source", "local");
            cJSON_AddItemToArray(items, row);
        }
    }
    cJSON_Delete(local_wrap);

    cJSON *cloud = NULL;
    const minihub_point_t *pt = minihub_point_get(point_id);
    const char *adapter = (pt && pt->adapter[0]) ? pt->adapter : NULL;
    if (adapter) {
        cJSON *rpc_params = cJSON_CreateObject();
        if (rpc_params) {
            cJSON_AddStringToObject(rpc_params, "point_id", point_id);
            cJSON_AddNumberToObject(rpc_params, "limit", (double)limit);
            esp_err_t rpc_err = minihub_module_invoke_rpc(adapter, "device_log", rpc_params, &cloud);
            cJSON_Delete(rpc_params);
            if (rpc_err != ESP_OK) {
                cJSON_AddStringToObject(root, "cloud_error",
                                        rpc_err == ESP_ERR_INVALID_STATE ? "module_not_ready"
                                                                         : esp_err_to_name(rpc_err));
            } else if (cloud) {
                const cJSON *cerr = cJSON_GetObjectItem(cloud, "cloud_error");
                if (cJSON_IsString(cerr) && cerr->valuestring[0]) {
                    cJSON_AddStringToObject(root, "cloud_error", cerr->valuestring);
                }
                const cJSON *citems = cJSON_GetObjectItem(cloud, "items");
                if (cJSON_IsArray(citems)) {
                    const cJSON *cit = NULL;
                    cJSON_ArrayForEach(cit, citems) {
                        const cJSON *ts = cJSON_GetObjectItem(cit, "ts_ms");
                        const cJSON *lab = cJSON_GetObjectItem(cit, "label");
                        if (!cJSON_IsNumber(ts)) {
                            continue;
                        }
                        int64_t ts_ms = (int64_t)ts->valuedouble;
                        const char *label =
                            (cJSON_IsString(lab) && lab->valuestring[0]) ? lab->valuestring : "event";
                        if (history_near_duplicate(items, ts_ms, label)) {
                            continue;
                        }
                        cJSON *row = cJSON_Duplicate(cit, 1);
                        if (!row) {
                            continue;
                        }
                        if (!cJSON_GetObjectItem(row, "source")) {
                            cJSON_AddStringToObject(row, "source", "cloud");
                        }
                        if (!cJSON_GetObjectItem(row, "label")) {
                            cJSON_AddStringToObject(row, "label", label);
                        }
                        cJSON_AddItemToArray(items, row);
                    }
                }
                cJSON_Delete(cloud);
            }
        }
    }

    history_sort_items_desc(items);
    /* Trim to limit after merge. */
    while (cJSON_GetArraySize(items) > (int)limit) {
        cJSON *drop = cJSON_DetachItemFromArray(items, cJSON_GetArraySize(items) - 1);
        cJSON_Delete(drop);
    }

    return mcp_return_json(root);
}

static esp_mcp_value_t tool_hub_api(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *method = esp_mcp_property_list_get_property_string(properties, "method");
    const char *path = esp_mcp_property_list_get_property_string(properties, "path");
    const char *query = esp_mcp_property_list_get_property_string(properties, "query");
    const char *body = esp_mcp_property_list_get_property_string(properties, "body");

    if (!method || !method[0] || !path || !path[0]) {
        return mcp_error("INVALID_ARG");
    }

    cJSON *resp = NULL;
    esp_err_t err = hub_api_call(method, path, query, body, &resp);
    if (err != ESP_OK || !resp) {
        cJSON_Delete(resp);
        return mcp_error("INTERNAL");
    }
    return mcp_return_json(resp);
}

static esp_mcp_value_t tool_points_set(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *point_id = esp_mcp_property_list_get_property_string(properties, "point_id");
    const char *value = esp_mcp_property_list_get_property_string(properties, "value");
    if (!point_id || !point_id[0] || !value) {
        return mcp_error("INVALID_ARG");
    }

    minihub_point_t snap = {0};
    esp_err_t snap_err = minihub_point_get_snapshot(point_id, &snap);
    if (snap_err != ESP_OK) {
        return mcp_error("NOT_FOUND");
    }

    bool handled = false;
    esp_err_t err = try_power_command_from_value(point_id, snap.type, value, &handled);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    if (handled) {
        if (err != ESP_OK) {
            return mcp_error("INVOKE_FAILED");
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        return mcp_return_json(root);
    }

    err = minihub_point_set(point_id, value, NULL);
    if (err != ESP_OK) {
        return mcp_error("SET_FAILED");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return mcp_return_json(root);
}

static esp_mcp_value_t tool_command_invoke(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *point_id = esp_mcp_property_list_get_property_string(properties, "point_id");
    const char *cmd = esp_mcp_property_list_get_property_string(properties, "cmd");
    const char *params_json = esp_mcp_property_list_get_property_string(properties, "params_json");
    if (!point_id || !point_id[0] || !cmd || !cmd[0]) {
        return mcp_error("INVALID_ARG");
    }

    minihub_command_t command = {0};
    strncpy(command.point_id, point_id, sizeof(command.point_id) - 1);
    strncpy(command.cmd, cmd, sizeof(command.cmd) - 1);

    if (params_json && params_json[0]) {
        cJSON *params = cJSON_Parse(params_json);
        if (!params || !cJSON_IsObject(params)) {
            cJSON_Delete(params);
            return mcp_error("INVALID_PARAMS");
        }
        command.params = params;
    }

    esp_err_t err = minihub_command_invoke(&command);
    if (command.params) {
        cJSON_Delete(command.params);
    }
    if (err != ESP_OK) {
        return mcp_error("INVOKE_FAILED");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return mcp_return_json(root);
}

static esp_mcp_value_t mcp_wrap_config_object(cJSON *cfg)
{
    if (!cfg) {
        return mcp_error("CONFIG_NOT_READY");
    }

    cJSON *root = cJSON_Duplicate(cfg, true);
    if (!root) {
        return mcp_error("INTERNAL");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return mcp_return_json(root);
}

static esp_mcp_value_t tool_rules_list(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    return mcp_wrap_config_object(minihub_config_get_rules());
}

static esp_mcp_value_t tool_rules_put(const esp_mcp_property_list_t *properties)
{
    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    const char *rules_json = esp_mcp_property_list_get_property_string(properties, "rules_json");
    if (!rules_json || !rules_json[0]) {
        return mcp_error("INVALID_ARG");
    }

    cJSON *root = cJSON_Parse(rules_json);
    if (!root) {
        return mcp_error("INVALID_JSON");
    }

    const cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (!cJSON_IsArray(rules)) {
        cJSON_Delete(root);
        return mcp_error("VALIDATION_FAILED");
    }

    int count = cJSON_GetArraySize(rules);

    /* Validate by reloading in-memory rules first; persist only after success. */
    esp_err_t err = minihub_rules_reload(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return mcp_error("RELOAD_FAILED");
    }

    err = minihub_config_save_rules(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)minihub_rules_reload(minihub_config_get_rules());
        return mcp_error("SAVE_FAILED");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "count", count);
    return mcp_return_json(resp);
}

static esp_mcp_value_t tool_adapters_get(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    return mcp_wrap_config_object(minihub_config_get_adapters());
}

static esp_mcp_value_t tool_hub_reload(const esp_mcp_property_list_t *properties)
{
    (void)properties;

    if (!minihub_mcp_hub_ready()) {
        return mcp_hub_not_ready();
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_hub_reload_ms != 0 && (now_ms - s_last_hub_reload_ms) < 5000) {
        return mcp_error("RATE_LIMITED");
    }

    minihub_mcp_set_hub_ready(false);

    esp_err_t err = minihub_hub_reload_async();
    if (err != ESP_OK) {
        minihub_mcp_set_hub_ready(true);
        return mcp_error("RELOAD_FAILED");
    }
    s_last_hub_reload_ms = now_ms;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return mcp_return_json(root);
}

static bool mcp_truthy(const char *s)
{
    return s && s[0] && (strcmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                         strcmp(s, "yes") == 0);
}

static cJSON *store_config_to_json(void)
{
    char custom[MINIHUB_STORE_BASE_URL_MAX];
    char def_url[MINIHUB_STORE_BASE_URL_MAX];
    bool disable_default = false;

    custom[0] = '\0';
    def_url[0] = '\0';
    if (minihub_store_get_config(custom, sizeof(custom), &disable_default,
                                 def_url, sizeof(def_url)) != ESP_OK) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "custom_url", custom);
    cJSON_AddBoolToObject(root, "disable_default", disable_default);
    cJSON_AddStringToObject(root, "default_url", def_url);
    return root;
}

static esp_mcp_value_t tool_store_config_get(const esp_mcp_property_list_t *properties)
{
    (void)properties;
    cJSON *root = store_config_to_json();
    if (!root) {
        return mcp_error("STORE_CONFIG_FAILED");
    }
    return mcp_return_json(root);
}

static esp_mcp_value_t tool_store_config_set(const esp_mcp_property_list_t *properties)
{
    const char *custom_url = esp_mcp_property_list_get_property_string(properties, "custom_url");
    const char *dis = esp_mcp_property_list_get_property_string(properties, "disable_default");
    bool disable_default = false;
    if (dis && dis[0]) {
        disable_default = mcp_truthy(dis);
    } else {
        (void)minihub_store_get_config(NULL, 0, &disable_default, NULL, 0);
    }

    if (!custom_url) {
        custom_url = "";
    }

    esp_err_t err = minihub_store_set_config(custom_url, disable_default);
    if (err == ESP_ERR_INVALID_ARG) {
        return mcp_error("INVALID_ARG");
    }
    if (err != ESP_OK) {
        return mcp_error("STORE_CONFIG_FAILED");
    }

    cJSON *root = store_config_to_json();
    if (!root) {
        return mcp_error("STORE_CONFIG_FAILED");
    }
    return mcp_return_json(root);
}

static const cap_mcp_server_tool_def_t s_tools[] = {
    {
        .name = "hub.api",
        .description =
            "Allowlisted MiniHub Host REST (/api/*). method+path required; optional query/body. "
            "Includes OTA URL, hub.reload, zigbee/ble_mesh permit, Store install/upgrade/uninstall. "
            "Denied: auth RPC (login/logout/session/verify), binary OTA upload.",
        .callback = tool_hub_api,
        .property_names = {"method", "path", "query", "body"},
        .property_count = 4,
    },
    {
        .name = "points.list",
        .description = "List MiniHub points (optional type/adapter filters).",
        .callback = tool_points_list,
        .property_names = {"type", "adapter"},
        .property_count = 2,
    },
    {
        .name = "points.get",
        .description = "Get one MiniHub point snapshot.",
        .callback = tool_points_get,
        .property_names = {"point_id"},
        .property_count = 1,
    },
    {
        .name = "points.history",
        .description =
            "Event log for a point (local Hub transitions + module device_log when available).",
        .callback = tool_points_history,
        .property_names = {"point_id", "limit"},
        .property_count = 2,
    },
    {
        .name = "points.set",
        .description = "Set a MiniHub point value.",
        .callback = tool_points_set,
        .property_names = {"point_id", "value"},
        .property_count = 2,
    },
    {
        .name = "command.invoke",
        .description = "Invoke a MiniHub adapter command on a point.",
        .callback = tool_command_invoke,
        .property_names = {"point_id", "cmd", "params_json"},
        .property_count = 3,
    },
    {
        .name = "rules.list",
        .description = "Read current automation rules JSON from config storage.",
        .callback = tool_rules_list,
        .property_names = {""},
        .property_count = 0,
    },
    {
        .name = "rules.put",
        .description = "Replace MiniHub automation rules JSON and hot-reload (point.updated events; not Claw router).",
        .callback = tool_rules_put,
        .property_names = {"rules_json"},
        .property_count = 1,
    },
    {
        .name = "adapters.get",
        .description = "Read-only adapters config snippet from storage.",
        .callback = tool_adapters_get,
        .property_names = {""},
        .property_count = 0,
    },
    {
        .name = "hub.reload",
        .description = "Soft-reload hub config/adapters (rate-limited to once per 5s).",
        .callback = tool_hub_reload,
        .property_names = {""},
        .property_count = 0,
    },
    {
        .name = "store.config.get",
        .description = "Read MiniHub Store server URL (NVS custom_url and disable_default).",
        .callback = tool_store_config_get,
        .property_names = {""},
        .property_count = 0,
    },
    {
        .name = "store.config.set",
        .description = "Save MiniHub Store base URL. custom_url is http(s)://host:port; empty clears. disable_default is true/false.",
        .callback = tool_store_config_set,
        .property_names = {"custom_url", "disable_default"},
        .property_count = 2,
    },
};

void minihub_mcp_set_hub_ready(bool ready)
{
    s_hub_ready = ready;
    ESP_LOGI(TAG, "hub ready: %s", ready ? "true" : "false");
}

void minihub_hub_reload_done(esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hub reload finished with error: %s", esp_err_to_name(err));
    }
    minihub_mcp_set_hub_ready(true);
}

bool minihub_mcp_hub_ready(void)
{
    return s_hub_ready;
}

esp_err_t minihub_mcp_server_init(void)
{
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

esp_err_t minihub_mcp_server_register_tools(void)
{
    esp_err_t err = cap_mcp_server_add_tool(s_tools, sizeof(s_tools) / sizeof(s_tools[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register tools failed: %s", esp_err_to_name(err));
    }
    return err;
}

static const char *json_arg_as_string(const cJSON *args, const char *key, char *scratch,
                                      size_t scratch_len)
{
    const cJSON *item;

    if (!args || !key || !scratch || scratch_len == 0) {
        return "";
    }
    scratch[0] = '\0';
    item = cJSON_GetObjectItem(args, key);
    if (!item || cJSON_IsNull(item)) {
        return "";
    }
    if (cJSON_IsString(item)) {
        return item->valuestring ? item->valuestring : "";
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble == (double)item->valueint) {
            snprintf(scratch, scratch_len, "%d", item->valueint);
        } else {
            snprintf(scratch, scratch_len, "%g", item->valuedouble);
        }
        return scratch;
    }
    if (cJSON_IsBool(item)) {
        strlcpy(scratch, cJSON_IsTrue(item) ? "true" : "false", scratch_len);
        return scratch;
    }
    {
        char *printed = cJSON_PrintUnformatted(item);
        if (!printed) {
            return "";
        }
        strlcpy(scratch, printed, scratch_len);
        free(printed);
        return scratch;
    }
}

static esp_err_t value_to_out(esp_mcp_value_t *value, char *out, size_t out_len)
{
    if (!value || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value->type == ESP_MCP_VALUE_TYPE_STRING && value->data.string_value) {
        strlcpy(out, value->data.string_value, out_len);
    } else if (value->type == ESP_MCP_VALUE_TYPE_BOOLEAN) {
        strlcpy(out, value->data.bool_value ? "true" : "false", out_len);
    } else if (value->type == ESP_MCP_VALUE_TYPE_INTEGER) {
        snprintf(out, out_len, "%d", value->data.int_value);
    } else if (value->type == ESP_MCP_VALUE_TYPE_FLOAT) {
        snprintf(out, out_len, "%g", (double)value->data.float_value);
    } else {
        strlcpy(out, "{\"ok\":false,\"error\":\"INTERNAL\"}", out_len);
    }
    (void)esp_mcp_value_destroy(value);
    return ESP_OK;
}

esp_err_t minihub_mcp_local_call_tool(const char *tool_name, const char *arguments_json,
                                      char *out, size_t out_len)
{
    const cap_mcp_server_tool_def_t *tool = NULL;
    esp_mcp_property_list_t *props = NULL;
    cJSON *args = NULL;
    esp_mcp_value_t value;
    char scratch[512];
    esp_err_t err = ESP_OK;

    if (!tool_name || !tool_name[0] || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    for (size_t i = 0; i < sizeof(s_tools) / sizeof(s_tools[0]); i++) {
        if (strcmp(s_tools[i].name, tool_name) == 0) {
            tool = &s_tools[i];
            break;
        }
    }
    if (!tool || !tool->callback) {
        snprintf(out, out_len, "{\"ok\":false,\"error\":\"UNKNOWN_TOOL\",\"tool\":\"%s\"}",
                 tool_name);
        return ESP_ERR_NOT_FOUND;
    }

    if (arguments_json && arguments_json[0]) {
        args = cJSON_Parse(arguments_json);
        if (!args || !cJSON_IsObject(args)) {
            cJSON_Delete(args);
            strlcpy(out, "{\"ok\":false,\"error\":\"INVALID_ARG\"}", out_len);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        args = cJSON_CreateObject();
        if (!args) {
            return ESP_ERR_NO_MEM;
        }
    }

    props = esp_mcp_property_list_create();
    if (!props) {
        cJSON_Delete(args);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < tool->property_count; i++) {
        const char *pname = tool->property_names[i];
        const char *pval;
        esp_mcp_property_t *prop;

        if (!pname || !pname[0]) {
            continue;
        }
        pval = json_arg_as_string(args, pname, scratch, sizeof(scratch));
        prop = esp_mcp_property_create_with_string(pname, pval ? pval : "");
        if (!prop) {
            err = ESP_ERR_NO_MEM;
            goto done;
        }
        err = esp_mcp_property_list_add_property(props, prop);
        if (err != ESP_OK) {
            (void)esp_mcp_property_destroy(prop);
            goto done;
        }
    }

    value = tool->callback(props);
    err = value_to_out(&value, out, out_len);

done:
    if (props) {
        (void)esp_mcp_property_list_destroy(props);
    }
    cJSON_Delete(args);
    return err;
}

esp_err_t minihub_mcp_local_list_tools(char *out, size_t out_len)
{
    size_t offset = 0;

    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    for (size_t i = 0; i < sizeof(s_tools) / sizeof(s_tools[0]); i++) {
        int n = snprintf(out + offset, out_len - offset, "%s: %s\n",
                         s_tools[i].name,
                         s_tools[i].description ? s_tools[i].description : "");
        if (n < 0 || (size_t)n >= out_len - offset) {
            out[out_len - 1] = '\0';
            return ESP_OK;
        }
        offset += (size_t)n;
    }
    return ESP_OK;
}
