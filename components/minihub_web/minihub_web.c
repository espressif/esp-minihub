/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_web.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "minihub_net.h"
#include "minihub_net_cfg.h"
#include "minihub_core.h"
#include "minihub_http_worker.h"
#include "minihub_config.h"
#include "minihub_pack.h"
#include "minihub_module.h"
#include "minihub_store.h"
#include "minihub_hub.h"
#include "minihub_rules.h"
#include "minihub_auth.h"
#include "minihub_scenes.h"
#include "minihub_ota.h"
#if CONFIG_MINIHUB_MATTER_ADAPTER
#include "minihub_matter.h"
#endif
#include "minihub_map.h"
#include "minihub_map_tiles.h"
#include "minihub_paths.h"
#include "minihub_zigbee.h"
#include "minihub_ble_mesh.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_littlefs.h"
#include "esp_app_desc.h"

static const char *TAG = "minihub_web";

/** Product version shown in sidebar / system page (stable; not git describe). */
#define MINIHUB_PRODUCT_VERSION "0.2.0"

__attribute__((weak)) void minihub_map_download_begin(void)
{
}

__attribute__((weak)) void minihub_map_download_end(void)
{
}

__attribute__((weak)) void minihub_lcd_request_refresh(void)
{
}

#if CONFIG_MINIHUB_WEB_UI
extern const char MINIHUB_INDEX_HTML_FALLBACK[];
#endif

static httpd_handle_t s_server;
/** Optional extra index.html (attach config); NULL = preinstall path only. */
static const char *s_index_override;

static esp_err_t read_http_body(httpd_req_t *req, char **out_buf, size_t *out_len, size_t max_bytes);

#if CONFIG_MINIHUB_WEB_UI
static const char *content_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(dot, ".ico") == 0) {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

static bool web_path_safe(const char *rel)
{
    if (!rel || !rel[0] || rel[0] == '/' || strstr(rel, "..") != NULL) {
        return false;
    }
    for (const char *p = rel; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\') {
            return false;
        }
    }
    return true;
}

static esp_err_t send_file_chunked(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 4KB PSRAM chunk: keep the console HTML off the internal heap. */
    const size_t cap = 4096;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(cap);
    }
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, content_type_for_path(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t n;
    bool sent_any = false;
    while ((n = fread(buf, 1, cap, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            /* Peer likely reset (errno 104). Do not send a final empty chunk. */
            return sent_any ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
        sent_any = true;
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_preinstall_index_or_fallback(httpd_req_t *req)
{
    const char *index_paths[] = {
        MINIHUB_PREINSTALL_WEB_INDEX,
        s_index_override,
    };
    struct stat st;

    for (size_t i = 0; i < sizeof(index_paths) / sizeof(index_paths[0]); i++) {
        if (!index_paths[i] || !index_paths[i][0]) {
            continue;
        }
        if (stat(index_paths[i], &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        esp_err_t err = send_file_chunked(req, index_paths[i]);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "web index send failed (%s) path=%s size=%ld",
                 esp_err_to_name(err), index_paths[i], (long)st.st_size);
        return err;
    }
    ESP_LOGW(TAG, "missing %s%s%s — serving ROM fallback",
             MINIHUB_PREINSTALL_WEB_INDEX,
             s_index_override ? " and " : "",
             s_index_override ? s_index_override : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, MINIHUB_INDEX_HTML_FALLBACK, HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t api_auth(httpd_req_t *req)
{
    if (minihub_auth_check(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (!body) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static bool module_id_from_rpc_uri(const char *uri, char *id_out, size_t id_len)
{
    const char *prefix = "/api/modules/";
    if (!uri || strncmp(uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *p = uri + strlen(prefix);
    const char *suffix = "/rpc";
    const char *end = strstr(p, suffix);
    if (!end || end[4] != '\0') {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= id_len) {
        return false;
    }
    memcpy(id_out, p, len);
    id_out[len] = '\0';
    return true;
}

static esp_err_t handle_api_module_rpc(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char module_id[32];
    if (!module_id_from_rpc_uri(req->uri, module_id, sizeof(module_id))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad module rpc uri");
    }

    /* Wyze session import carries two long tokens; 768 was truncating → invalid json. */
    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 16 * 1024);
    if (body_err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method) || !method->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing method");
    }
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    if (params && !cJSON_IsObject(params)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "params must be object");
    }

    cJSON *result = NULL;
    err = minihub_module_invoke_rpc(module_id, method->valuestring, params, &result);
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "module RPC '%s' not ready (not loaded or no rpc)", module_id);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "module_not_ready");
        cJSON_AddStringToObject(resp, "message", "模块未安装或未加载");
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "module RPC '%s' method not found: %s", module_id,
                 method && method->valuestring ? method->valuestring : "?");
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "method_not_found");
        cJSON_AddStringToObject(resp, "message", "方法不存在");
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }
    if (err != ESP_OK || !result) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        }
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }
    err = send_json(req, result);
    cJSON_Delete(result);
    return err;
}

#define MODULE_CAM_PREVIEW_FMT "/data/tmp/%s_cam_preview.jpg"

static bool module_id_from_preview_uri(const char *uri, char *id_out, size_t id_len)
{
    const char *prefix = "/api/modules/";
    if (!uri || strncmp(uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    const char *p = uri + strlen(prefix);
    const char *suffix = "/preview";
    const char *end = strstr(p, suffix);
    if (!end || strcmp(end, suffix) != 0) {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len == 0 || len >= id_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    memcpy(id_out, p, len);
    id_out[len] = '\0';
    return true;
}

static esp_err_t handle_api_module_preview(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char module_id[32];
    if (!module_id_from_preview_uri(req->uri, module_id, sizeof(module_id))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad module preview uri");
    }

    char query[192];
    char point_id[MINIHUB_POINT_ID_MAX] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "point_id", point_id, sizeof(point_id)) != ESP_OK ||
        !point_id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
    }

    cJSON *params = cJSON_CreateObject();
    if (!params) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(params, "point_id", point_id);
    cJSON *result = NULL;
    err = minihub_module_invoke_rpc(module_id, "camera_preview", params, &result);
    cJSON_Delete(params);
    if (err == ESP_ERR_INVALID_STATE) {
        cJSON_Delete(result);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "module_not_ready");
    }
    if (err != ESP_OK || !result) {
        cJSON_Delete(result);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }
    const cJSON *empty = cJSON_GetObjectItem(result, "empty");
    const cJSON *ok = cJSON_GetObjectItem(result, "ok");
    if (cJSON_IsTrue(empty)) {
        cJSON_Delete(result);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no snapshot");
    }
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(result);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "preview failed");
    }

    char preview_path[96];
    const cJSON *file = cJSON_GetObjectItem(result, "file");
    if (cJSON_IsString(file) && file->valuestring[0]) {
        strncpy(preview_path, file->valuestring, sizeof(preview_path) - 1);
        preview_path[sizeof(preview_path) - 1] = '\0';
    } else {
        snprintf(preview_path, sizeof(preview_path), MODULE_CAM_PREVIEW_FMT, module_id);
    }
    cJSON_Delete(result);

    FILE *f = fopen(preview_path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no snapshot");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[2048];
    size_t n;
    bool sent_any = false;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            unlink(preview_path);
            return sent_any ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
        sent_any = true;
    }
    fclose(f);
    unlink(preview_path);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handle_root(httpd_req_t *req)
{
#if CONFIG_MINIHUB_WEB_UI
    return send_preinstall_index_or_fallback(req);
#else
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "Web UI disabled", HTTPD_RESP_USE_STRLEN);
#endif
}

#if CONFIG_MINIHUB_WEB_UI
static esp_err_t handle_web_asset(httpd_req_t *req)
{
    const char *prefix = "/web/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *rel = req->uri + strlen(prefix);
    if (!web_path_safe(rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char path[MINIHUB_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", MINIHUB_PREINSTALL_WEB_DIR, rel) >=
        (int)sizeof(path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    esp_err_t err = send_file_chunked(req, path);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    return err;
}
#endif

#if CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB
static bool module_web_id_ok(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        if (n >= MINIHUB_MODULE_ID_MAX - 1) {
            return false;
        }
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/**
 * GET /modules/<id>/web[/<rel>] → /store/modules/<id>/web/<rel>
 * Empty rel or trailing slash → index.html.
 */
static esp_err_t handle_module_web_asset(httpd_req_t *req)
{
    const char *prefix = "/modules/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *p = req->uri + strlen(prefix);
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    char id[MINIHUB_MODULE_ID_MAX];
    size_t id_len = (size_t)(slash - p);
    if (id_len == 0 || id_len >= sizeof(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad module id");
    }
    memcpy(id, p, id_len);
    id[id_len] = '\0';
    if (!module_web_id_ok(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad module id");
    }

    /* Expect "/web" or "/web/..." */
    if (strncmp(slash, "/" MINIHUB_MODULE_WEB_SUBDIR, strlen("/" MINIHUB_MODULE_WEB_SUBDIR)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *after_web = slash + strlen("/" MINIHUB_MODULE_WEB_SUBDIR);
    if (after_web[0] != '\0' && after_web[0] != '/') {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    const char *rel = (after_web[0] == '/') ? (after_web + 1) : "";
    char rel_buf[192];
    if (rel[0] == '\0' || rel[strlen(rel) - 1] == '/') {
        if (snprintf(rel_buf, sizeof(rel_buf), "%sindex.html", rel) >= (int)sizeof(rel_buf)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
        }
        rel = rel_buf;
    }
    if (!web_path_safe(rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }

    char path[MINIHUB_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s/%s/%s", MINIHUB_MODULES_DIR, id,
                 MINIHUB_MODULE_WEB_SUBDIR, rel) >= (int)sizeof(path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    esp_err_t err = send_file_chunked(req, path);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    return err;
}
#endif

static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_api_info(httpd_req_t *req)
{
    const int64_t uptime_ms = esp_timer_get_time() / 1000;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "MiniHub");
    cJSON_AddStringToObject(root, "version", MINIHUB_PRODUCT_VERSION);
    cJSON_AddNumberToObject(root, "api_version", MINIHUB_CORE_API_VERSION);
    cJSON_AddBoolToObject(root, "auth_required", minihub_auth_enabled());
    cJSON_AddNumberToObject(root, "uptime_ms", (double)uptime_ms);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(uptime_ms / 1000));
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static cJSON *lfs_usage_json(const char *label)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    size_t total = 0, used = 0;
    esp_err_t err = esp_littlefs_info(label, &total, &used);
    if (err != ESP_OK) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", esp_err_to_name(err));
        return o;
    }
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "total", (double)total);
    cJSON_AddNumberToObject(o, "used", (double)used);
    cJSON_AddNumberToObject(o, "free", (double)(total > used ? total - used : 0));
    return o;
}

static cJSON *iface_status_json(minihub_net_iface_t iface)
{
    minihub_net_iface_status_t st = {0};
    (void)minihub_net_get_iface_status(iface, &st);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddBoolToObject(o, "present", st.present);
    cJSON_AddBoolToObject(o, "up", st.up);
    cJSON_AddBoolToObject(o, "dhcp", st.dhcp);
    cJSON_AddStringToObject(o, "ip", st.ip);
    cJSON_AddStringToObject(o, "netmask", st.netmask);
    cJSON_AddStringToObject(o, "gw", st.gw);
    cJSON *dns = cJSON_CreateArray();
    if (dns) {
        if (st.dns_main[0]) {
            cJSON_AddItemToArray(dns, cJSON_CreateString(st.dns_main));
        }
        if (st.dns_backup[0]) {
            cJSON_AddItemToArray(dns, cJSON_CreateString(st.dns_backup));
        }
        cJSON_AddItemToObject(o, "dns", dns);
    }
    return o;
}

static esp_err_t handle_api_system(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    const int64_t uptime_ms = esp_timer_get_time() / 1000;
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "name", "MiniHub");
    /* Product version shown in sidebar (hub-ver); keep stable across git builds. */
    cJSON_AddStringToObject(root, "version", MINIHUB_PRODUCT_VERSION);
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    if (app && app->version[0]) {
        cJSON_AddStringToObject(root, "commit", app->version);
    } else {
        cJSON_AddStringToObject(root, "commit", "");
    }
    cJSON_AddNumberToObject(root, "uptime_s", (double)(uptime_ms / 1000));

    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);
    cJSON_AddStringToObject(root, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(root, "chip_cores", chip.cores);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        cJSON_AddNumberToObject(root, "flash_size", (double)flash_size);
    }

    cJSON *ram = cJSON_CreateObject();
    if (ram) {
        cJSON_AddNumberToObject(ram, "internal_free",
                                (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(ram, "internal_total",
                                (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(ram, "psram_free",
                                (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(ram, "psram_total",
                                (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        cJSON_AddItemToObject(root, "ram", ram);
    }

    cJSON_AddItemToObject(root, "storage", lfs_usage_json("storage"));
    cJSON_AddItemToObject(root, "modstore", lfs_usage_json("modstore"));

    cJSON *net = cJSON_CreateObject();
    if (net) {
        cJSON_AddItemToObject(net, "eth", iface_status_json(MINIHUB_NET_IF_ETH));
        cJSON_AddItemToObject(net, "wifi", iface_status_json(MINIHUB_NET_IF_WIFI));
        cJSON_AddItemToObject(root, "net", net);
    }

    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static void copy_json_str(const cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    dst[0] = '\0';
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(dst, v->valuestring, dst_len - 1);
        dst[dst_len - 1] = '\0';
    }
}

static esp_err_t handle_api_net_config(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 2048);
    if (body_err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *iface_j = cJSON_GetObjectItem(root, "iface");
    const cJSON *dhcp_j = cJSON_GetObjectItem(root, "dhcp");
    if (!cJSON_IsString(iface_j) || !iface_j->valuestring[0] || !cJSON_IsBool(dhcp_j)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "iface and dhcp required");
    }

    minihub_net_iface_t iface;
    if (strcmp(iface_j->valuestring, "eth") == 0) {
        iface = MINIHUB_NET_IF_ETH;
    } else if (strcmp(iface_j->valuestring, "wifi") == 0) {
        iface = MINIHUB_NET_IF_WIFI;
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "iface must be eth or wifi");
    }

    minihub_net_iface_config_t cfg = {0};
    cfg.dhcp = cJSON_IsTrue(dhcp_j);
    copy_json_str(root, "ip", cfg.ip, sizeof(cfg.ip));
    copy_json_str(root, "netmask", cfg.netmask, sizeof(cfg.netmask));
    copy_json_str(root, "gw", cfg.gw, sizeof(cfg.gw));

    const cJSON *dns = cJSON_GetObjectItem(root, "dns");
    if (cJSON_IsArray(dns)) {
        const cJSON *d0 = cJSON_GetArrayItem(dns, 0);
        const cJSON *d1 = cJSON_GetArrayItem(dns, 1);
        if (cJSON_IsString(d0) && d0->valuestring) {
            strncpy(cfg.dns_main, d0->valuestring, sizeof(cfg.dns_main) - 1);
        }
        if (cJSON_IsString(d1) && d1->valuestring) {
            strncpy(cfg.dns_backup, d1->valuestring, sizeof(cfg.dns_backup) - 1);
        }
    } else {
        copy_json_str(root, "dns_main", cfg.dns_main, sizeof(cfg.dns_main));
        copy_json_str(root, "dns_backup", cfg.dns_backup, sizeof(cfg.dns_backup));
    }
    cJSON_Delete(root);

    err = minihub_net_set_iface_config(iface, &cfg);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static cJSON *adapters_enabled_json(void)
{
    return minihub_module_overview_adapters_json();
}

static esp_err_t handle_api_adapters(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = adapters_enabled_json();
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

#define GEO_CACHE_TTL_US (3600LL * 1000000LL)
#define GEO_RETRY_BACKOFF_US (60LL * 1000000LL)
#define GEO_API_URL "http://ip-api.com/json/?fields=status,country,regionName,city,lat,lon,query"

typedef struct {
    bool valid;
    int64_t fetched_us;
    double lat;
    double lon;
    char city[64];
    char region[64];
    char country[64];
    char public_ip[48];
} geo_cache_t;

static geo_cache_t s_geo_cache;
static bool s_geo_refresh_inflight;
static int64_t s_geo_last_fail_us;

static void local_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    if (!minihub_net_get_ip(buf, len)) {
        buf[0] = '\0';
    }
}

bool minihub_web_sta_has_ip(void)
{
    return minihub_net_has_ip();
}

bool minihub_web_get_sta_ip(char *buf, size_t len)
{
    return minihub_net_get_ip(buf, len);
}

static esp_err_t fetch_ip_geo(geo_cache_t *out)
{
    if (!minihub_web_sta_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_http_client_config_t cfg = {
        .url = GEO_API_URL,
        .timeout_ms = 8000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    char body[512];
    int n = esp_http_client_read_response(client, body, sizeof(body) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (n <= 0) {
        return ESP_FAIL;
    }
    body[n] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return ESP_FAIL;
    }

    const cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "success") != 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    const cJSON *lat = cJSON_GetObjectItem(root, "lat");
    const cJSON *lon = cJSON_GetObjectItem(root, "lon");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    out->lat = lat->valuedouble;
    out->lon = lon->valuedouble;
    const cJSON *city = cJSON_GetObjectItem(root, "city");
    const cJSON *region = cJSON_GetObjectItem(root, "regionName");
    const cJSON *country = cJSON_GetObjectItem(root, "country");
    const cJSON *query = cJSON_GetObjectItem(root, "query");
    strncpy(out->city, cJSON_IsString(city) ? city->valuestring : "", sizeof(out->city) - 1);
    strncpy(out->region, cJSON_IsString(region) ? region->valuestring : "", sizeof(out->region) - 1);
    strncpy(out->country, cJSON_IsString(country) ? country->valuestring : "", sizeof(out->country) - 1);
    strncpy(out->public_ip, cJSON_IsString(query) ? query->valuestring : "", sizeof(out->public_ip) - 1);
    out->valid = true;
    out->fetched_us = esp_timer_get_time();
    cJSON_Delete(root);
    return ESP_OK;
}

typedef struct {
    geo_cache_t *out;
} geo_worker_ctx_t;

static esp_err_t geo_worker_fn(void *arg)
{
    geo_worker_ctx_t *ctx = arg;
    return fetch_ip_geo(ctx->out);
}

static void geo_apply_cache(const geo_cache_t *fresh);

static void geo_note_fetch_failure(void)
{
    s_geo_last_fail_us = esp_timer_get_time();
}

static bool geo_retry_backoff_active(int64_t now)
{
    return s_geo_last_fail_us != 0 && (now - s_geo_last_fail_us) < GEO_RETRY_BACKOFF_US;
}

static void refresh_geo_cache_if_stale(void)
{
    if (!minihub_web_sta_has_ip()) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_geo_cache.valid && (now - s_geo_cache.fetched_us) <= GEO_CACHE_TTL_US) {
        return;
    }
    if (geo_retry_backoff_active(now)) {
        return;
    }
    geo_cache_t fresh = {0};
    geo_worker_ctx_t wctx = { .out = &fresh };
    if (minihub_http_run_worker(geo_worker_fn, &wctx) == ESP_OK && fresh.valid) {
        geo_apply_cache(&fresh);
        s_geo_last_fail_us = 0;
    } else {
        geo_note_fetch_failure();
    }
}

typedef struct {
    geo_cache_t result;
} geo_refresh_async_ctx_t;

static esp_err_t geo_refresh_async_worker_fn(void *arg)
{
    geo_refresh_async_ctx_t *ctx = arg;
    return fetch_ip_geo(&ctx->result);
}

static void geo_refresh_async_done(esp_err_t err, void *user_ctx)
{
    geo_refresh_async_ctx_t *ctx = user_ctx;
    s_geo_refresh_inflight = false;
    if (err == ESP_OK && ctx->result.valid) {
        geo_apply_cache(&ctx->result);
        s_geo_last_fail_us = 0;
        ESP_LOGI(TAG,
                 "geo refreshed: %s, %s (%.4f, %.4f)",
                 ctx->result.city,
                 ctx->result.public_ip,
                 ctx->result.lat,
                 ctx->result.lon);
        minihub_lcd_request_refresh();
    } else {
        geo_note_fetch_failure();
        ESP_LOGW(TAG, "geo refresh failed: %s", esp_err_to_name(err));
    }
    free(ctx);
}

esp_err_t minihub_web_refresh_geo_async(void)
{
    if (!minihub_web_sta_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_geo_refresh_inflight) {
        return ESP_OK;
    }
    if (geo_retry_backoff_active(esp_timer_get_time())) {
        return ESP_OK;
    }
    geo_refresh_async_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    s_geo_refresh_inflight = true;
    esp_err_t err = minihub_http_run_worker_async(geo_refresh_async_worker_fn, ctx, geo_refresh_async_done, ctx);
    if (err != ESP_OK) {
        s_geo_refresh_inflight = false;
        free(ctx);
    }
    return err;
}

static esp_err_t geo_copy_cache(minihub_geo_t *out)
{
    if (!s_geo_cache.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    out->valid = true;
    out->lat = s_geo_cache.lat;
    out->lon = s_geo_cache.lon;
    strncpy(out->city, s_geo_cache.city, sizeof(out->city) - 1);
    strncpy(out->region, s_geo_cache.region, sizeof(out->region) - 1);
    strncpy(out->country, s_geo_cache.country, sizeof(out->country) - 1);
    strncpy(out->public_ip, s_geo_cache.public_ip, sizeof(out->public_ip) - 1);
    return ESP_OK;
}

esp_err_t minihub_web_peek_geo(minihub_geo_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return geo_copy_cache(out);
}

esp_err_t minihub_web_get_geo(minihub_geo_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    refresh_geo_cache_if_stale();
    return geo_copy_cache(out);
}

#define MAP_SNAPSHOT_ZOOM 13
#define MAP_CACHE_EPSILON 0.02
#define MAP_CACHE_TTL_US (24LL * 3600LL * 1000000LL)
#define MAP_FETCH_RETRY_US (5LL * 60LL * 1000000LL)
#define MAP_HTTP_UA "MiniHub/1.0 (ESP32-S3; +https://github.com/espressif/esp-brookesia)"
/** Fastly anycast for tile.openstreetmap.org (bypass DNS). */
#define OSM_TILE_HOST "tile.openstreetmap.org"
#define OSM_TILE_IP "151.101.193.91"

typedef struct {
    bool valid;
    int64_t fetched_us;
    double lat;
    double lon;
} map_snapshot_cache_t;

static map_snapshot_cache_t s_map_cache;
static int64_t s_map_last_fail_us;

static void geo_invalidate_map_cache_if_moved(const geo_cache_t *prev, const geo_cache_t *next)
{
    if (!prev->valid) {
        s_map_cache.valid = false;
        return;
    }
    if (prev->public_ip[0] != '\0' && next->public_ip[0] != '\0' &&
        strcmp(prev->public_ip, next->public_ip) != 0) {
        s_map_cache.valid = false;
        return;
    }
    if (fabs(prev->lat - next->lat) > MAP_CACHE_EPSILON || fabs(prev->lon - next->lon) > MAP_CACHE_EPSILON) {
        s_map_cache.valid = false;
        return;
    }
    const bool was_china = minihub_map_in_china(prev->lat, prev->lon);
    const bool now_china = minihub_map_in_china(next->lat, next->lon);
    if (was_china != now_china) {
        s_map_cache.valid = false;
    }
}

static void geo_apply_cache(const geo_cache_t *fresh)
{
    if (!fresh || !fresh->valid) {
        return;
    }
    geo_invalidate_map_cache_if_moved(&s_geo_cache, fresh);
    s_geo_cache = *fresh;
}

static bool buffer_is_png(const unsigned char *data, size_t len)
{
    static const unsigned char k_sig[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return data != NULL && len >= sizeof(k_sig) && memcmp(data, k_sig, sizeof(k_sig)) == 0;
}

static bool file_is_png(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    unsigned char sig[8];
    const size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    return n == sizeof(sig) && buffer_is_png(sig, sizeof(sig));
}

static void remove_legacy_map_caches(void)
{
    char legacy[256];
    snprintf(legacy, sizeof(legacy), "%s/ui/cache/map.bin", MINIHUB_VFS_MOUNT);
    unlink(legacy);
}

static esp_err_t http_download_url_to_path(const char *url, const char *http_host, const char *path,
                                           size_t *out_bytes)
{
    if (!url || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = http_host,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "User-Agent", MAP_HTTP_UA);
    esp_http_client_set_header(client, "Accept", "image/png,image/*");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (http_host != NULL) {
        esp_http_client_set_header(client, "Host", http_host);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "map GET open failed (%s): %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "map GET HTTP %d: %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "cannot write %s", path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char buf[4096];
    size_t total = 0;
    while (true) {
        const int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)n;
    }
    fclose(out);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        unlink(path);
        return err;
    }
    if (total < 128) {
        ESP_LOGW(TAG, "map response too small (%u) from %s", (unsigned)total, url);
        unlink(path);
        return ESP_FAIL;
    }
    if (!file_is_png(path)) {
        ESP_LOGW(TAG, "map response not PNG from %s", url);
        unlink(path);
        return ESP_FAIL;
    }
    if (out_bytes) {
        *out_bytes = total;
    }
    return ESP_OK;
}

static esp_err_t fetch_map_snapshot_impl(double lat, double lon, int width_px, int height_px)
{
    if (!minihub_web_sta_has_ip()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width_px < 64 || width_px > 1280 || height_px < 64 || height_px > 1280) {
        return ESP_ERR_INVALID_ARG;
    }

    minihub_map_download_begin();
    esp_err_t result = ESP_FAIL;

    char path[256];
    int pn = snprintf(path, sizeof(path), "%s/%s", MINIHUB_VFS_MOUNT, MINIHUB_MAP_SNAPSHOT_REL);
    if (pn < 0 || pn >= (int)sizeof(path)) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    remove_legacy_map_caches();

    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s failed", dir);
            result = ESP_FAIL;
            goto done;
        }
    }

    char url[512];
    size_t bytes = 0;
    esp_err_t err = ESP_FAIL;
    const char *source = NULL;

    static const minihub_map_tile_source_t gaode_sources[] = {
        { "https://webrd01.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d",
          NULL,
          MINIHUB_MAP_TILE_FMT_AMAP_XYZ,
          true },
        { "https://webrd02.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d",
          NULL,
          MINIHUB_MAP_TILE_FMT_AMAP_XYZ,
          true },
        { "https://webrd03.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d",
          NULL,
          MINIHUB_MAP_TILE_FMT_AMAP_XYZ,
          true },
        { "https://webrd04.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d",
          NULL,
          MINIHUB_MAP_TILE_FMT_AMAP_XYZ,
          true },
    };
    static const minihub_map_tile_source_t osm_sources[] = {
        { "https://" OSM_TILE_IP "/%d/%d/%d.png", OSM_TILE_HOST, MINIHUB_MAP_TILE_FMT_ZXY, false },
        { "https://" OSM_TILE_HOST "/%d/%d/%d.png", NULL, MINIHUB_MAP_TILE_FMT_ZXY, false },
    };

    const bool in_china = minihub_map_in_china(lat, lon);
    const minihub_map_tile_source_t *primary = in_china ? gaode_sources : osm_sources;
    const size_t primary_count = in_china ? sizeof(gaode_sources) / sizeof(gaode_sources[0])
                                          : sizeof(osm_sources) / sizeof(osm_sources[0]);
    const minihub_map_tile_source_t *fallback = in_china ? osm_sources : NULL;
    const size_t fallback_count = in_china ? sizeof(osm_sources) / sizeof(osm_sources[0]) : 0;

    for (size_t pass = 0; pass < 2; ++pass) {
        const minihub_map_tile_source_t *list = pass == 0 ? primary : fallback;
        const size_t count = pass == 0 ? primary_count : fallback_count;
        if (list == NULL || count == 0) {
            continue;
        }
        for (size_t i = 0; i < count; ++i) {
            err = minihub_map_build_tile_snapshot(lat,
                                                  lon,
                                                  MAP_SNAPSHOT_ZOOM,
                                                  width_px,
                                                  height_px,
                                                  &list[i],
                                                  path);
            if (err == ESP_OK) {
                source = list[i].http_host != NULL ? list[i].http_host : list[i].url_template;
                struct stat st;
                if (stat(path, &st) == 0) {
                    bytes = (size_t)st.st_size;
                }
                goto success;
            }
        }
    }

    snprintf(url,
             sizeof(url),
             "https://staticmap.openstreetmap.de/staticmap.php?"
             "center=%.6f,%.6f&zoom=%d&size=%dx%d&maptype=mapnik&markers=%.6f,%.6f,red-pushpin",
             lat,
             lon,
             MAP_SNAPSHOT_ZOOM,
             width_px,
             height_px,
             lat,
             lon);
    err = http_download_url_to_path(url, NULL, path, &bytes);
    if (err == ESP_OK) {
        source = "staticmap.de";
        goto success;
    }

    s_map_last_fail_us = esp_timer_get_time();
    ESP_LOGW(TAG, "map snapshot: all sources failed (DNS/network?)");
    goto done;

success:
    ESP_LOGI(TAG, "map snapshot saved %s (%u bytes, source=%s)", path, (unsigned)bytes, source);
    s_map_last_fail_us = 0;
    s_map_cache.valid = true;
    s_map_cache.fetched_us = esp_timer_get_time();
    s_map_cache.lat = lat;
    s_map_cache.lon = lon;
    result = ESP_OK;

done:
    minihub_map_download_end();
    return result;
}

typedef struct {
    double lat;
    double lon;
    int width_px;
    int height_px;
} map_snapshot_worker_ctx_t;

static esp_err_t map_snapshot_worker_fn(void *arg)
{
    map_snapshot_worker_ctx_t *ctx = arg;
    return fetch_map_snapshot_impl(ctx->lat, ctx->lon, ctx->width_px, ctx->height_px);
}

bool minihub_web_map_snapshot_stale(double lat, double lon)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MINIHUB_VFS_MOUNT, MINIHUB_MAP_SNAPSHOT_REL);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 128 || !file_is_png(path)) {
        return true;
    }
    if (!s_map_cache.valid) {
        return true;
    }
    const int64_t now = esp_timer_get_time();
    if ((now - s_map_cache.fetched_us) > MAP_CACHE_TTL_US) {
        return true;
    }
    if (fabs(lat - s_map_cache.lat) > MAP_CACHE_EPSILON || fabs(lon - s_map_cache.lon) > MAP_CACHE_EPSILON) {
        return true;
    }
    return false;
}

bool minihub_web_map_snapshot_should_fetch(double lat, double lon)
{
    if (!minihub_web_map_snapshot_stale(lat, lon)) {
        return false;
    }
    if (s_map_last_fail_us != 0) {
        const int64_t now = esp_timer_get_time();
        if ((now - s_map_last_fail_us) < MAP_FETCH_RETRY_US) {
            return false;
        }
    }
    return true;
}

esp_err_t minihub_web_fetch_map_snapshot(double lat, double lon, int width_px, int height_px)
{
    map_snapshot_worker_ctx_t ctx = {
        .lat = lat,
        .lon = lon,
        .width_px = width_px,
        .height_px = height_px,
    };
    return minihub_http_run_worker(map_snapshot_worker_fn, &ctx);
}

typedef struct {
    map_snapshot_worker_ctx_t params;
    minihub_map_snapshot_done_cb_t cb;
    void *user_ctx;
} map_snapshot_async_ctx_t;

static esp_err_t map_snapshot_async_worker_fn(void *arg)
{
    map_snapshot_async_ctx_t *ctx = arg;
    return fetch_map_snapshot_impl(ctx->params.lat,
                                   ctx->params.lon,
                                   ctx->params.width_px,
                                   ctx->params.height_px);
}

static void map_snapshot_async_done(esp_err_t err, void *user_ctx)
{
    map_snapshot_async_ctx_t *ctx = user_ctx;
    minihub_map_snapshot_done_cb_t cb = ctx->cb;
    void *cb_user = ctx->user_ctx;
    free(ctx);
    if (cb) {
        cb(err, cb_user);
    }
}

esp_err_t minihub_web_fetch_map_snapshot_async(double lat, double lon, int width_px, int height_px,
                                                 minihub_map_snapshot_done_cb_t cb, void *user_ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    map_snapshot_async_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->params.lat = lat;
    ctx->params.lon = lon;
    ctx->params.width_px = width_px;
    ctx->params.height_px = height_px;
    ctx->cb = cb;
    ctx->user_ctx = user_ctx;
    esp_err_t err = minihub_http_run_worker_async(map_snapshot_async_worker_fn, ctx, map_snapshot_async_done, ctx);
    if (err != ESP_OK) {
        free(ctx);
    }
    return err;
}

static esp_err_t handle_api_location(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    /* Never block httpd on outbound geo HTTP — that stalled /api/points and UI device refresh.
     * Return cache immediately; kick async refresh when stale/forced. */
    const bool force_refresh = strstr(req->uri, "refresh=1") != NULL;
    int64_t now = esp_timer_get_time();
    const bool stale = !s_geo_cache.valid || (now - s_geo_cache.fetched_us) > GEO_CACHE_TTL_US;
    if (force_refresh || stale) {
        (void)minihub_web_refresh_geo_async();
    }

    char local_ip[16] = {0};
    local_ip_str(local_ip, sizeof(local_ip));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", s_geo_cache.valid);
    if (local_ip[0]) {
        cJSON_AddStringToObject(root, "local_ip", local_ip);
    }
    if (s_geo_cache.valid) {
        cJSON_AddStringToObject(root, "source", "ip");
        cJSON_AddNumberToObject(root, "lat", s_geo_cache.lat);
        cJSON_AddNumberToObject(root, "lon", s_geo_cache.lon);
        if (s_geo_cache.city[0]) {
            cJSON_AddStringToObject(root, "city", s_geo_cache.city);
        }
        if (s_geo_cache.region[0]) {
            cJSON_AddStringToObject(root, "region", s_geo_cache.region);
        }
        if (s_geo_cache.country[0]) {
            cJSON_AddStringToObject(root, "country", s_geo_cache.country);
        }
        cJSON_AddStringToObject(root, "map_provider", minihub_map_in_china(s_geo_cache.lat, s_geo_cache.lon) ? "amap" : "osm");
        if (s_geo_cache.public_ip[0]) {
            cJSON_AddStringToObject(root, "public_ip", s_geo_cache.public_ip);
        }
        char address[256];
        snprintf(address, sizeof(address), "%s, %s, %s",
                 s_geo_cache.city, s_geo_cache.region, s_geo_cache.country);
        cJSON_AddStringToObject(root, "address", address);
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static cJSON *point_to_json(const minihub_point_t *p)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "point_id", p->point_id);
    cJSON_AddStringToObject(obj, "type", p->type);
    cJSON_AddStringToObject(obj, "value", p->value);
    cJSON_AddStringToObject(obj, "adapter", p->adapter);
    cJSON_AddBoolToObject(obj, "online", p->online);
    cJSON_AddNumberToObject(obj, "updated_ms", (double)p->updated_ms);
    if (p->props) {
        cJSON_AddItemToObject(obj, "props", cJSON_Duplicate(p->props, true));
    }
    return obj;
}

static esp_err_t handle_api_points_history(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char query[160];
    char point_id[MINIHUB_POINT_ID_MAX] = {0};
    char limit_buf[16] = {0};
    size_t limit = 48;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "point_id", point_id, sizeof(point_id)) != ESP_OK ||
        !point_id[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
    }
    if (httpd_query_key_value(query, "limit", limit_buf, sizeof(limit_buf)) == ESP_OK) {
        int lim = atoi(limit_buf);
        if (lim > 0 && lim <= 128) {
            limit = (size_t)lim;
        }
    }
    if (!minihub_point_get(point_id)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    cJSON *obj = minihub_point_history_json(point_id, limit);
    if (!obj) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    err = send_json(req, obj);
    cJSON_Delete(obj);
    return err;
}

static esp_err_t handle_api_points(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char point_id[MINIHUB_POINT_ID_MAX];
        if (httpd_query_key_value(query, "point_id", point_id, sizeof(point_id)) == ESP_OK) {
            minihub_point_t snap = {0};
            err = minihub_point_get_snapshot(point_id, &snap);
            if (err != ESP_OK) {
                return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
            }
            cJSON *obj = point_to_json(&snap);
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
            err = send_json(req, obj);
            cJSON_Delete(obj);
            return err;
        }
    }

    minihub_point_t *points = NULL;
    size_t count = 0;
    err = minihub_point_list(&points, &count);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "list failed");
    }

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, point_to_json(&points[i]));
    }
    free(points);

    err = send_json(req, arr);
    cJSON_Delete(arr);
    return err;
}

static esp_err_t handle_api_command(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *point_id = cJSON_GetObjectItem(root, "point_id");
    const cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(point_id) || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id/cmd");
    }

    minihub_command_t command = {0};
    strncpy(command.point_id, point_id->valuestring, sizeof(command.point_id) - 1);
    strncpy(command.cmd, cmd->valuestring, sizeof(command.cmd) - 1);
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsObject(params)) {
        command.params = cJSON_Duplicate(params, true);
    }

    err = minihub_command_invoke(&command);
    if (command.params) {
        cJSON_Delete(command.params);
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#define RULES_JSON_MAX_BYTES (32 * 1024)
#define RULES_IMPORT_URL_MAX 512

static esp_err_t rules_http_get_body(const char *url, char **out_body, size_t *out_len)
{
    if (!url || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    if (out_len) {
        *out_len = 0;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t cap = 4096;
    size_t total = 0;
    char *buf = malloc(cap);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    while (true) {
        if (total >= RULES_JSON_MAX_BYTES) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        if (total + 512 > cap) {
            size_t ncap = cap * 2;
            if (ncap > RULES_JSON_MAX_BYTES + 1) {
                ncap = RULES_JSON_MAX_BYTES + 1;
            }
            char *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
            cap = ncap;
        }
        size_t space = (cap > total + 1) ? (cap - total - 1) : 0;
        if (space == 0) {
            free(buf);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        int n = esp_http_client_read(client, buf + total, (int)space);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        total += (size_t)n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    if (total > RULES_JSON_MAX_BYTES) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    buf[total] = '\0';
    *out_body = buf;
    if (out_len) {
        *out_len = total;
    }
    return ESP_OK;
}

static bool rules_url_allowed(const char *url)
{
    return url && (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static esp_err_t handle_api_rules(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *rules = minihub_config_get_rules();
    if (!rules) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no rules");
    }
    return send_json(req, rules);
}

static esp_err_t rules_import_apply_root(httpd_req_t *req, cJSON *root)
{
    const cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (!cJSON_IsArray(rules)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing rules array");
    }
    int count = cJSON_GetArraySize(rules);

    esp_err_t err = minihub_config_save_rules(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }

    err = minihub_rules_reload(minihub_config_get_rules());
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reload failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "count", count);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_rules_import(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *body = NULL;
    size_t body_len = 0;
    /* Direct POST may send full rules.json (≤32 KiB); URL-only body is small. */
    err = read_http_body(req, &body, &body_len, RULES_JSON_MAX_BYTES);
    if (err != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *req_json = cJSON_Parse(body);
    free(body);
    if (!req_json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    /* Mode A: body is rules document {"rules":[...]} */
    const cJSON *inline_rules = cJSON_GetObjectItem(req_json, "rules");
    if (cJSON_IsArray(inline_rules)) {
        return rules_import_apply_root(req, req_json);
    }

    /* Mode B: {"url":"http(s)://..."} — Hub fetches remote file */
    const cJSON *url_item = cJSON_GetObjectItem(req_json, "url");
    if (!cJSON_IsString(url_item) || !url_item->valuestring || !url_item->valuestring[0]) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url or rules");
    }
    if (!rules_url_allowed(url_item->valuestring) ||
        strlen(url_item->valuestring) >= RULES_IMPORT_URL_MAX) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad url");
    }

    char url[RULES_IMPORT_URL_MAX];
    strncpy(url, url_item->valuestring, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    cJSON_Delete(req_json);

    char *remote = NULL;
    size_t remote_len = 0;
    err = rules_http_get_body(url, &remote, &remote_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rules too large");
    }
    if (err != ESP_OK || !remote) {
        /* esp_http_server has no 502 enum; use 500 for upstream fetch failure */
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fetch failed");
    }

    cJSON *root = cJSON_Parse(remote);
    free(remote);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid rules json");
    }
    return rules_import_apply_root(req, root);
}

#define RULES_AI_REQ_MAX_BYTES  (16 * 1024)
#define RULES_AI_RESP_MAX_BYTES (24 * 1024)
#define RULES_AI_URL_MAX        640

/** Browser → Hub → OpenAI-compatible chat/completions (avoids CORS; key not stored). */
static esp_err_t handle_api_rules_ai_chat(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *body = NULL;
    size_t body_len = 0;
    err = read_http_body(req, &body, &body_len, RULES_AI_REQ_MAX_BYTES);
    if (err != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *req_json = cJSON_Parse(body);
    free(body);
    body = NULL;
    if (!req_json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *base_item = cJSON_GetObjectItem(req_json, "base_url");
    const cJSON *model_item = cJSON_GetObjectItem(req_json, "model");
    const cJSON *key_item = cJSON_GetObjectItem(req_json, "api_key");
    const cJSON *messages = cJSON_GetObjectItem(req_json, "messages");
    if (!cJSON_IsString(base_item) || !base_item->valuestring || !base_item->valuestring[0] ||
        !cJSON_IsString(model_item) || !model_item->valuestring || !model_item->valuestring[0] ||
        !cJSON_IsString(key_item) || !key_item->valuestring || !key_item->valuestring[0] ||
        !cJSON_IsArray(messages) || cJSON_GetArraySize(messages) < 1) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need base_url model api_key messages");
    }

    const char *base = base_item->valuestring;
    if ((strncmp(base, "https://", 8) != 0 && strncmp(base, "http://", 7) != 0) ||
        strstr(base, "..") != NULL || strlen(base) >= 500) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad base_url");
    }

    char url[RULES_AI_URL_MAX];
    size_t blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') {
        blen--;
    }
    if (snprintf(url, sizeof(url), "%.*s/chat/completions", (int)blen, base) >= (int)sizeof(url)) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "base_url too long");
    }

    cJSON *upstream = cJSON_CreateObject();
    if (!upstream) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(upstream, "model", model_item->valuestring);
    cJSON_AddNumberToObject(upstream, "temperature", 0.2);
    cJSON *msgs_copy = cJSON_Duplicate(messages, true);
    if (!msgs_copy) {
        cJSON_Delete(upstream);
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddItemToObject(upstream, "messages", msgs_copy);

    char *post_body = cJSON_PrintUnformatted(upstream);
    cJSON_Delete(upstream);
    if (!post_body) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    char auth_hdr[768];
    if (snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", key_item->valuestring) >= (int)sizeof(auth_hdr)) {
        free(post_body);
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "api_key too long");
    }
    /* Drop parsed request (includes key) before long network wait. */
    cJSON_Delete(req_json);
    req_json = NULL;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(post_body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "http init");
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    memset(auth_hdr, 0, sizeof(auth_hdr));

    const int post_len = (int)strlen(post_body);
    err = esp_http_client_open(client, post_len);
    if (err != ESP_OK) {
        free(post_body);
        esp_http_client_cleanup(client);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream open");
    }
    int w = esp_http_client_write(client, post_body, post_len);
    free(post_body);
    post_body = NULL;
    if (w < 0 || w != post_len) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream write");
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    char *resp = malloc(RULES_AI_RESP_MAX_BYTES + 1);
    if (!resp) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int total = 0;
    while (total < RULES_AI_RESP_MAX_BYTES) {
        int n = esp_http_client_read(client, resp + total, RULES_AI_RESP_MAX_BYTES - total);
        if (n < 0) {
            free(resp);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream read");
        }
        if (n == 0) {
            break;
        }
        total += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    resp[total] = '\0';

    if (status < 200 || status >= 300) {
        cJSON *err_wrap = cJSON_CreateObject();
        if (err_wrap) {
            cJSON_AddNumberToObject(err_wrap, "upstream_status", status);
            cJSON_AddStringToObject(err_wrap, "upstream_body", resp);
            free(resp);
            httpd_resp_set_status(req, "502 Bad Gateway");
            httpd_resp_set_type(req, "application/json");
            char *ej = cJSON_PrintUnformatted(err_wrap);
            cJSON_Delete(err_wrap);
            if (!ej) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            }
            err = httpd_resp_send(req, ej, HTTPD_RESP_USE_STRLEN);
            free(ej);
            return err;
        }
        free(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream error");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, resp, total);
    free(resp);
    return err;
}

static esp_err_t handle_api_scenes(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    /* Full scenes.json document (same shape as rules GET) for Web CRUD. */
    cJSON *scenes = minihub_config_get_scenes();
    if (!scenes) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no scenes");
    }
    return send_json(req, scenes);
}

static esp_err_t scenes_import_apply_root(httpd_req_t *req, cJSON *root)
{
    const cJSON *scenes = cJSON_GetObjectItem(root, "scenes");
    if (!cJSON_IsArray(scenes)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing scenes array");
    }
    int count = cJSON_GetArraySize(scenes);
    esp_err_t err = minihub_config_save_scenes(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    err = minihub_scenes_reload(minihub_config_get_scenes());
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reload failed");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "count", count);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_scenes_import(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *body = NULL;
    size_t body_len = 0;
    err = read_http_body(req, &body, &body_len, RULES_JSON_MAX_BYTES);
    if (err != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    cJSON *req_json = cJSON_Parse(body);
    free(body);
    if (!req_json) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *inline_scenes = cJSON_GetObjectItem(req_json, "scenes");
    if (!cJSON_IsArray(inline_scenes)) {
        cJSON_Delete(req_json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing scenes");
    }
    err = scenes_import_apply_root(req, req_json);
    cJSON_Delete(req_json);
    return err;
}

static esp_err_t handle_api_scene_invoke(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *id = cJSON_GetObjectItem(root, "scene_id");
    if (!cJSON_IsString(id)) {
        id = cJSON_GetObjectItem(root, "id");
    }
    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing scene_id");
    }

    err = minihub_scenes_invoke(id->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_events(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *events = minihub_event_bus_recent_json();
    if (!events) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no events");
    }
    err = send_json(req, events);
    cJSON_Delete(events);
    return err;
}

static esp_err_t send_conflict(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg ? msg : "busy", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_api_ota_status(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "state", (double)minihub_ota_get_state());
    cJSON_AddStringToObject(resp, "error", minihub_ota_get_error());
    cJSON_AddNumberToObject(resp, "progress", (double)minihub_ota_get_progress());
    cJSON_AddNumberToObject(resp, "bytes_written", (double)minihub_ota_get_bytes_written());
    cJSON_AddStringToObject(resp, "source", minihub_ota_source_str(minihub_ota_get_source()));
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_ota_start(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
    }

    err = minihub_ota_start_url(url->valuestring);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_conflict(req, "busy");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "started");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_ota_upload(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    size_t expected = 0;
    if (req->content_len > 0) {
        expected = (size_t)req->content_len;
    }
    if (expected == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing Content-Length");
    }

    err = minihub_ota_upload_begin(expected);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_conflict(req, "busy");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    char buf[4096];
    size_t remaining = expected;
    while (remaining > 0) {
        int to_read = (remaining > sizeof(buf)) ? (int)sizeof(buf) : (int)remaining;
        int n = httpd_req_recv(req, buf, to_read);
        if (n <= 0) {
            minihub_ota_upload_abort("upload recv failed");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        err = minihub_ota_upload_write(buf, (size_t)n);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, minihub_ota_get_error());
        }
        remaining -= (size_t)n;
    }

    err = minihub_ota_upload_finish();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, minihub_ota_get_error());
    }

    /* Device will reboot shortly; still return JSON if possible. */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_zigbee_permit(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    int seconds = 60;
    char buf[64];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *sec = cJSON_GetObjectItem(root, "seconds");
            if (cJSON_IsNumber(sec)) {
                seconds = sec->valueint;
            }
            cJSON_Delete(root);
        }
    }

    cJSON *params = NULL;
    (void)params;
    esp_err_t zb_err = minihub_zigbee_permit_join(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", zb_err == ESP_OK ? "ok" : "fail");
    if (zb_err != ESP_OK) {
        if (zb_err == ESP_ERR_INVALID_STATE && minihub_zigbee_permit_join_active()) {
            cJSON_AddStringToObject(resp, "error", "Zigbee 正在入网，请勿重复开启");
        } else if (zb_err == ESP_ERR_INVALID_STATE && minihub_ble_mesh_provisioning_busy()) {
            cJSON_AddStringToObject(resp, "error", "BLE Mesh 正在配网，无法同时开启 Zigbee 入网");
        } else if (zb_err == ESP_ERR_INVALID_STATE && !minihub_zigbee_running()) {
            cJSON_AddStringToObject(resp, "error", "Zigbee coordinator not ready (network forming)");
        } else if (zb_err == ESP_ERR_INVALID_ARG) {
            cJSON_AddStringToObject(resp, "error", "seconds 无效（须 >= 0；0 关闭窗口）");
        } else {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(zb_err));
        }
    }
    cJSON_AddBoolToObject(resp, "ready", minihub_zigbee_running());
    cJSON_AddBoolToObject(resp, "permit_join", minihub_zigbee_permit_join_active());
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_zigbee_install_code(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    const cJSON *ieee_j = cJSON_GetObjectItem(root, "ieee");
    const cJSON *ic_j = cJSON_GetObjectItem(root, "install_code");
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    if (!cJSON_IsString(ieee_j) || !cJSON_IsString(ic_j)) {
        cJSON_Delete(root);
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "need ieee + install_code strings");
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }
    uint64_t ieee = 0;
    if (minihub_zb_parse_ieee(ieee_j->valuestring, &ieee) != 0) {
        cJSON_Delete(root);
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "ieee parse failed");
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }
    esp_err_t zb_err = minihub_zigbee_add_install_code(ieee, ic_j->valuestring);
    cJSON_Delete(root);
    cJSON_AddStringToObject(resp, "result", zb_err == ESP_OK ? "ok" : "fail");
    if (zb_err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(zb_err));
    }
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_ble_mesh_permit(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    int seconds = 60;
    char buf[64];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *sec = cJSON_GetObjectItem(root, "seconds");
            if (cJSON_IsNumber(sec)) {
                seconds = sec->valueint;
            }
            cJSON_Delete(root);
        }
    }

    cJSON *params = NULL;
    (void)params;
    esp_err_t bm_err = minihub_ble_mesh_permit_provision(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", bm_err == ESP_OK ? "ok" : "fail");
    if (bm_err != ESP_OK) {
        if (bm_err == ESP_ERR_INVALID_STATE && minihub_ble_mesh_provisioning_busy()) {
            cJSON_AddStringToObject(resp, "error", "BLE Mesh 正在配网，请勿重复开启");
        } else if (bm_err == ESP_ERR_INVALID_STATE && minihub_zigbee_permit_join_active()) {
            cJSON_AddStringToObject(resp, "error", "Zigbee 正在入网，无法同时开启 BLE Mesh 配网");
        } else if (bm_err == ESP_ERR_INVALID_ARG) {
            cJSON_AddStringToObject(resp, "error", "seconds 无效（须 >= 0；0 关闭窗口）");
        } else {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(bm_err));
        }
    }
    cJSON_AddBoolToObject(resp, "permit_provision", minihub_ble_mesh_permit_provision_active());
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_radio_provision_status(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    const bool zb = minihub_zigbee_permit_join_active();
    const bool bm_permit = minihub_ble_mesh_permit_provision_active();
    const bool bm_busy = minihub_ble_mesh_provisioning_busy();
    cJSON_AddBoolToObject(resp, "zigbee_permit_join", zb);
    cJSON_AddBoolToObject(resp, "ble_mesh_permit_provision", bm_permit);
    cJSON_AddBoolToObject(resp, "ble_mesh_provisioning", bm_busy);
    cJSON_AddBoolToObject(resp, "busy", zb || bm_busy);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

/** Best-effort: drop manual devices[] entry from adapters.json for adapter name. */
static void remove_adapters_json_device(const char *adapter_name, const char *point_id)
{
    if (!adapter_name || !point_id) {
        return;
    }
    cJSON *root = minihub_config_get_adapters();
    if (!root) {
        return;
    }
    cJSON *adapters = cJSON_GetObjectItem(root, "adapters");
    cJSON *ad = adapters ? cJSON_GetObjectItem(adapters, adapter_name) : NULL;
    cJSON *devices = ad ? cJSON_GetObjectItem(ad, "devices") : NULL;
    if (!cJSON_IsArray(devices)) {
        return;
    }
    const int count = cJSON_GetArraySize(devices);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(devices, i);
        const cJSON *pid = cJSON_GetObjectItem(item, "point_id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, point_id) == 0) {
            cJSON_DeleteItemFromArray(devices, i);
            (void)minihub_config_save_adapters(root);
            ESP_LOGI(TAG, "removed %s from adapters.json %s.devices", point_id, adapter_name);
            return;
        }
    }
}

static esp_err_t handle_api_zigbee_devices(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    if (!cJSON_IsTrue(del)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only delete supported");
    }
    const cJSON *point_id = cJSON_GetObjectItem(root, "point_id");
    if (!cJSON_IsString(point_id) || !point_id->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
    }
    char point_buf[96];
    strncpy(point_buf, point_id->valuestring, sizeof(point_buf) - 1);
    point_buf[sizeof(point_buf) - 1] = '\0';
    cJSON_Delete(root);

    err = minihub_zigbee_remove_device(point_buf);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }
    remove_adapters_json_device("zigbee", point_buf);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "point_id", point_buf);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_ble_mesh_devices(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    if (!cJSON_IsTrue(del)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only delete supported");
    }
    const cJSON *point_id = cJSON_GetObjectItem(root, "point_id");
    if (!cJSON_IsString(point_id) || !point_id->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
    }
    char point_buf[96];
    strncpy(point_buf, point_id->valuestring, sizeof(point_buf) - 1);
    point_buf[sizeof(point_buf) - 1] = '\0';
    cJSON_Delete(root);

    err = minihub_ble_mesh_remove_device(point_buf);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }
    remove_adapters_json_device("ble_mesh", point_buf);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "point_id", point_buf);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_modules(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *modules = minihub_module_list_json();
    if (!modules) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no modules");
    }
    err = send_json(req, modules);
    cJSON_Delete(modules);
    return err;
}

static esp_err_t handle_api_modules_manage(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
    }

    char id_copy[64];
    strncpy(id_copy, id->valuestring, sizeof(id_copy) - 1);
    id_copy[sizeof(id_copy) - 1] = '\0';

    if (cJSON_IsTrue(del)) {
        err = minihub_module_delete(id_copy);
    } else if (cJSON_IsBool(enabled)) {
        err = minihub_module_set_enabled(id_copy, cJSON_IsTrue(enabled));
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled/delete");
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "id", id_copy);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_packs(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *packs = minihub_pack_list_json();
    if (!packs) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no packs");
    }
    err = send_json(req, packs);
    cJSON_Delete(packs);
    return err;
}

static esp_err_t handle_api_packs_manage(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing id");
    }

    char id_copy[64];
    strncpy(id_copy, id->valuestring, sizeof(id_copy) - 1);
    id_copy[sizeof(id_copy) - 1] = '\0';

    if (cJSON_IsTrue(del)) {
        err = minihub_pack_delete(id_copy);
    } else if (cJSON_IsBool(enabled)) {
        err = minihub_pack_set_enabled(id_copy, cJSON_IsTrue(enabled));
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled/delete");
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "id", id_copy);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#if CONFIG_MINIHUB_STORE
static esp_err_t handle_api_store_catalog(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *catalog = minihub_store_catalog_merged();
    if (!catalog) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "catalog failed");
    }
    err = send_json(req, catalog);
    cJSON_Delete(catalog);
    return err;
}

static esp_err_t handle_api_store_refresh(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_store_refresh_catalog();
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_RESPONSE) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "invalid catalog");
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    cJSON *catalog = minihub_store_catalog_merged();
    if (!catalog) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "catalog failed");
    }
    err = send_json(req, catalog);
    cJSON_Delete(catalog);
    return err;
}

#endif /* CONFIG_MINIHUB_STORE */

static esp_err_t handle_api_store_config_get(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    char custom[MINIHUB_STORE_BASE_URL_MAX];
    char def_url[MINIHUB_STORE_BASE_URL_MAX];
    bool disable_default = false;
    err = minihub_store_get_config(custom, sizeof(custom), &disable_default, def_url, sizeof(def_url));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "custom_url", custom);
    cJSON_AddBoolToObject(resp, "disable_default", disable_default);
    cJSON_AddStringToObject(resp, "default_url", def_url);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_store_config_set(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[320];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *custom = cJSON_GetObjectItem(root, "custom_url");
    const cJSON *dis = cJSON_GetObjectItem(root, "disable_default");
    const char *custom_url = cJSON_IsString(custom) ? custom->valuestring : "";
    bool disable_default = false;
    if (cJSON_IsBool(dis)) {
        disable_default = cJSON_IsTrue(dis);
    } else {
        (void)minihub_store_get_config(NULL, 0, &disable_default, NULL, 0);
    }

    err = minihub_store_set_config(custom_url, disable_default);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid config");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    char out_custom[MINIHUB_STORE_BASE_URL_MAX];
    char def_url[MINIHUB_STORE_BASE_URL_MAX];
    bool out_dis = false;
    (void)minihub_store_get_config(out_custom, sizeof(out_custom), &out_dis, def_url, sizeof(def_url));
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "custom_url", out_custom);
    cJSON_AddBoolToObject(resp, "disable_default", out_dis);
    cJSON_AddStringToObject(resp, "default_url", def_url);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#if CONFIG_MINIHUB_STORE
static esp_err_t handle_api_store_install(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *kind = cJSON_GetObjectItem(root, "kind");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    const cJSON *upgrade = cJSON_GetObjectItem(root, "upgrade");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);
    bool do_upgrade = cJSON_IsBool(upgrade) && cJSON_IsTrue(upgrade);

    if (!cJSON_IsString(kind) || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing kind/id");
    }

    err = do_upgrade ? minihub_store_upgrade(kind->valuestring, id->valuestring)
                     : minihub_store_install(kind->valuestring, id->valuestring);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, 409, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_store_upgrade(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *kind = cJSON_GetObjectItem(root, "kind");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (!cJSON_IsString(kind) || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing kind/id");
    }

    err = minihub_store_upgrade(kind->valuestring, id->valuestring);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, 409, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_store_uninstall(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *kind = cJSON_GetObjectItem(root, "kind");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (!cJSON_IsString(kind) || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing kind/id");
    }

    err = minihub_store_uninstall(kind->valuestring, id->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}
#endif /* CONFIG_MINIHUB_STORE */

#if CONFIG_MINIHUB_MATTER_ADAPTER
/* pairing_code / QR + optional wifi_ssid/password for ble_wifi commission. */
#define MATTER_API_BODY_MAX 1460

static esp_err_t handle_api_matter_status(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *status = minihub_matter_status_json();
    if (!status) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no status");
    }
    err = send_json(req, status);
    cJSON_Delete(status);
    return err;
}

static esp_err_t handle_api_matter_commission(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[MATTER_API_BODY_MAX];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
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

    minihub_matter_discovery_t discovery = MINIHUB_MATTER_DISCOVERY_NETWORK;
    if (cJSON_IsString(disc) && disc->valuestring) {
        if (strcmp(disc->valuestring, "ble_wifi") == 0 ||
            strcmp(disc->valuestring, "ble-wifi") == 0 ||
            strcmp(disc->valuestring, "first_time") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_WIFI;
        }
    }

    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    if (cJSON_IsString(code)) {
        err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery, ssid_s, pass_s);
    } else if (cJSON_IsNumber(pin)) {
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "ble_wifi requires pairing_code (not setup_pin)");
        }
        err = minihub_matter_commission_pin(node_id, (uint32_t)pin->valueint);
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need pairing_code or setup_pin");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "started" : esp_err_to_name(err));
    if (err != ESP_OK && minihub_matter_last_error()) {
        cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
    }
    cJSON_Delete(root);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t parse_matter_endpoints(const cJSON *root, uint16_t *out, size_t max_out, size_t *out_count)
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
    out[1] = 2;
    *out_count = 2;
    return ESP_OK;
}

static esp_err_t handle_api_matter_devices(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[MATTER_API_BODY_MAX];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *del = cJSON_GetObjectItem(root, "delete");
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    bool do_reload = !cJSON_IsBool(reload) || cJSON_IsTrue(reload);

    if (cJSON_IsTrue(del)) {
        const cJSON *point_id = cJSON_GetObjectItem(root, "point_id");
        if (!cJSON_IsString(point_id)) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
        }
        char point_buf[96];
        strncpy(point_buf, point_id->valuestring, sizeof(point_buf) - 1);
        point_buf[sizeof(point_buf) - 1] = '\0';
        err = minihub_matter_device_remove(point_buf);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
        }
        if (do_reload) {
            minihub_hub_reload();
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "result", "ok");
        cJSON_AddStringToObject(resp, "point_id", point_buf);
        cJSON_AddBoolToObject(resp, "reloaded", do_reload);
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
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
        err = minihub_matter_next_node_id(&node_id);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        }
    }

    uint16_t endpoints[16];
    size_t ep_count = 0;
    err = parse_matter_endpoints(root, endpoints, sizeof(endpoints) / sizeof(endpoints[0]), &ep_count);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid endpoints");
    }

    minihub_matter_discovery_t discovery = MINIHUB_MATTER_DISCOVERY_NETWORK;
    if (cJSON_IsString(disc) && disc->valuestring) {
        if (strcmp(disc->valuestring, "ble_wifi") == 0 ||
            strcmp(disc->valuestring, "ble-wifi") == 0 ||
            strcmp(disc->valuestring, "first_time") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_WIFI;
        }
    }
    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    esp_err_t commission_err = ESP_OK;
    if (cJSON_IsString(code) && code->valuestring[0]) {
        commission_err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery,
                                                           ssid_s, pass_s);
    }

    size_t synced = 0;
    err = minihub_matter_devices_upsert(node_id, endpoints, ep_count, merge_existing, &synced);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
    cJSON_AddNumberToObject(resp, "synced", (double)synced);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    if (commission_err == ESP_OK) {
        cJSON_AddStringToObject(resp, "commission", "started");
    } else {
        cJSON_AddStringToObject(resp, "commission", esp_err_to_name(commission_err));
        if (minihub_matter_last_error()) {
            cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
        }
    }
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */

static esp_err_t handle_reload(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_hub_reload();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

#define ADAPTERS_JSON_MAX_BYTES (64 * 1024)

static esp_err_t read_http_body(httpd_req_t *req, char **out_buf, size_t *out_len, size_t max_bytes)
{
    const size_t total = req->content_len;
    if (total == 0 || total > max_bytes) {
        ESP_LOGW(TAG, "request body size %u exceeds max %u", (unsigned)total, (unsigned)max_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < total) {
        const int chunk = httpd_req_recv(req, buf + received, total - received);
        if (chunk <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }
    buf[received] = '\0';
    *out_buf = buf;
    *out_len = received;
    return ESP_OK;
}

static esp_err_t handle_api_config_adapters_get(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *cfg = minihub_config_get_adapters();
    if (!cfg) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no config");
    }
    return send_json(req, cfg);
}

static esp_err_t handle_api_config_adapters_put(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *body = NULL;
    size_t body_len = 0;
    err = read_http_body(req, &body, &body_len, ADAPTERS_JSON_MAX_BYTES);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    bool do_reload = true;
    const cJSON *reload = cJSON_GetObjectItem(root, "reload");
    if (cJSON_IsBool(reload)) {
        do_reload = cJSON_IsTrue(reload);
        cJSON_DeleteItemFromObject(root, "reload");
    }

    const cJSON *adapters = cJSON_GetObjectItem(root, "adapters");
    if (!cJSON_IsObject(adapters)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing adapters object");
    }

    err = minihub_config_save_adapters(root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    cJSON_Delete(root);

    if (do_reload) {
        err = minihub_hub_reload();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#if CONFIG_MINIHUB_DEV_FS_UPLOAD
/* minihub_sc.otf (3000 CJK subset) is ~1.5 MiB; JSON UI files stay well under 256 KiB. */
#define DEV_FS_UPLOAD_MAX_BYTES (2 * 1024 * 1024)

static esp_err_t handle_api_dev_fs_put(httpd_req_t *req)
{
    static const char prefix[] = "/api/dev/fs/";
    const char *uri = req->uri;
    if (strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const char *rel = uri + sizeof(prefix) - 1;
    if (strncmp(rel, "ui/", 3) != 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "only ui/ paths allowed");
    }

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t     err = read_http_body(req, &body, &body_len, DEV_FS_UPLOAD_MAX_BYTES);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    err = minihub_fs_write_file(rel, body, body_len);
    free(body);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "path", rel);
    cJSON_AddNumberToObject(resp, "bytes", (double)body_len);
    cJSON_AddStringToObject(resp, "hint", "reboot device to reload LCD UI");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}
#endif

static esp_err_t minihub_web_add_uri(httpd_handle_t server, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri->uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t minihub_web_attach(void *httpd, const minihub_web_attach_config_t *cfg)
{
    httpd_handle_t server = (httpd_handle_t)httpd;
    minihub_web_attach_config_t local = {
        .index_uri = "/",
        .index_path = NULL,
        .register_favicon = true,
        .start_websocket = true,
    };
    const char *index_uri;
    esp_err_t err;

    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg) {
        local = *cfg;
    }
    index_uri = (local.index_uri && local.index_uri[0]) ? local.index_uri : "/";
    s_index_override = (local.index_path && local.index_path[0]) ? local.index_path : NULL;
    s_server = server;

    httpd_uri_t root = { .uri = index_uri, .method = HTTP_GET, .handler = handle_root };
#if CONFIG_MINIHUB_WEB_UI
    httpd_uri_t web_assets = { .uri = "/web/*", .method = HTTP_GET, .handler = handle_web_asset };
#endif
#if CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB
    httpd_uri_t module_web = { .uri = "/modules/*", .method = HTTP_GET,
                               .handler = handle_module_web_asset };
#endif
    httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon };
    httpd_uri_t api = { .uri = "/api/", .method = HTTP_GET, .handler = handle_api_info };
    httpd_uri_t api_system = { .uri = "/api/system", .method = HTTP_GET, .handler = handle_api_system };
    httpd_uri_t api_net_cfg = { .uri = "/api/net/config", .method = HTTP_POST, .handler = handle_api_net_config };
    httpd_uri_t adapters_api = { .uri = "/api/adapters", .method = HTTP_GET, .handler = handle_api_adapters };
    httpd_uri_t config_adapters_get = { .uri = "/api/config/adapters", .method = HTTP_GET,
                                        .handler = handle_api_config_adapters_get };
    httpd_uri_t config_adapters_put = { .uri = "/api/config/adapters", .method = HTTP_PUT,
                                        .handler = handle_api_config_adapters_put };
    httpd_uri_t location_api = { .uri = "/api/location", .method = HTTP_GET, .handler = handle_api_location };
    httpd_uri_t points = { .uri = "/api/points", .method = HTTP_GET, .handler = handle_api_points };
    httpd_uri_t points_history = { .uri = "/api/points/history", .method = HTTP_GET,
                                   .handler = handle_api_points_history };
    httpd_uri_t command = { .uri = "/api/command", .method = HTTP_POST, .handler = handle_api_command };
    httpd_uri_t rules = { .uri = "/api/rules", .method = HTTP_GET, .handler = handle_api_rules };
    httpd_uri_t rules_import = { .uri = "/api/rules/import", .method = HTTP_POST,
                                 .handler = handle_api_rules_import };
    httpd_uri_t rules_ai_chat = { .uri = "/api/rules/ai/chat", .method = HTTP_POST,
                                  .handler = handle_api_rules_ai_chat };
    httpd_uri_t packs = { .uri = "/api/packs", .method = HTTP_GET, .handler = handle_api_packs };
    httpd_uri_t packs_manage = { .uri = "/api/packs", .method = HTTP_POST, .handler = handle_api_packs_manage };
    httpd_uri_t modules = { .uri = "/api/modules", .method = HTTP_GET, .handler = handle_api_modules };
    httpd_uri_t modules_manage = { .uri = "/api/modules", .method = HTTP_POST, .handler = handle_api_modules_manage };
    httpd_uri_t scenes = { .uri = "/api/scenes", .method = HTTP_GET, .handler = handle_api_scenes };
    httpd_uri_t scenes_import = { .uri = "/api/scenes/import", .method = HTTP_POST,
                                  .handler = handle_api_scenes_import };
    httpd_uri_t scene_invoke = { .uri = "/api/scenes/invoke", .method = HTTP_POST, .handler = handle_api_scene_invoke };
    httpd_uri_t events = { .uri = "/api/events", .method = HTTP_GET, .handler = handle_api_events };
    httpd_uri_t ota_status = { .uri = "/api/ota", .method = HTTP_GET, .handler = handle_api_ota_status };
    httpd_uri_t ota_start = { .uri = "/api/ota", .method = HTTP_POST, .handler = handle_api_ota_start };
    httpd_uri_t ota_upload = { .uri = "/api/ota/upload", .method = HTTP_POST, .handler = handle_api_ota_upload };
    httpd_uri_t zb_permit = { .uri = "/api/zigbee/permit_join", .method = HTTP_POST, .handler = handle_api_zigbee_permit };
    httpd_uri_t zb_install_code = { .uri = "/api/zigbee/install_code", .method = HTTP_POST,
                                    .handler = handle_api_zigbee_install_code };
    httpd_uri_t zb_devices = { .uri = "/api/zigbee/devices", .method = HTTP_POST, .handler = handle_api_zigbee_devices };
    httpd_uri_t ble_mesh_permit = { .uri = "/api/ble_mesh/permit_provision", .method = HTTP_POST,
                                    .handler = handle_api_ble_mesh_permit };
    httpd_uri_t ble_mesh_devices = { .uri = "/api/ble_mesh/devices", .method = HTTP_POST,
                                     .handler = handle_api_ble_mesh_devices };
    httpd_uri_t radio_prov = { .uri = "/api/radio/provision", .method = HTTP_GET,
                               .handler = handle_api_radio_provision_status };
    httpd_uri_t reload = { .uri = "/api/hub/reload", .method = HTTP_POST, .handler = handle_reload };
    httpd_uri_t module_rpc = { .uri = "/api/modules/*", .method = HTTP_POST,
                               .handler = handle_api_module_rpc };
    httpd_uri_t module_preview = { .uri = "/api/modules/*/preview", .method = HTTP_GET,
                                   .handler = handle_api_module_preview };
#if CONFIG_MINIHUB_MATTER_ADAPTER
    httpd_uri_t matter_status = { .uri = "/api/matter/status", .method = HTTP_GET, .handler = handle_api_matter_status };
    httpd_uri_t matter_commission = { .uri = "/api/matter/commission", .method = HTTP_POST,
                                      .handler = handle_api_matter_commission };
    httpd_uri_t matter_devices = { .uri = "/api/matter/devices", .method = HTTP_POST,
                                   .handler = handle_api_matter_devices };
#endif

    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &root), TAG, "index %s", index_uri);
#if CONFIG_MINIHUB_WEB_UI
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &web_assets), TAG, "/web/*");
#endif
#if CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_web), TAG, "/modules/*");
#endif
    if (local.register_favicon) {
        ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &favicon), TAG, "/favicon.ico");
    }
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api), TAG, "/api/");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_system), TAG, "/api/system");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_net_cfg), TAG, "/api/net/config");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &adapters_api), TAG, "/api/adapters");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &config_adapters_get), TAG, "/api/config/adapters GET");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &config_adapters_put), TAG, "/api/config/adapters PUT");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &location_api), TAG, "/api/location");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points), TAG, "/api/points");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points_history), TAG, "/api/points/history");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &command), TAG, "/api/command");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &rules), TAG, "/api/rules");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &rules_import), TAG, "/api/rules/import");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &rules_ai_chat), TAG, "/api/rules/ai/chat");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &packs), TAG, "/api/packs");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &packs_manage), TAG, "/api/packs POST");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &modules), TAG, "/api/modules");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &modules_manage), TAG, "/api/modules POST");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &scenes), TAG, "/api/scenes");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &scenes_import), TAG, "/api/scenes/import");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &scene_invoke), TAG, "/api/scenes/invoke");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &events), TAG, "/api/events");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ota_status), TAG, "/api/ota");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ota_start), TAG, "/api/ota POST");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ota_upload), TAG, "/api/ota/upload");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_permit), TAG, "/api/zigbee/permit_join");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_install_code), TAG, "/api/zigbee/install_code");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_devices), TAG, "/api/zigbee/devices");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ble_mesh_permit), TAG, "/api/ble_mesh/permit_provision");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ble_mesh_devices), TAG, "/api/ble_mesh/devices");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &radio_prov), TAG, "/api/radio/provision");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &reload), TAG, "/api/hub/reload");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_rpc), TAG, "/api/modules/*");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_preview), TAG, "/api/modules/*/preview");
#if CONFIG_MINIHUB_MATTER_ADAPTER
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_status), TAG, "/api/matter/status");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_commission), TAG, "/api/matter/commission");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_devices), TAG, "/api/matter/devices");
#endif

#if CONFIG_MINIHUB_STORE
    httpd_uri_t store_catalog = { .uri = "/api/store/catalog", .method = HTTP_GET,
                                  .handler = handle_api_store_catalog };
    httpd_uri_t store_refresh = { .uri = "/api/store/refresh", .method = HTTP_POST,
                                  .handler = handle_api_store_refresh };
    httpd_uri_t store_install = { .uri = "/api/store/install", .method = HTTP_POST,
                                  .handler = handle_api_store_install };
    httpd_uri_t store_upgrade = { .uri = "/api/store/upgrade", .method = HTTP_POST,
                                  .handler = handle_api_store_upgrade };
    httpd_uri_t store_uninstall = { .uri = "/api/store/uninstall", .method = HTTP_POST,
                                    .handler = handle_api_store_uninstall };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_catalog), TAG, "/api/store/catalog");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_refresh), TAG, "/api/store/refresh");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_install), TAG, "/api/store/install");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_upgrade), TAG, "/api/store/upgrade");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_uninstall), TAG, "/api/store/uninstall");
#endif
    httpd_uri_t store_config_get = { .uri = "/api/store/config", .method = HTTP_GET,
                                     .handler = handle_api_store_config_get };
    httpd_uri_t store_config_set = { .uri = "/api/store/config", .method = HTTP_POST,
                                     .handler = handle_api_store_config_set };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_config_get), TAG, "/api/store/config GET");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_config_set), TAG, "/api/store/config POST");

#if CONFIG_MINIHUB_DEV_FS_UPLOAD
    httpd_uri_t dev_fs_put = {
        .uri = "/api/dev/fs/*",
        .method = HTTP_PUT,
        .handler = handle_api_dev_fs_put,
    };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &dev_fs_put), TAG, "/api/dev/fs/*");
#endif

    if (local.start_websocket) {
        err = minihub_web_ws_start(server);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket init failed: %s", esp_err_to_name(err));
        }
    }

    {
        const char *web_ui_state = "off";
        const char *index_state = "n/a";
#if defined(CONFIG_MINIHUB_WEB_UI) && CONFIG_MINIHUB_WEB_UI
        struct stat st;
        web_ui_state = "on";
        if (stat(MINIHUB_PREINSTALL_WEB_INDEX, &st) == 0 && S_ISREG(st.st_mode)) {
            index_state = "preinstall";
        } else if (s_index_override &&
                   stat(s_index_override, &st) == 0 && S_ISREG(st.st_mode)) {
            index_state = "override";
        } else {
            index_state = "fallback";
        }
#endif
        ESP_LOGI(TAG, "MiniHub routes attached index=%s web_ui=%s console=%s ws=%d",
                 index_uri, web_ui_state, index_state, (int)local.start_websocket);
    }
    return ESP_OK;
}

esp_err_t minihub_web_start(uint16_t port)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    /* Default ctrl_port (32768) collides with RainMaker protocomm_httpd
     * (on-network chal_resp) which also uses HTTPD_DEFAULT_CONFIG. */
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;
    config.max_uri_handlers = 60;
    config.max_open_sockets = 20;
    /* Store install/upgrade may HTTP-download .mhub on this task (esp_http_client
     * + local buffers). 8K overflows; 16K matches typical IDF httpd+client use. */
    config.stack_size = 16384;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = minihub_web_attach(s_server, NULL);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server on port %u", port);
    return ESP_OK;
}

void *minihub_web_httpd(void)
{
    return s_server;
}
