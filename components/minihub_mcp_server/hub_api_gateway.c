/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Allowlisted Hub REST gateway for MCP (Host Web /api subset).
 * Module RPC gate: allowlist includes login/logout/2FA/session for account bind.
 */

#include "hub_api_gateway.h"

#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_hub.h"
#include "minihub_module.h"
#include "minihub_net.h"
#include "minihub_ota.h"
#include "minihub_rules.h"
#include "minihub_store.h"
#if CONFIG_MINIHUB_ZIGBEE_SDK
#include "minihub_zigbee.h"
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
#include "minihub_ble_mesh.h"
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
#include "minihub_matter.h"
#endif

#include "esp_log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "hub_api";

typedef enum {
    HUB_M_GET = 1,
    HUB_M_POST = 2,
    HUB_M_PUT = 4,
} hub_method_mask_t;

typedef struct {
    uint8_t methods;
    const char *path; /* exact, or modules/<id>/rpc pattern with one star segment */
} hub_api_rule_t;

static const hub_api_rule_t s_rules[] = {
    {HUB_M_GET, "/api/"},
    {HUB_M_GET, "/api/system"},
    {HUB_M_GET, "/api/points"},
    {HUB_M_GET, "/api/points/history"},
    {HUB_M_GET, "/api/rules"},
    {HUB_M_GET, "/api/adapters"},
    {HUB_M_GET, "/api/config/adapters"},
    {HUB_M_PUT, "/api/config/adapters"},
    {HUB_M_GET, "/api/location"},
    {HUB_M_GET, "/api/modules"},
    {HUB_M_GET, "/api/packs"},
    {HUB_M_GET, "/api/scenes"},
    {HUB_M_GET, "/api/events"},
    {HUB_M_GET, "/api/store/catalog"},
    {HUB_M_GET, "/api/store/config"},
#if CONFIG_MINIHUB_MATTER_ADAPTER
    {HUB_M_GET, "/api/matter/status"},
    {HUB_M_POST, "/api/matter/commission"},
    {HUB_M_POST, "/api/matter/devices"},
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    {HUB_M_GET, "/api/thread/status"},
    {HUB_M_GET, "/api/thread/dataset"},
#endif
    {HUB_M_GET, "/api/radio/provision"},
#if CONFIG_MINIHUB_ZIGBEE_SDK
    {HUB_M_GET, "/api/zigbee"},
#endif
    {HUB_M_GET, "/api/modules/*/preview"},
    {HUB_M_POST, "/api/command"},
    {HUB_M_POST, "/api/rules/import"},
    {HUB_M_POST, "/api/scenes/import"},
    {HUB_M_POST, "/api/scenes/invoke"},
    {HUB_M_POST, "/api/store/config"},
    {HUB_M_GET, "/api/ota"},
    {HUB_M_POST, "/api/ota"},
    {HUB_M_POST, "/api/hub/reload"},
    {HUB_M_POST, "/api/points/remove"},
#if CONFIG_MINIHUB_ZIGBEE_SDK
    {HUB_M_POST, "/api/zigbee/permit_join"},
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    {HUB_M_POST, "/api/ble_mesh/permit_provision"},
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    {HUB_M_POST, "/api/thread/start"},
    {HUB_M_POST, "/api/thread/stop"},
    {HUB_M_POST, "/api/thread/dataset"},
#endif
    {HUB_M_POST, "/api/store/install"},
    {HUB_M_POST, "/api/store/upgrade"},
    {HUB_M_POST, "/api/store/uninstall"},
    {HUB_M_POST, "/api/store/refresh"},
    {HUB_M_POST, "/api/modules/*/rpc"},
};

static const char *s_rpc_allow[] = {
    "status", "homes", "devices", "device_log", "prop_get", "prop_probe", "camera_preview", "sync",
    "logout", "login", "session", "verify", "verify_send", "login_retry",
};

static int method_mask(const char *method)
{
    if (!method || !method[0]) {
        return 0;
    }
    if (strcasecmp(method, "GET") == 0) {
        return HUB_M_GET;
    }
    if (strcasecmp(method, "POST") == 0) {
        return HUB_M_POST;
    }
    if (strcasecmp(method, "PUT") == 0) {
        return HUB_M_PUT;
    }
    return 0;
}

static bool path_matches_rule(const char *path, const char *rule)
{
    const char *star = strchr(rule, '*');
    if (!star) {
        return strcmp(path, rule) == 0;
    }
    size_t pre = (size_t)(star - rule);
    const char *suf = star + 1;
    if (strncmp(path, rule, pre) != 0) {
        return false;
    }
    const char *rest = path + pre;
    const char *slash = strchr(rest, '/');
    if (!slash) {
        return suf[0] == '\0';
    }
    return strcmp(slash, suf) == 0;
}

static bool rule_allows(int mask, const char *path)
{
    for (size_t i = 0; i < sizeof(s_rules) / sizeof(s_rules[0]); i++) {
        if ((s_rules[i].methods & (uint8_t)mask) == 0) {
            continue;
        }
        if (path_matches_rule(path, s_rules[i].path)) {
            return true;
        }
    }
    return false;
}

static bool rpc_method_allowed(const char *rpc_method)
{
    if (!rpc_method || !rpc_method[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_rpc_allow) / sizeof(s_rpc_allow[0]); i++) {
        if (strcmp(rpc_method, s_rpc_allow[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void query_get(const char *query, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!query || !key || out_len == 0) {
        return;
    }
    size_t klen = strlen(key);
    const char *p = query;
    while (*p) {
        if ((p == query || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < out_len) {
                char c = *p++;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%' && isxdigit((unsigned char)p[0]) &&
                           isxdigit((unsigned char)p[1])) {
                    char hex[3] = {p[0], p[1], 0};
                    c = (char)strtol(hex, NULL, 16);
                    p += 2;
                }
                out[i++] = c;
            }
            out[i] = '\0';
            return;
        }
        p++;
    }
}

static bool extract_module_rpc(const char *path, char *module_id, size_t id_len)
{
    const char *prefix = "/api/modules/";
    const char *suffix = "/rpc";
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *start = path + strlen(prefix);
    const char *end = strstr(start, suffix);
    if (!end || strcmp(end, suffix) != 0) {
        return false;
    }
    size_t n = (size_t)(end - start);
    if (n == 0 || n + 1 > id_len) {
        return false;
    }
    memcpy(module_id, start, n);
    module_id[n] = '\0';
    return true;
}

static cJSON *err_obj(const char *code, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", code);
    if (detail && detail[0]) {
        cJSON_AddStringToObject(root, "detail", detail);
    }
    return root;
}

static void free_point_list(minihub_point_t *list, size_t count)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].props) {
            cJSON_Delete(list[i].props);
            list[i].props = NULL;
        }
    }
    free(list);
}

static esp_err_t dispatch_get_points(const char *query, cJSON **out)
{
    char type[32] = {0};
    char adapter[32] = {0};
    query_get(query, "type", type, sizeof(type));
    query_get(query, "adapter", adapter, sizeof(adapter));

    minihub_point_t *list = NULL;
    size_t count = 0;
    esp_err_t err;
    if (type[0] && !adapter[0]) {
        err = minihub_point_list_by_type(type, &list, &count);
    } else if (adapter[0] && !type[0]) {
        err = minihub_point_list_by_adapter(adapter, &list, &count);
    } else {
        err = minihub_point_list(&list, &count);
    }
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        free_point_list(list, count);
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "points", arr);

    for (size_t i = 0; i < count; i++) {
        if (type[0] && strcmp(list[i].type, type) != 0) {
            continue;
        }
        if (adapter[0] && strcmp(list[i].adapter, adapter) != 0) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "point_id", list[i].point_id);
        cJSON_AddStringToObject(obj, "type", list[i].type);
        cJSON_AddStringToObject(obj, "value", list[i].value);
        cJSON_AddStringToObject(obj, "adapter", list[i].adapter);
        cJSON_AddBoolToObject(obj, "online", list[i].online);
        cJSON_AddNumberToObject(obj, "updated_ms", (double)list[i].updated_ms);
        if (list[i].props) {
            cJSON_AddItemToObject(obj, "props", cJSON_Duplicate(list[i].props, true));
        }
        cJSON_AddItemToArray(arr, obj);
    }
    free_point_list(list, count);
    *out = root;
    return ESP_OK;
}

static esp_err_t dispatch_get_history(const char *query, cJSON **out)
{
    char point_id[MINIHUB_POINT_ID_MAX] = {0};
    char limit_buf[16] = {0};
    query_get(query, "point_id", point_id, sizeof(point_id));
    query_get(query, "limit", limit_buf, sizeof(limit_buf));
    if (!point_id[0]) {
        *out = err_obj("INVALID_ARG", "point_id required");
        return ESP_OK;
    }
    size_t limit = 20;
    if (limit_buf[0]) {
        int v = atoi(limit_buf);
        if (v > 0) {
            limit = (size_t)v;
        }
    }
    if (limit > 50) {
        limit = 50;
    }

    cJSON *local = minihub_point_history_json(point_id, limit);
    if (!local) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(local, "ok", true);
    cJSON_AddStringToObject(local, "point_id", point_id);

    cJSON *params = cJSON_CreateObject();
    cJSON *cloud = NULL;
    const minihub_point_t *pt = minihub_point_get(point_id);
    const char *adapter = (pt && pt->adapter[0]) ? pt->adapter : NULL;
    if (params && adapter) {
        cJSON_AddStringToObject(params, "point_id", point_id);
        cJSON_AddNumberToObject(params, "limit", (double)limit);
        if (minihub_module_invoke_rpc(adapter, "device_log", params, &cloud) == ESP_OK && cloud) {
            cJSON *citems = cJSON_DetachItemFromObject(cloud, "items");
            if (citems) {
                cJSON_AddItemToObject(local, "cloud_items", citems);
            }
            cJSON *cerr = cJSON_GetObjectItem(cloud, "cloud_error");
            if (cJSON_IsString(cerr)) {
                cJSON_AddStringToObject(local, "cloud_error", cerr->valuestring);
            }
            cJSON_Delete(cloud);
        }
        cJSON_Delete(params);
    } else {
        cJSON_Delete(params);
    }
    *out = local;
    return ESP_OK;
}

static esp_err_t dispatch_post_command(const char *body, cJSON **out)
{
    cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "JSON body with point_id and cmd required");
        return ESP_OK;
    }
    const cJSON *pid = cJSON_GetObjectItem(req, "point_id");
    const cJSON *cmd = cJSON_GetObjectItem(req, "cmd");
    const cJSON *params = cJSON_GetObjectItem(req, "params");
    if (!cJSON_IsString(pid) || !pid->valuestring[0] || !cJSON_IsString(cmd) || !cmd->valuestring[0]) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "point_id and cmd required");
        return ESP_OK;
    }

    minihub_command_t command = {0};
    strncpy(command.point_id, pid->valuestring, sizeof(command.point_id) - 1);
    strncpy(command.cmd, cmd->valuestring, sizeof(command.cmd) - 1);
    if (cJSON_IsObject(params)) {
        command.params = cJSON_Duplicate(params, true);
    }
    esp_err_t err = minihub_command_invoke(&command);
    if (command.params) {
        cJSON_Delete(command.params);
    }
    cJSON_Delete(req);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    *out = root;
    return ESP_OK;
}

static esp_err_t dispatch_module_rpc(const char *module_id, const char *body, cJSON **out)
{
    cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "JSON body with method required");
        return ESP_OK;
    }
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    const cJSON *params = cJSON_GetObjectItem(req, "params");
    if (!cJSON_IsString(method) || !method->valuestring[0]) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "method required");
        return ESP_OK;
    }
    if (!rpc_method_allowed(method->valuestring)) {
        cJSON *denied = err_obj("DENIED", method->valuestring);
        cJSON_Delete(req);
        *out = denied;
        return ESP_OK;
    }
    cJSON *result = NULL;
    esp_err_t err = minihub_module_invoke_rpc(module_id, method->valuestring,
                                              cJSON_IsObject(params) ? params : NULL, &result);
    cJSON_Delete(req);
    if (err != ESP_OK) {
        cJSON_Delete(result);
        *out = err_obj(err == ESP_ERR_INVALID_STATE ? "MODULE_NOT_READY" : esp_err_to_name(err),
                       module_id);
        return ESP_OK;
    }
    if (!result) {
        result = cJSON_CreateObject();
    }
    if (!cJSON_GetObjectItem(result, "ok")) {
        cJSON_AddBoolToObject(result, "ok", true);
    }
    *out = result;
    return ESP_OK;
}

static esp_err_t dispatch_rules_import(const char *body, cJSON **out)
{
    if (!body || !body[0]) {
        *out = err_obj("INVALID_ARG", "body required");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        *out = err_obj("INVALID_JSON", NULL);
        return ESP_OK;
    }
    const cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (!cJSON_IsArray(rules)) {
        cJSON_Delete(root);
        *out = err_obj("VALIDATION_FAILED", "rules array required");
        return ESP_OK;
    }
    int count = cJSON_GetArraySize(rules);
    esp_err_t err = minihub_rules_reload(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        *out = err_obj("RELOAD_FAILED", esp_err_to_name(err));
        return ESP_OK;
    }
    err = minihub_config_save_rules(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        (void)minihub_rules_reload(minihub_config_get_rules());
        *out = err_obj("SAVE_FAILED", esp_err_to_name(err));
        return ESP_OK;
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "count", count);
    *out = resp;
    return ESP_OK;
}

static esp_err_t dispatch_ota_get(cJSON **out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "state", (double)minihub_ota_get_state());
    cJSON_AddStringToObject(root, "error", minihub_ota_get_error());
    cJSON_AddNumberToObject(root, "progress", (double)minihub_ota_get_progress());
    cJSON_AddNumberToObject(root, "bytes_written", (double)minihub_ota_get_bytes_written());
    cJSON_AddStringToObject(root, "source", minihub_ota_source_str(minihub_ota_get_source()));
    cJSON_AddBoolToObject(root, "busy", minihub_ota_is_busy());
    *out = root;
    return ESP_OK;
}

static esp_err_t dispatch_ota_start(const char *body, cJSON **out)
{
    cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!req) {
        *out = err_obj("INVALID_ARG", "JSON body with url required (binary upload not via MCP)");
        return ESP_OK;
    }
    const cJSON *url = cJSON_GetObjectItem(req, "url");
    if (!cJSON_IsString(url) || !url->valuestring[0]) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "url required");
        return ESP_OK;
    }
    esp_err_t err = minihub_ota_start_url(url->valuestring);
    cJSON_Delete(req);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "result", "started");
    } else {
        cJSON_AddStringToObject(root, "error",
                                err == ESP_ERR_INVALID_STATE ? "busy" : esp_err_to_name(err));
    }
    *out = root;
    return ESP_OK;
}

static esp_err_t dispatch_hub_reload(cJSON **out)
{
    esp_err_t err = minihub_hub_reload_async();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddStringToObject(root, "result", err == ESP_OK ? "ok" : "fail");
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    *out = root;
    return ESP_OK;
}

#if CONFIG_MINIHUB_ZIGBEE_SDK
static esp_err_t dispatch_zigbee_permit(const char *body, cJSON **out)
{
    int seconds = 60;
    if (body && body[0]) {
        cJSON *req = cJSON_Parse(body);
        if (req) {
            const cJSON *sec = cJSON_GetObjectItem(req, "seconds");
            if (cJSON_IsNumber(sec)) {
                seconds = sec->valueint;
            }
            cJSON_Delete(req);
        }
    }
    esp_err_t zb_err = minihub_zigbee_permit_join(seconds);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", zb_err == ESP_OK);
    cJSON_AddStringToObject(root, "result", zb_err == ESP_OK ? "ok" : "fail");
    if (zb_err != ESP_OK) {
        minihub_zigbee_json_add_permit_join_error(root, zb_err);
    }
    cJSON_AddBoolToObject(root, "ready", minihub_zigbee_running());
    cJSON_AddBoolToObject(root, "permit_join", minihub_zigbee_permit_join_active());
    cJSON_AddNumberToObject(root, "seconds", seconds);
    *out = root;
    return ESP_OK;
}
#endif /* CONFIG_MINIHUB_ZIGBEE_SDK */

#if CONFIG_MINIHUB_MATTER_ADAPTER

static minihub_matter_discovery_t parse_matter_discovery(const cJSON *disc)
{
    if (cJSON_IsString(disc) && disc->valuestring) {
        if (strcmp(disc->valuestring, "ble_wifi") == 0 ||
            strcmp(disc->valuestring, "ble-wifi") == 0 ||
            strcmp(disc->valuestring, "first_time") == 0) {
            return MINIHUB_MATTER_DISCOVERY_BLE_WIFI;
        }
    }
    return MINIHUB_MATTER_DISCOVERY_NETWORK;
}

static esp_err_t parse_matter_endpoints_json(const cJSON *root, uint16_t *out, size_t max_out, size_t *out_count)
{
    if (!out || !out_count || max_out == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    const cJSON *eps = cJSON_GetObjectItem(root, "endpoints");
    if (cJSON_IsArray(eps)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, eps) {
            if (*out_count >= max_out) {
                break;
            }
            if (cJSON_IsNumber(item)) {
                out[(*out_count)++] = (uint16_t)item->valueint;
            }
        }
        if (*out_count > 0) {
            return ESP_OK;
        }
    }

    if (cJSON_IsString(eps) && eps->valuestring[0]) {
        char buf[64];
        strncpy(buf, eps->valuestring, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            if (*out_count >= max_out) {
                break;
            }
            while (*tok == ' ') {
                tok++;
            }
            if (!tok[0]) {
                continue;
            }
            unsigned long v = strtoul(tok, NULL, 10);
            if (v > 0 && v < 65536) {
                out[(*out_count)++] = (uint16_t)v;
            }
        }
        if (*out_count > 0) {
            return ESP_OK;
        }
    }

    out[0] = 1;
    *out_count = 1;
    return ESP_OK;
}

static esp_err_t dispatch_get_matter_status(cJSON **out)
{
    cJSON *status = minihub_matter_status_json();
    if (!status) {
        *out = err_obj("INTERNAL", "matter status");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(status, "ok", true);
    *out = status;
    return ESP_OK;
}

static esp_err_t dispatch_matter_commission(const char *body, cJSON **out)
{
    cJSON *root = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "JSON body required");
        return ESP_OK;
    }

    const cJSON *node = cJSON_GetObjectItem(root, "node_id");
    const cJSON *code = cJSON_GetObjectItem(root, "pairing_code");
    const cJSON *pin = cJSON_GetObjectItem(root, "setup_pin");
    const cJSON *disc = cJSON_GetObjectItem(root, "discovery");
    const cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    const cJSON *wpass = cJSON_GetObjectItem(root, "wifi_password");

    uint64_t node_id = 2;
    if (cJSON_IsString(node)) {
        node_id = strtoull(node->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(node)) {
        node_id = (uint64_t)node->valuedouble;
    }

    minihub_matter_discovery_t discovery = parse_matter_discovery(disc);
    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(code)) {
        err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery, ssid_s, pass_s);
    } else if (cJSON_IsNumber(pin)) {
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
            cJSON_Delete(root);
            *out = err_obj("INVALID_ARG", "ble_wifi requires pairing_code");
            return ESP_OK;
        }
        err = minihub_matter_commission_pin(node_id, (uint32_t)pin->valueint);
    } else {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "need pairing_code or setup_pin");
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_Delete(root);
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "started" : esp_err_to_name(err));
    if (err != ESP_OK && minihub_matter_last_error()) {
        cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t dispatch_matter_devices(const char *body, cJSON **out)
{
    cJSON *root = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "JSON body required");
        return ESP_OK;
    }

    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    if (cJSON_IsTrue(del)) {
        if (!cJSON_GetObjectItem(root, "adapter")) {
            cJSON_AddStringToObject(root, "adapter", "matter");
        }
        esp_err_t err = minihub_hub_points_remove(root, out);
        cJSON_Delete(root);
        if (err == ESP_ERR_NO_MEM) {
            return err;
        }
        if (!*out) {
            *out = err_obj("INTERNAL", "remove failed");
        }
        return ESP_OK;
    }

    const cJSON *code = cJSON_GetObjectItem(root, "pairing_code");
    const cJSON *node = cJSON_GetObjectItem(root, "node_id");
    const cJSON *merge = cJSON_GetObjectItem(root, "merge");
    const cJSON *disc = cJSON_GetObjectItem(root, "discovery");
    const cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    const cJSON *wpass = cJSON_GetObjectItem(root, "wifi_password");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool merge_existing = !cJSON_IsBool(merge) || cJSON_IsTrue(merge);
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    uint64_t node_id = 0;
    if (cJSON_IsString(node)) {
        node_id = strtoull(node->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(node)) {
        node_id = (uint64_t)node->valuedouble;
    }
    if (node_id < 2) {
        esp_err_t nid_err = minihub_matter_next_node_id(&node_id);
        if (nid_err != ESP_OK) {
            cJSON_Delete(root);
            *out = err_obj("INTERNAL", esp_err_to_name(nid_err));
            return ESP_OK;
        }
    }

    uint16_t endpoints[16];
    size_t ep_count = 0;
    esp_err_t err = parse_matter_endpoints_json(root, endpoints, sizeof(endpoints) / sizeof(endpoints[0]),
                                                &ep_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "invalid endpoints");
        return ESP_OK;
    }

    minihub_matter_discovery_t discovery = parse_matter_discovery(disc);
    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    const bool want_commission = cJSON_IsString(code) && code->valuestring[0];
    esp_err_t commission_err = ESP_OK;
    if (want_commission) {
        commission_err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery,
                                                           ssid_s, pass_s);
        if (commission_err != ESP_OK) {
            cJSON_Delete(root);
            cJSON *resp = cJSON_CreateObject();
            if (!resp) {
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "result", "error");
            cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
            cJSON_AddStringToObject(resp, "commission", esp_err_to_name(commission_err));
            cJSON_AddStringToObject(resp, "message",
                                    minihub_matter_last_error() ? minihub_matter_last_error()
                                                                : esp_err_to_name(commission_err));
            *out = resp;
            return ESP_OK;
        }
    }

    size_t synced = 0;
    err = minihub_matter_devices_upsert(node_id, endpoints, ep_count, merge_existing, &synced);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        *out = err_obj("INTERNAL", esp_err_to_name(err));
        return ESP_OK;
    }

    if (do_reload) {
        (void)minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
    cJSON_AddNumberToObject(resp, "synced", (double)synced);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    cJSON_AddStringToObject(resp, "commission", want_commission ? "started" : "skipped");
    *out = resp;
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */

static esp_err_t dispatch_points_remove(const char *body, cJSON **out)
{
    cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!req || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "JSON body required");
        return ESP_OK;
    }

    esp_err_t err = minihub_hub_points_remove(req, out);
    cJSON_Delete(req);
    if (err == ESP_ERR_NO_MEM) {
        return err;
    }
    if (!*out) {
        *out = err_obj("INTERNAL", "remove failed");
    }
    return ESP_OK;
}

static esp_err_t dispatch_get_radio_provision(cJSON **out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
#if CONFIG_MINIHUB_ZIGBEE_SDK
    cJSON_AddBoolToObject(root, "zigbee_available", true);
    const bool zb = minihub_zigbee_permit_join_active();
#else
    cJSON_AddBoolToObject(root, "zigbee_available", false);
    const bool zb = false;
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    cJSON_AddBoolToObject(root, "ble_mesh_available", true);
    cJSON_AddBoolToObject(root, "ble_mesh_running", minihub_ble_mesh_running());
    const bool bm_permit = minihub_ble_mesh_permit_provision_active();
    const bool bm_busy = minihub_ble_mesh_provisioning_busy();
#else
    cJSON_AddBoolToObject(root, "ble_mesh_available", false);
    cJSON_AddBoolToObject(root, "ble_mesh_running", false);
    const bool bm_permit = false;
    const bool bm_busy = false;
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
    cJSON_AddBoolToObject(root, "matter_available", true);
    cJSON_AddBoolToObject(root, "matter_ready", minihub_matter_ready());
    const bool matter_pairing = false; /* no permit-join window; commission is on-demand */
#else
    cJSON_AddBoolToObject(root, "matter_available", false);
    cJSON_AddBoolToObject(root, "matter_ready", false);
    const bool matter_pairing = false;
#endif
    cJSON_AddBoolToObject(root, "zigbee_permit_join", zb);
    cJSON_AddBoolToObject(root, "ble_mesh_permit_provision", bm_permit);
    cJSON_AddBoolToObject(root, "ble_mesh_provisioning", bm_busy);
    cJSON_AddBoolToObject(root, "matter_pairing", matter_pairing);
    cJSON_AddBoolToObject(root, "busy", zb || bm_busy || matter_pairing);
    *out = root;
    return ESP_OK;
}

#if CONFIG_MINIHUB_ZIGBEE_SDK
static esp_err_t dispatch_get_zigbee(cJSON **out)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "available", true);
    cJSON_AddBoolToObject(root, "ready", minihub_zigbee_running());
    cJSON_AddBoolToObject(root, "permit_join", minihub_zigbee_permit_join_active());
    minihub_point_t *list = NULL;
    size_t count = 0;
    if (minihub_point_list_by_adapter("zigbee", &list, &count) == ESP_OK) {
        cJSON_AddNumberToObject(root, "device_count", (double)count);
        free_point_list(list, count);
    } else {
        cJSON_AddNumberToObject(root, "device_count", 0);
    }
    *out = root;
    return ESP_OK;
}
#endif /* CONFIG_MINIHUB_ZIGBEE_SDK */

#if CONFIG_MINIHUB_MATTER_ADAPTER

static minihub_matter_discovery_t parse_matter_discovery(const cJSON *disc)
{
    if (cJSON_IsString(disc) && disc->valuestring) {
        if (strcmp(disc->valuestring, "ble_wifi") == 0 ||
            strcmp(disc->valuestring, "ble-wifi") == 0 ||
            strcmp(disc->valuestring, "first_time") == 0) {
            return MINIHUB_MATTER_DISCOVERY_BLE_WIFI;
        }
        if (strcmp(disc->valuestring, "ble_thread") == 0 ||
            strcmp(disc->valuestring, "ble-thread") == 0) {
            return MINIHUB_MATTER_DISCOVERY_BLE_THREAD;
        }
        if (strcmp(disc->valuestring, "ble_ethernet") == 0 ||
            strcmp(disc->valuestring, "ble-ethernet") == 0 ||
            strcmp(disc->valuestring, "ethernet") == 0) {
            return MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET;
        }
    }
    return MINIHUB_MATTER_DISCOVERY_NETWORK;
}

static esp_err_t parse_matter_endpoints_json(const cJSON *root, uint16_t *out, size_t max_out, size_t *out_count)
{
    if (!out || !out_count || max_out == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    const cJSON *eps = cJSON_GetObjectItem(root, "endpoints");
    if (cJSON_IsArray(eps)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, eps) {
            if (*out_count >= max_out) {
                break;
            }
            if (cJSON_IsNumber(item)) {
                out[(*out_count)++] = (uint16_t)item->valueint;
            }
        }
        if (*out_count > 0) {
            return ESP_OK;
        }
    }

    if (cJSON_IsString(eps) && eps->valuestring[0]) {
        char buf[64];
        strncpy(buf, eps->valuestring, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *save = NULL;
        for (char *tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            if (*out_count >= max_out) {
                break;
            }
            while (*tok == ' ') {
                tok++;
            }
            if (!tok[0]) {
                continue;
            }
            unsigned long v = strtoul(tok, NULL, 10);
            if (v > 0 && v < 65536) {
                out[(*out_count)++] = (uint16_t)v;
            }
        }
        if (*out_count > 0) {
            return ESP_OK;
        }
    }

    out[0] = 1;
    *out_count = 1;
    return ESP_OK;
}

static esp_err_t dispatch_get_matter_status(cJSON **out)
{
    cJSON *status = minihub_matter_status_json();
    if (!status) {
        *out = err_obj("INTERNAL", "matter status");
        return ESP_OK;
    }
    if (!cJSON_GetObjectItem(status, "ok")) {
        cJSON_AddBoolToObject(status, "ok", true);
    }
    *out = status;
    return ESP_OK;
}

static esp_err_t dispatch_matter_commission(const char *body, cJSON **out)
{
    cJSON *root = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "JSON body required");
        return ESP_OK;
    }

    const cJSON *node = cJSON_GetObjectItem(root, "node_id");
    const cJSON *code = cJSON_GetObjectItem(root, "pairing_code");
    const cJSON *pin = cJSON_GetObjectItem(root, "setup_pin");
    const cJSON *disc = cJSON_GetObjectItem(root, "discovery");
    const cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    const cJSON *wpass = cJSON_GetObjectItem(root, "wifi_password");

    uint64_t node_id = 2;
    if (cJSON_IsString(node)) {
        node_id = strtoull(node->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(node)) {
        node_id = (uint64_t)node->valuedouble;
    }

    minihub_matter_discovery_t discovery = parse_matter_discovery(disc);
    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(code)) {
        err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery, ssid_s, pass_s);
    } else if (cJSON_IsNumber(pin)) {
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI ||
            discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD ||
            discovery == MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET) {
            cJSON_Delete(root);
            *out = err_obj("INVALID_ARG", "BLE discovery requires pairing_code");
            return ESP_OK;
        }
        err = minihub_matter_commission_pin(node_id, (uint32_t)pin->valueint);
    } else {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "need pairing_code or setup_pin");
        return ESP_OK;
    }

    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "started" : esp_err_to_name(err));
    if (err != ESP_OK && minihub_matter_last_error()) {
        cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
    }
    *out = resp;
    return ESP_OK;
}

static esp_err_t dispatch_matter_devices(const char *body, cJSON **out)
{
    cJSON *root = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "JSON body required");
        return ESP_OK;
    }

    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (cJSON_IsTrue(del)) {
        if (!cJSON_GetObjectItem(root, "adapter")) {
            cJSON_AddStringToObject(root, "adapter", "matter");
        }
        esp_err_t err = minihub_hub_points_remove(root, out);
        cJSON_Delete(root);
        if (err == ESP_ERR_NO_MEM) {
            return err;
        }
        if (!*out) {
            *out = err_obj("INTERNAL", "remove failed");
        }
        return ESP_OK;
    }

    const cJSON *code = cJSON_GetObjectItem(root, "pairing_code");
    const cJSON *node = cJSON_GetObjectItem(root, "node_id");
    const cJSON *merge = cJSON_GetObjectItem(root, "merge");
    const cJSON *disc = cJSON_GetObjectItem(root, "discovery");
    const cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    const cJSON *wpass = cJSON_GetObjectItem(root, "wifi_password");
    bool merge_existing = !cJSON_IsBool(merge) || cJSON_IsTrue(merge);

    uint64_t node_id = 0;
    if (cJSON_IsString(node)) {
        node_id = strtoull(node->valuestring, NULL, 0);
    } else if (cJSON_IsNumber(node)) {
        node_id = (uint64_t)node->valuedouble;
    }
    if (node_id < 2) {
        esp_err_t nid_err = minihub_matter_next_node_id(&node_id);
        if (nid_err != ESP_OK) {
            cJSON_Delete(root);
            *out = err_obj("INTERNAL", esp_err_to_name(nid_err));
            return ESP_OK;
        }
    }

    uint16_t endpoints[16];
    size_t ep_count = 0;
    esp_err_t err = parse_matter_endpoints_json(root, endpoints, sizeof(endpoints) / sizeof(endpoints[0]),
                                                &ep_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_ARG", "invalid endpoints");
        return ESP_OK;
    }

    minihub_matter_discovery_t discovery = parse_matter_discovery(disc);
    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;
    const cJSON *tr = cJSON_GetObjectItem(root, "transport");
    const char *transport = cJSON_IsString(tr) ? tr->valuestring : NULL;
    if (!transport) {
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
            transport = "wifi";
        } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD) {
            transport = "thread";
        } else if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET) {
            transport = "ethernet";
        }
    }

    esp_err_t commission_err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(code) && code->valuestring[0]) {
        commission_err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery,
                                                           ssid_s, pass_s);
    }
    if (commission_err != ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_Delete(root);
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "result", "error");
        cJSON_AddStringToObject(resp, "commission", esp_err_to_name(commission_err));
        if (minihub_matter_last_error()) {
            cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
        } else {
            cJSON_AddStringToObject(resp, "message", "commission did not start");
        }
        *out = resp;
        return ESP_OK;
    }

    size_t synced = 0;
    err = minihub_matter_devices_upsert(node_id, endpoints, ep_count, merge_existing, &synced, transport);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        *out = err_obj("INTERNAL", esp_err_to_name(err));
        return ESP_OK;
    }

    if (do_reload) {
        (void)minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
    cJSON_AddNumberToObject(resp, "synced", (double)synced);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    cJSON_AddStringToObject(resp, "commission", "started");
    *out = resp;
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */

#if CONFIG_MINIHUB_BLE_MESH_SDK
static esp_err_t dispatch_ble_mesh_permit(const char *body, cJSON **out)
{
    int seconds = 60;
    if (body && body[0]) {
        cJSON *req = cJSON_Parse(body);
        if (req) {
            const cJSON *sec = cJSON_GetObjectItem(req, "seconds");
            if (cJSON_IsNumber(sec)) {
                seconds = sec->valueint;
            }
            cJSON_Delete(req);
        }
    }
    esp_err_t bm_err = minihub_ble_mesh_permit_provision(seconds);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", bm_err == ESP_OK);
    cJSON_AddStringToObject(root, "result", bm_err == ESP_OK ? "ok" : "fail");
    if (bm_err != ESP_OK) {
        if (bm_err == ESP_ERR_INVALID_STATE && minihub_ble_mesh_provisioning_busy()) {
            cJSON_AddStringToObject(root, "error", "BLE Mesh 正在配网，请勿重复开启");
        } else if (bm_err == ESP_ERR_INVALID_STATE && !minihub_ble_mesh_running()) {
            cJSON_AddStringToObject(root, "error", "BLE Mesh 栈未就绪（启动失败或仍在初始化）");
#if CONFIG_MINIHUB_ZIGBEE_SDK
        } else if (bm_err == ESP_ERR_INVALID_STATE && minihub_zigbee_permit_join_active()) {
            cJSON_AddStringToObject(root, "error", "Zigbee 正在入网，无法同时开启 BLE Mesh 配网");
#endif
        } else {
            cJSON_AddStringToObject(root, "error", esp_err_to_name(bm_err));
        }
    }
    cJSON_AddBoolToObject(root, "ready", minihub_ble_mesh_running());
    cJSON_AddBoolToObject(root, "permit_provision", minihub_ble_mesh_permit_provision_active());
    cJSON_AddNumberToObject(root, "seconds", seconds);
    *out = root;
    return ESP_OK;
}
#endif /* CONFIG_MINIHUB_BLE_MESH_SDK */

typedef enum {
    STORE_OP_INSTALL = 0,
    STORE_OP_UPGRADE,
    STORE_OP_UNINSTALL,
} store_op_t;

static esp_err_t dispatch_store_op(store_op_t op, const char *body, cJSON **out)
{
    cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
    if (!req) {
        *out = err_obj("INVALID_ARG", "JSON body with kind and id required");
        return ESP_OK;
    }
    const cJSON *kind = cJSON_GetObjectItem(req, "kind");
    const cJSON *id = cJSON_GetObjectItem(req, "id");
    const cJSON *reload = cJSON_GetObjectItem(req, "reload");
    const cJSON *upgrade = cJSON_GetObjectItem(req, "upgrade");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);
    bool do_upgrade = cJSON_IsBool(upgrade) && cJSON_IsTrue(upgrade);

    if (!cJSON_IsString(kind) || !cJSON_IsString(id)) {
        cJSON_Delete(req);
        *out = err_obj("INVALID_ARG", "kind and id required");
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if (op == STORE_OP_INSTALL) {
        err = do_upgrade ? minihub_store_upgrade(kind->valuestring, id->valuestring)
                         : minihub_store_install(kind->valuestring, id->valuestring);
    } else if (op == STORE_OP_UPGRADE) {
        err = minihub_store_upgrade(kind->valuestring, id->valuestring);
    } else {
        err = minihub_store_uninstall(kind->valuestring, id->valuestring);
    }
    cJSON_Delete(req);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err == ESP_OK) {
        cJSON_AddStringToObject(root, "result", "ok");
        if (do_reload) {
            esp_err_t rerr = minihub_hub_reload_async();
            cJSON_AddBoolToObject(root, "reload_ok", rerr == ESP_OK);
        }
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    *out = root;
    return ESP_OK;
}

static esp_err_t dispatch_adapters_put(const char *body, cJSON **out)
{
    if (!body || !body[0]) {
        *out = err_obj("INVALID_BODY", "empty");
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        *out = err_obj("INVALID_JSON", "adapters");
        return ESP_OK;
    }
    bool do_reload = true;
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    if (cJSON_IsBool(reload)) {
        do_reload = cJSON_IsTrue(reload);
        cJSON_DeleteItemFromObject(root, "reload");
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(root, "adapters"))) {
        cJSON_Delete(root);
        *out = err_obj("INVALID_BODY", "missing adapters object");
        return ESP_OK;
    }
    esp_err_t err = minihub_config_save_adapters(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        *out = err_obj(esp_err_to_name(err), "save_adapters");
        return ESP_OK;
    }
    if (do_reload) {
        err = minihub_hub_reload_async();
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(resp, "ok", err == ESP_OK);
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    *out = resp;
    return ESP_OK;
}

static esp_err_t dispatch_store_refresh(cJSON **out)
{
    esp_err_t err = minihub_store_refresh_catalog();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }
    *out = root;
    return ESP_OK;
}

esp_err_t hub_api_call(const char *method, const char *path, const char *query, const char *body,
                       cJSON **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (!method || !path || path[0] != '/') {
        *out = err_obj("INVALID_ARG", "method and absolute path required");
        return ESP_OK;
    }

    char path_buf[160];
    strlcpy(path_buf, path, sizeof(path_buf));
    char *qmark = strchr(path_buf, '?');
    const char *effective_query = query;
    if (qmark) {
        *qmark = '\0';
        if (!effective_query || !effective_query[0]) {
            effective_query = qmark + 1;
        }
    }

    int mask = method_mask(method);
    if (mask == 0) {
        *out = err_obj("INVALID_ARG", "unsupported method");
        return ESP_OK;
    }
    if (!rule_allows(mask, path_buf)) {
        ESP_LOGW(TAG, "denied %s %s", method, path_buf);
        *out = err_obj("DENIED", path_buf);
        return ESP_OK;
    }

    char module_id[32];
    if (mask == HUB_M_POST && extract_module_rpc(path_buf, module_id, sizeof(module_id))) {
        return dispatch_module_rpc(module_id, body, out);
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/points") == 0) {
        return dispatch_get_points(effective_query, out);
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/points/history") == 0) {
        return dispatch_get_history(effective_query, out);
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/radio/provision") == 0) {
        return dispatch_get_radio_provision(out);
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/zigbee") == 0) {
        return dispatch_get_zigbee(out);
    }
#endif
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/command") == 0) {
        return dispatch_post_command(body, out);
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/rules") == 0) {
        cJSON *rules = minihub_config_get_rules();
        if (!rules) {
            *out = err_obj("CONFIG_NOT_READY", "rules");
            return ESP_OK;
        }
        cJSON *dup = cJSON_Duplicate(rules, true);
        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(dup, "ok", true);
        *out = dup;
        return ESP_OK;
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/rules/import") == 0) {
        return dispatch_rules_import(body, out);
    }
    if (mask == HUB_M_GET &&
        (strcmp(path_buf, "/api/adapters") == 0 || strcmp(path_buf, "/api/config/adapters") == 0)) {
        cJSON *adapters = minihub_config_get_adapters();
        if (!adapters) {
            *out = err_obj("CONFIG_NOT_READY", "adapters");
            return ESP_OK;
        }
        cJSON *dup = cJSON_Duplicate(adapters, true);
        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(dup, "ok", true);
        *out = dup;
        return ESP_OK;
    }
    if (mask == HUB_M_PUT && strcmp(path_buf, "/api/config/adapters") == 0) {
        return dispatch_adapters_put(body, out);
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/store/catalog") == 0) {
        cJSON *catalog = minihub_store_catalog_merged();
        if (!catalog) {
            *out = err_obj("STORE_ERROR", "catalog failed");
            return ESP_OK;
        }
        *out = catalog;
        return ESP_OK;
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/modules") == 0) {
        cJSON *modules = minihub_module_list_json();
        if (!modules) {
            *out = err_obj("MODULE_ERROR", "modules list failed");
            return ESP_OK;
        }
        *out = modules;
        return ESP_OK;
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/system") == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
        char ip[16] = {0};
        (void)minihub_net_get_ip(ip, sizeof(ip));
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "local_ip", ip);
#if defined(CONFIG_APP_CLAW_CAP_CORE)
        /* Claw portal serves MiniHub UI at /minihub */
        cJSON_AddStringToObject(root, "web_base", "/minihub");
#else
        cJSON_AddStringToObject(root, "web_base", "");
#endif
        *out = root;
        return ESP_OK;
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/store/config") == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
        char url[128] = {0};
        bool disable_default = false;
        (void)minihub_store_get_config(url, sizeof(url), &disable_default, NULL, 0);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "custom_url", url);
        cJSON_AddBoolToObject(root, "disable_default", disable_default);
        *out = root;
        return ESP_OK;
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/store/config") == 0) {
        cJSON *req = body && body[0] ? cJSON_Parse(body) : NULL;
        if (!req) {
            *out = err_obj("INVALID_ARG", "JSON body required");
            return ESP_OK;
        }
        const cJSON *url = cJSON_GetObjectItem(req, "custom_url");
        const cJSON *dis = cJSON_GetObjectItem(req, "disable_default");
        const char *url_s = cJSON_IsString(url) ? url->valuestring : "";
        bool dis_b = cJSON_IsTrue(dis);
        esp_err_t err = minihub_store_set_config(url_s, dis_b);
        cJSON_Delete(req);
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
        if (err != ESP_OK) {
            cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        }
        *out = root;
        return ESP_OK;
    }
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/ota") == 0) {
        return dispatch_ota_get(out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/ota") == 0) {
        return dispatch_ota_start(body, out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/hub/reload") == 0) {
        return dispatch_hub_reload(out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/points/remove") == 0) {
        return dispatch_points_remove(body, out);
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/zigbee/permit_join") == 0) {
        return dispatch_zigbee_permit(body, out);
    }
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/ble_mesh/permit_provision") == 0) {
        return dispatch_ble_mesh_permit(body, out);
    }
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
    if (mask == HUB_M_GET && strcmp(path_buf, "/api/matter/status") == 0) {
        return dispatch_get_matter_status(out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/matter/commission") == 0) {
        return dispatch_matter_commission(body, out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/matter/devices") == 0) {
        return dispatch_matter_devices(body, out);
    }
#endif
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/store/install") == 0) {
        return dispatch_store_op(STORE_OP_INSTALL, body, out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/store/upgrade") == 0) {
        return dispatch_store_op(STORE_OP_UPGRADE, body, out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/store/uninstall") == 0) {
        return dispatch_store_op(STORE_OP_UNINSTALL, body, out);
    }
    if (mask == HUB_M_POST && strcmp(path_buf, "/api/store/refresh") == 0) {
        return dispatch_store_refresh(out);
    }

    ESP_LOGW(TAG, "allowlisted but unimplemented: %s %s", method, path_buf);
    *out = err_obj("NOT_IMPLEMENTED", path_buf);
    return ESP_OK;
}
