/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_web.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
#include "esp_rom_crc.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "minihub_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minihub_provision.h"
#include "minihub_net_cfg.h"
#include "minihub_core.h"
#include "minihub_http_worker.h"
#include "minihub_config.h"
#include "minihub_device_widget.h"
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
#if CONFIG_MINIHUB_THREAD_ADAPTER
#include "minihub_thread.h"
#endif
#include "minihub_map.h"
#include "minihub_map_tiles.h"
#include "minihub_paths.h"
#if CONFIG_MINIHUB_ZIGBEE_SDK
#include "minihub_zigbee.h"
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
#include "minihub_ble_mesh.h"
#endif
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_littlefs.h"
#include "esp_app_desc.h"

static const char *TAG = "minihub_web";

/* Product version shown in the sidebar / system page (not git describe).
 * Kconfig string; quoted by sdkconfig.h. Fallback keeps host builds that
 * have not regenerated sdkconfig.h on the same default. */
#ifndef CONFIG_MINIHUB_PRODUCT_VERSION
#define CONFIG_MINIHUB_PRODUCT_VERSION "0.1.0"
#endif

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
extern const char MINIHUB_INDEX_HTML_FALLBACK_ZH[];
extern const char MINIHUB_INDEX_HTML_FALLBACK_EN[];
#endif

static httpd_handle_t s_server;
/** Optional extra index.html (attach config); NULL = preinstall path only. */
static const char *s_index_override;
/** Console mount path (e.g. /minihub); used for login link web_base. */
static const char *s_index_uri = "/";

static esp_err_t read_http_body(httpd_req_t *req, char **out_buf, size_t *out_len, size_t max_bytes);
static esp_err_t send_json(httpd_req_t *req, cJSON *json);

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

/* littlefs reports no usable mtime here (st_mtime comes back 0xffffffff), so a
 * size+mtime tag would degrade to size-only and 304 a same-length edit. Fold a
 * CRC32 over the bytes instead; the extra read pass is cheap next to the send. */
static bool file_etag(const char *path, off_t size, char *out, size_t out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t buf[512];
    uint32_t crc = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = esp_rom_crc32_le(crc, buf, n);
    }
    fclose(f);
    snprintf(out, out_len, "\"%lx-%lx\"", (unsigned long)size, (unsigned long)crc);
    return true;
}

#if CONFIG_MINIHUB_WEB_UI
/** Prefer /data/web; fall back to legacy /store/preinstall/web for unflashed storage. */
static bool resolve_host_web_file(const char *rel_or_null, char *out, size_t out_len, struct stat *st)
{
    static const char *const roots[] = {
        MINIHUB_WEB_DIR,
        MINIHUB_PREINSTALL_WEB_DIR,
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        int n;
        if (rel_or_null && rel_or_null[0]) {
            n = snprintf(out, out_len, "%s/%s", roots[i], rel_or_null);
        } else {
            n = snprintf(out, out_len, "%s", roots[i]);
        }
        if (n < 0 || (size_t)n >= out_len) {
            continue;
        }
        if (stat(out, st) == 0 && S_ISREG(st->st_mode)) {
            return true;
        }
    }
    return false;
}
#endif

static esp_err_t send_file_chunked(httpd_req_t *req, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Console JS/CSS is ~300 KB and no-cache means the browser revalidates every
     * load; without an ETag that revalidation re-sends the whole body. */
    char etag[32];
    if (!file_etag(path, st.st_size, etag, sizeof(etag))) {
        return ESP_ERR_NOT_FOUND;
    }
    char inm[64];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    /* 16KB PSRAM chunk, and do not shrink this without re-measuring: a 4 KB
     * chunk costs ~1 s per chunk here (4 KB splits into 3 MSS segments and each
     * write waits on the peer's delayed ACK), so the console's ~300 KB of
     * JS/CSS took 30 s+ to load. At 16 KB the same assets run 50-100x faster.
     * PSRAM keeps the console HTML off the internal heap. */
    const size_t cap = 16384;
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
    httpd_resp_set_hdr(req, "ETag", etag);
    size_t n;
    bool sent_any = false;
    while ((n = fread(buf, 1, cap, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            /* Peer reset (errno 104) or SO_SNDTIMEO (errno 11). No empty trailer. */
            return sent_any ? ESP_ERR_INVALID_STATE : ESP_FAIL;
        }
        sent_any = true;
    }
    free(buf);
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool http_want_zh(httpd_req_t *req)
{
    char lang[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Accept-Language", lang, sizeof(lang)) != ESP_OK) {
        return false;
    }
    return strcasestr(lang, "zh") != NULL;
}

/* Human-readable API text follows the client's language. Browsers send
 * Accept-Language on every fetch; a client that sends none (a script, an MCP
 * bridge) gets English, which is the machine-readable default. */
static const char *api_msg(httpd_req_t *req, const char *zh, const char *en)
{
    return http_want_zh(req) ? zh : en;
}

static esp_err_t send_preinstall_index_or_fallback(httpd_req_t *req)
{
    const char *index_paths[] = {
        MINIHUB_WEB_INDEX,
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
    ESP_LOGW(TAG, "missing %s (legacy %s)%s%s — serving ROM fallback",
             MINIHUB_WEB_INDEX, MINIHUB_PREINSTALL_WEB_INDEX,
             s_index_override ? " and " : "",
             s_index_override ? s_index_override : "");
    const char *html = http_want_zh(req) ? MINIHUB_INDEX_HTML_FALLBACK_ZH : MINIHUB_INDEX_HTML_FALLBACK_EN;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}
#endif

static esp_err_t send_login_redirect(httpd_req_t *req)
{
    minihub_auth_clear_session_cookie(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

#if CONFIG_MINIHUB_WEB_UI
/* Send login.html with an inline auth-state block so the page never has to
 * fetch a JSON endpoint. Browser JSON-viewer extensions replace the current
 * document when they see a JSON fetch response, which breaks the login form.
 * Uses the same chunked send as send_file_chunked() to avoid a second big
 * heap allocation (a single httpd_resp_send() of a freshly malloc'd buffer
 * was triggering ECONNRESET/uri-handler-failed under memory pressure). */
static esp_err_t send_login_page_injected(httpd_req_t *req, const char *path, bool expired)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    /* login.html is a few KB. If something huge/garbled was uploaded to
     * /data/web/login.html, skip injection and just stream it. */
    if (st.st_size > 256 * 1024) {
        return send_file_chunked(req, path);
    }

    char *file_buf = (char *)malloc((size_t)st.st_size + 1);
    if (!file_buf) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        free(file_buf);
        return ESP_ERR_NOT_FOUND;
    }
    size_t file_len = fread(file_buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (file_len == 0) {
        free(file_buf);
        return ESP_ERR_NOT_FOUND;
    }
    file_buf[file_len] = '\0';

    const char *inject_marker = "</head>";
    char *inject_pos = strcasestr(file_buf, inject_marker);
    size_t prefix_len = inject_pos ? (size_t)(inject_pos - file_buf) : 0;

    char script[256];
    int script_len = snprintf(script, sizeof(script),
                              "<script>window.__mhAuth={login_auth:%s,password_set:%s,"
                              "logged_in:%s,expired:%s};</script>",
                              minihub_auth_web_login_enabled() ? "true" : "false",
                              minihub_auth_password_set() ? "true" : "false",
                              minihub_auth_check(req) == ESP_OK ? "true" : "false",
                              expired ? "true" : "false");
    if (script_len < 0 || script_len >= (int)sizeof(script)) {
        free(file_buf);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    esp_err_t err = ESP_OK;
    if (httpd_resp_send_chunk(req, file_buf, prefix_len) != ESP_OK) {
        err = ESP_FAIL;
    } else if (httpd_resp_send_chunk(req, script, (size_t)script_len) != ESP_OK) {
        err = ESP_FAIL;
    } else if (httpd_resp_send_chunk(req, file_buf + prefix_len, file_len - prefix_len) != ESP_OK) {
        err = ESP_FAIL;
    } else if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
        err = ESP_FAIL;
    }

    free(file_buf);
    return err;
}
#endif

static esp_err_t handle_login_page(httpd_req_t *req)
{
#if CONFIG_MINIHUB_WEB_UI
    /* reason=expired is the console's "session is over" hop. Kill the session
     * on this response so the page cannot bounce back into device APIs. */
    char query[64];
    char reason[16];
    bool expired = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "reason", reason, sizeof(reason)) == ESP_OK &&
        strcmp(reason, "expired") == 0) {
        expired = true;
        minihub_auth_logout();
        minihub_auth_clear_session_cookie(req);
    }
    char path[MINIHUB_PATH_MAX];
    struct stat st;
    if (resolve_host_web_file("login.html", path, sizeof(path), &st)) {
        return send_login_page_injected(req, path, expired);
    }
    ESP_LOGW(TAG, "missing %s (legacy %s)", MINIHUB_WEB_LOGIN, MINIHUB_PREINSTALL_WEB_LOGIN);
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req,
                           http_want_zh(req) ? "登录页未安装" : "login page not installed",
                           HTTPD_RESP_USE_STRLEN);
#else
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
#endif
}

static esp_err_t handle_api_auth_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "username", MINIHUB_WEB_USERNAME);
    cJSON_AddBoolToObject(root, "login_auth", minihub_auth_web_login_enabled());
    cJSON_AddBoolToObject(root, "password_set", minihub_auth_password_set());
    cJSON_AddBoolToObject(root, "logged_in", minihub_auth_check(req) == ESP_OK);
    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_api_auth_setup(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 512);
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *user = cJSON_GetObjectItem(root, "username");
    const cJSON *pw = cJSON_GetObjectItem(root, "password");
    char u[16] = {0};
    char p[80] = {0};
    if (cJSON_IsString(user) && user->valuestring) {
        strncpy(u, user->valuestring, sizeof(u) - 1);
    }
    if (cJSON_IsString(pw) && pw->valuestring) {
        strncpy(p, pw->valuestring, sizeof(p) - 1);
    }
    cJSON_Delete(root);
    esp_err_t err = minihub_auth_setup_password(u, p);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "password already set", HTTPD_RESP_USE_STRLEN);
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username must be admin; password 6-64 chars");
    }
    char sess[MINIHUB_AUTH_SESS_HEX_LEN + 1];
    err = minihub_auth_login(u, p, sess);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "login after setup failed");
    }
    minihub_auth_set_session_cookie(req, sess);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_auth_login(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 512);
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *user = cJSON_GetObjectItem(root, "username");
    const cJSON *pw = cJSON_GetObjectItem(root, "password");
    char u[16] = {0};
    char p[80] = {0};
    if (cJSON_IsString(user) && user->valuestring) {
        strncpy(u, user->valuestring, sizeof(u) - 1);
    }
    if (cJSON_IsString(pw) && pw->valuestring) {
        strncpy(p, pw->valuestring, sizeof(p) - 1);
    }
    cJSON_Delete(root);
    char sess[MINIHUB_AUTH_SESS_HEX_LEN + 1];
    esp_err_t err = minihub_auth_login(u, p, sess);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }
    minihub_auth_set_session_cookie(req, sess);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_auth_logout(httpd_req_t *req)
{
    minihub_auth_logout();
    minihub_auth_clear_session_cookie(req);
    /* Plain text so that browser JSON-viewer extensions cannot replace the
     * login page with this response when the page sends a best-effort logout. */
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_auth(httpd_req_t *req);

static esp_err_t handle_api_auth_change(httpd_req_t *req)
{
    esp_err_t auth_err = api_auth(req);
    if (auth_err != ESP_OK) {
        return auth_err;
    }
    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 512);
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    const cJSON *user = cJSON_GetObjectItem(root, "username");
    const cJSON *old = cJSON_GetObjectItem(root, "old_password");
    const cJSON *nw = cJSON_GetObjectItem(root, "new_password");
    char u[16] = {0};
    char o[80] = {0};
    char n[80] = {0};
    if (cJSON_IsString(user) && user->valuestring) {
        strncpy(u, user->valuestring, sizeof(u) - 1);
    }
    if (cJSON_IsString(old) && old->valuestring) {
        strncpy(o, old->valuestring, sizeof(o) - 1);
    }
    if (cJSON_IsString(nw) && nw->valuestring) {
        strncpy(n, nw->valuestring, sizeof(n) - 1);
    }
    cJSON_Delete(root);

    esp_err_t err = minihub_auth_change_password(u, o, n);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "login auth disabled");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, "wrong old password", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "username must be admin; new password 6-64 chars");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "change failed");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t api_auth(httpd_req_t *req)
{
    if (minihub_auth_check(req) != ESP_OK) {
        /* Drop stale cookie so the SPA cannot keep acting on a cached UI. */
        minihub_auth_clear_session_cookie(req);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
        /* httpd_resp_send_err() returns ESP_OK once the 401 is on the wire, and
         * every caller gates on `err != ESP_OK` — returning it would let the
         * handler run anyway and append a second, unauthenticated JSON block to
         * the same connection (device list, adapter config, location). ESP_FAIL
         * makes httpd close the socket instead. */
        return ESP_FAIL;
    }
    /* Writes are deliberate user actions, so they count as presence; the GET
     * polls the console runs every second deliberately do not. */
    switch (req->method) {
    case HTTP_POST:
    case HTTP_PUT:
    case HTTP_PATCH:
    case HTTP_DELETE:
        minihub_auth_touch(req);
        break;
    default:
        break;
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
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
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
        cJSON_AddStringToObject(resp, "message", api_msg(req, "模块未安装或未加载",
                                                          "module not installed or loaded"));
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
        cJSON_AddStringToObject(resp, "message", api_msg(req, "方法不存在", "method not found"));
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
    if (minihub_auth_web_login_enabled() && minihub_auth_check(req) != ESP_OK) {
        return send_login_redirect(req);
    }
    /* Document load = the user is here. Only navigations reset the idle timer;
     * API polls and static asset GETs deliberately do not. */
    minihub_auth_touch(req);
    return send_preinstall_index_or_fallback(req);
#else
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "Web UI disabled", HTTPD_RESP_USE_STRLEN);
#endif
}

#if CONFIG_MINIHUB_WEB_UI
static esp_err_t handle_web_asset(httpd_req_t *req)
{
    /* The login page is the first thing a user sees and it localises itself, so it
     * needs the string files and the helper before it can fetch /api/auth/status.
     * Both paths carry UI strings only — no device data, no credentials — so they are
     * the deliberate exception to the session gate. Everything else under /web/ and
     * /img/ still requires a session. */
    const bool public_i18n = (strncmp(req->uri, "/web/i18n/", strlen("/web/i18n/")) == 0) ||
                             (strcmp(req->uri, "/web/js/i18n.js") == 0);
    if (!public_i18n && minihub_auth_web_login_enabled() && minihub_auth_check(req) != ESP_OK) {
        return send_login_redirect(req);
    }
    const char *prefix = "/web/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *rel = req->uri + strlen(prefix);
    if (!web_path_safe(rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char path[MINIHUB_PATH_MAX];
    struct stat st;
    if (!resolve_host_web_file(rel, path, sizeof(path), &st)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    esp_err_t err = send_file_chunked(req, path);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    return err;
}

/* GET /img/<subdir>/<file> → <storage>/images/<subdir>/<file>
 *
 * Device / control icons live in storage (/data/images, see minihub_paths.h and
 * the images/ staging in application/minihub/main/CMakeLists.txt) so they can be
 * added or replaced at runtime next to devices.csv, without a firmware rebuild.
 * The directory is split by what the icon depicts: devices/ holds the
 * device-class avatars, parameters/ the control mockups. The relative path is
 * picked from the device attributes by minihub_device_widget_image_name() when
 * /api/device_widgets is served, so this handler only maps a URI to a file
 * inside the icon directory: the path is confined there (no leading "/", no
 * "..") and the extension is limited to the image types we ship. */
static esp_err_t handle_device_image(httpd_req_t *req)
{
    if (minihub_auth_web_login_enabled() && minihub_auth_check(req) != ESP_OK) {
        return send_login_redirect(req);
    }
    const char *prefix = MINIHUB_IMAGES_URI_PREFIX;
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *rel = req->uri + strlen(prefix);
    const char *dot = strrchr(rel, '.');
    if (!web_path_safe(rel) || rel[0] == '.' || !dot ||
        (strcasecmp(dot, ".svg") != 0 && strcasecmp(dot, ".png") != 0)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char path[MINIHUB_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", MINIHUB_IMAGES_DIR, rel) >= (int)sizeof(path)) {
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
    if (minihub_auth_web_login_enabled() && minihub_auth_check(req) != ESP_OK) {
        return send_login_redirect(req);
    }
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
    cJSON_AddStringToObject(root, "version", CONFIG_MINIHUB_PRODUCT_VERSION);
    cJSON_AddNumberToObject(root, "api_version", MINIHUB_CORE_API_VERSION);
    cJSON_AddBoolToObject(root, "auth_required", minihub_auth_enabled());
    cJSON_AddBoolToObject(root, "password_set", minihub_auth_password_set());
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
    cJSON_AddStringToObject(root, "version", CONFIG_MINIHUB_PRODUCT_VERSION);
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

    {
        char local_ip[16] = {0};
        if (minihub_net_get_ip(local_ip, sizeof(local_ip)) && local_ip[0]) {
            cJSON_AddStringToObject(root, "local_ip", local_ip);
        }
        if (s_index_uri && strcmp(s_index_uri, "/") != 0) {
            cJSON_AddStringToObject(root, "web_base", s_index_uri);
        } else {
            cJSON_AddStringToObject(root, "web_base", "");
        }
    }

    cJSON *net = cJSON_CreateObject();
    if (net) {
        cJSON_AddItemToObject(net, "eth", iface_status_json(MINIHUB_NET_IF_ETH));
        cJSON *wifi = iface_status_json(MINIHUB_NET_IF_WIFI);
        if (wifi) {
            char ssid[33] = {0};
            bool has_password = false;
            if (minihub_net_get_wifi_sta(ssid, sizeof(ssid), &has_password) == ESP_OK) {
                cJSON_AddStringToObject(wifi, "ssid", ssid);
                cJSON_AddBoolToObject(wifi, "has_password", has_password);
            }
        }
        cJSON_AddItemToObject(net, "wifi", wifi);
        cJSON_AddItemToObject(root, "net", net);
    }

    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static void reboot_deferred(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handle_api_system_reboot(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 128);
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm required");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsTrue(cJSON_GetObjectItem(root, "confirm"))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm required");
    }
    cJSON_Delete(root);

    if (xTaskCreate(reboot_deferred, "web_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_system_reset(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 128);
    if (body_err != ESP_OK || !buf) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm required");
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsTrue(cJSON_GetObjectItem(root, "confirm"))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "confirm required");
    }
    cJSON_Delete(root);

    err = minihub_factory_reset_schedule();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
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

static esp_err_t handle_api_net_wifi(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 512);
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

    const cJSON *ssid_j = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(ssid_j) || !ssid_j->valuestring || ssid_j->valuestring[0] == '\0' ||
        strlen(ssid_j->valuestring) > 32) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }

    const cJSON *pass_j = cJSON_GetObjectItem(root, "password");
    const char *password = (cJSON_IsString(pass_j) && pass_j->valuestring) ? pass_j->valuestring : "";
    if (strlen(password) > 64) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password too long");
    }
    bool open_network = cJSON_IsTrue(cJSON_GetObjectItem(root, "open"));

    char ssid[33];
    char pass_buf[65];
    strncpy(ssid, ssid_j->valuestring, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(pass_buf, password, sizeof(pass_buf) - 1);
    pass_buf[sizeof(pass_buf) - 1] = '\0';
    cJSON_Delete(root);

    err = minihub_net_set_wifi_sta(ssid, pass_buf, open_network);
    memset(pass_buf, 0, sizeof(pass_buf));

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
    if (!obj) {
        return NULL;
    }
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
            if (!obj) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
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
    if (!arr) {
        minihub_point_list_free(points, count);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = point_to_json(&points[i]);
        if (item) {
            cJSON_AddItemToArray(arr, item);
        }
    }
    minihub_point_list_free(points, count);

    err = send_json(req, arr);
    cJSON_Delete(arr);
    return err;
}

/* ---- device-widget registry bridge (data-driven device catalog) ---- */

/* Which language to serve catalog labels in: ?lang= wins, then Accept-Language,
 * then Chinese. Anything unrecognised is treated as "zh" — a usable catalog
 * beats a strict one, and the frontend always sends the short code. */
static const char *dw_request_lang(httpd_req_t *req)
{
    char query[160];
    char lang[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "lang", lang, sizeof(lang)) == ESP_OK &&
        lang[0]) {
        if (strcasecmp(lang, "en") == 0) {
            return "en";
        }
        return "zh";
    }
    return http_want_zh(req) ? "zh" : "en";
}

/* One parse per catalog entry: the widget id is needed for every response, and
 * `labels[lang]` is the display name for this one. style_json carries no flat
 * `label`, so a language the catalog does not provide falls back to zh and then
 * to the entry's default label (the parameter name when even zh is missing). */
static void dw_style_describe(const char *style_json, const char *lang,
                              char *widget, size_t widget_sz,
                              char *label, size_t label_sz)
{
    widget[0] = '\0';
    label[0] = '\0';
    if (!style_json) {
        return;
    }
    cJSON *root = cJSON_Parse(style_json);
    if (!root) {
        return;
    }
    cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "widget");
    if (cJSON_IsString(w) && w->valuestring) {
        snprintf(widget, widget_sz, "%s", w->valuestring);
    }
    cJSON *labels = cJSON_GetObjectItemCaseSensitive(root, "labels");
    if (labels) {
        cJSON *l = cJSON_GetObjectItemCaseSensitive(labels, lang);
        if (!cJSON_IsString(l) || !l->valuestring[0]) {
            l = cJSON_GetObjectItemCaseSensitive(labels, "zh");
        }
        if (cJSON_IsString(l) && l->valuestring[0]) {
            snprintf(label, label_sz, "%s", l->valuestring);
        }
    }
    cJSON_Delete(root);
}

/* GET /api/device_widgets
 * Serves the loaded registry as JSON for the UI and the rules editor: every
 * parameter of the type is offered; hiding one is a property of the registered
 * POINT (props.params.<name>.visible), which the UI intersects with this catalog.
 * Each device carries its device-class `image` (avatar, /img/devices/...).
 * Parameters carry `label` (in the language asked for, from the entry's
 * style_json.labels map), `widget`, and `style` (min/max/unit/options);
 * interactive controls are CSS widgets — param `image` is always empty
 * (legacy field kept for clients). `style` is passed through verbatim, so its
 * `labels` map and the per-option `labels` reach the frontend.
 * See minihub_device_widget.h. */
static esp_err_t handle_api_device_widgets(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    const char *lang = dw_request_lang(req);

    cJSON *root = cJSON_CreateObject();
    cJSON *devs = cJSON_CreateArray();
    if (!root || !devs) {
        cJSON_Delete(root);
        cJSON_Delete(devs);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddItemToObject(root, "devices", devs);
    char ibuf[MINIHUB_DW_IMAGE_PATH_MAX];   /* resolved icon path (subdir/name) */
    size_t ndev = minihub_device_widget_device_count();
    for (size_t i = 0; i < ndev; i++) {
        const minihub_device_widget_device_t *d = minihub_device_widget_device_at(i);
        if (!d) {
            continue;
        }
        cJSON *dj = cJSON_CreateObject();
        cJSON_AddStringToObject(dj, "device", d->device);
        /* Device-class icon: resolved from the type against storage at render
         * time (never baked into the firmware), served by GET /img/ . Empty
         * when this device type has no icon (NULL parameter = device icon). */
        char dicon[MINIHUB_DW_IMAGE_PATH_MAX + 16] = "";
        if (minihub_device_widget_image_name(d->device, NULL, ibuf, sizeof(ibuf))) {
            snprintf(dicon, sizeof(dicon), "%s%s", MINIHUB_IMAGES_URI_PREFIX, ibuf);
        }
        cJSON_AddStringToObject(dj, "image", dicon);
        cJSON *params = cJSON_CreateArray();
        for (size_t j = 0; j < d->count; j++) {
            const minihub_device_widget_entry_t *e = &d->entries[j];
            cJSON *pj = cJSON_CreateObject();
            char wbuf[32];
            char lbuf[64];
            dw_style_describe(e->style_json, lang, wbuf, sizeof(wbuf), lbuf, sizeof(lbuf));
            cJSON_AddStringToObject(pj, "parameter", e->parameter);
            cJSON_AddStringToObject(pj, "label", lbuf[0] ? lbuf : e->label);
            cJSON_AddStringToObject(pj, "widget", wbuf);
            /* Controls are CSS widgets; do not resolve /img/parameters mockups. */
            cJSON_AddStringToObject(pj, "image", "");
            cJSON_AddStringToObject(pj, "style", e->style_json ? e->style_json : "{}");
            /* The concrete transport point for this (type, parameter) is decided
             * by the device REGISTRATION, not the protocol-agnostic catalog. Resolve
             * it so the rules editor can target the real device point_id
             * (e.g. ("light","power") -> "sim.color_bulb"), which is
             * what the rules engine matches against. Absent when no device of this
             * type is registered yet. Parameter key stays on this catalog row. */
            char pid[MINIHUB_POINT_ID_MAX];
            if (minihub_point_resolve_by_type_param(d->device, e->parameter, pid, sizeof(pid)) == ESP_OK) {
                cJSON_AddStringToObject(pj, "point_id", pid);
            }
            cJSON_AddItemToArray(params, pj);
        }
        cJSON_AddItemToObject(dj, "params", params);
        cJSON_AddItemToArray(devs, dj);
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

/* POST /api/devices/control
 * Body: { "device": <type>, "parameter": <id>, "value": <json>,
 *         "point_id": <opt>, "seq": <u32 opt> }
 * `device` is the data-model TYPE (e.g. "light"). Optional `point_id` selects
 * a concrete instance when several points share that type (Devices UI sends
 * live.point_id). If present and the point exists with matching type, use it;
 * else fall back to minihub_point_resolve_by_type_param (first match). The
 * type catalog stays protocol-agnostic; transport prefix comes from registration.
 * Parameter name is passed in cmd.params so adapters update
 * props.params.<parameter> on that device point.
 *
 * Control bodies are tiny ({device, parameter, value, point_id?, seq}); the
 * read is capped so a bogus Content-Length cannot exhaust the HTTP task stack. */
#define MINIHUB_WEB_CONTROL_BODY_MAX 1024
static esp_err_t handle_api_devices_control(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    /* Drain the body instead of taking a single httpd_req_recv(): one call only
     * returns the bytes that already arrived, so a chunked/slow client used to
     * yield a truncated body and a confusing "invalid json" 400. */
    char buf[MINIHUB_WEB_CONTROL_BODY_MAX];
    size_t want = (req->content_len > 0) ? (size_t)req->content_len : 0;
    size_t total = 0;
    if (want > sizeof(buf) - 1) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    }
    for (;;) {
        size_t room = want ? (want - total) : (sizeof(buf) - 1 - total);
        if (room == 0) {
            break;
        }
        int received = httpd_req_recv(req, buf + total, room);
        if (received < 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        }
        if (received == 0) {
            break;   /* end of body (unknown length / chunked) */
        }
        total += (size_t)received;
    }
    if (total == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }

    const cJSON *device = cJSON_GetObjectItem(root, "device");
    const cJSON *parameter = cJSON_GetObjectItem(root, "parameter");
    const cJSON *value = cJSON_GetObjectItem(root, "value");
    const cJSON *seqj = cJSON_GetObjectItem(root, "seq");
    const cJSON *point_id_j = cJSON_GetObjectItem(root, "point_id");
    if (!cJSON_IsString(device) || !cJSON_IsString(parameter) || !value ||
        !parameter->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "missing device/parameter/value");
    }
    /* Copy before cJSON_Delete(root): the response still names this parameter. */
    char param_name[64];
    snprintf(param_name, sizeof(param_name), "%s", parameter->valuestring);
    if (!minihub_device_widget_find(device->valuestring, param_name)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown device/parameter");
    }
    /* Prefer explicit point_id (multi-instance same type); else first-match
     * resolve. Transport prefix comes from registration, not the type catalog. */
    char path[MINIHUB_POINT_ID_MAX];
    path[0] = '\0';
    if (cJSON_IsString(point_id_j) && point_id_j->valuestring &&
        point_id_j->valuestring[0]) {
        const minihub_point_t *pt = minihub_point_get(point_id_j->valuestring);
        if (pt && pt->type[0] && strcmp(pt->type, device->valuestring) == 0) {
            strncpy(path, point_id_j->valuestring, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    }
    if (path[0] == '\0') {
        esp_err_t rerr = minihub_point_resolve_by_type_param(device->valuestring,
                                                             parameter->valuestring,
                                                             path, sizeof(path));
        if (rerr != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "device not registered");
        }
    }
    uint32_t seq = cJSON_IsNumber(seqj) ? (uint32_t)seqj->valueint : 0;

    /* Serialise the value THROUGH cJSON instead of pasting the raw string: a
     * bare string used to be emitted without quotes at all, and any '"' or '\'
     * inside it silently produced an invalid report payload. */
    char value_json[256];
    cJSON *vitem = cJSON_IsString(value) ? cJSON_CreateString(value->valuestring)
                                         : cJSON_Duplicate(value, true);
    if (!vitem) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    char *vs = cJSON_PrintUnformatted(vitem);
    cJSON_Delete(vitem);
    if (!vs || strlen(vs) >= sizeof(value_json)) {
        free(vs);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "value too long");
    }
    snprintf(value_json, sizeof(value_json), "%s", vs);
    free(vs);
    char payload[384];
    if (!minihub_device_widget_report_payload(seq, device->valuestring,
                                               param_name, value_json,
                                               payload, sizeof(payload))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "payload too long");
    }

    minihub_command_t cmd = {0};
    strncpy(cmd.point_id, path, sizeof(cmd.point_id) - 1);
    strncpy(cmd.cmd, cJSON_IsBool(value) ? "toggle" : "set", sizeof(cmd.cmd) - 1);
    cJSON *params = cJSON_CreateObject();
    if (!params) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddNumberToObject(params, "seq", (double)seq);
    cJSON_AddStringToObject(params, "parameter", param_name);
    cJSON_AddItemToObject(params, "value", cJSON_Duplicate(value, true));
    cmd.params = params;
    err = minihub_command_invoke(&cmd);
    if (params) {
        cJSON_Delete(params);
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req,
                                   err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND
                                                            : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "path", path);
    cJSON_AddStringToObject(resp, "parameter", param_name);
    cJSON_AddStringToObject(resp, "payload", payload);
    err = send_json(req, resp);
    cJSON_Delete(resp);
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
        return httpd_resp_send_err(req,
                                   err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND
                                                            : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

/* POST /api/devices/ota — end-device OTA (not Hub firmware /api/ota).
 * Body: { "point_id": "...", "path"|"url"|"file_path"?: "..." }
 * Routes to the point's adapter via minihub_command_invoke(cmd="ota").
 * Plugins publish availability on props.ota.available. */
static esp_err_t handle_api_devices_ota(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[512];
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
    if (!cJSON_IsString(point_id) || !point_id->valuestring || !point_id->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing point_id");
    }

    const minihub_point_t *pt = minihub_point_get(point_id->valuestring);
    if (!pt) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "point not found");
    }

    minihub_command_t command = {0};
    strncpy(command.point_id, point_id->valuestring, sizeof(command.point_id) - 1);
    strncpy(command.cmd, "ota", sizeof(command.cmd) - 1);

    cJSON *params = cJSON_CreateObject();
    if (!params) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    const char *path_keys[] = { "path", "file_path", "url", NULL };
    for (int i = 0; path_keys[i]; i++) {
        const cJSON *pj = cJSON_GetObjectItem(root, path_keys[i]);
        if (cJSON_IsString(pj) && pj->valuestring && pj->valuestring[0]) {
            cJSON_AddStringToObject(params, path_keys[i], pj->valuestring);
            break;
        }
    }
    /* Prefer plugin-published local file, then image URL, from props.ota. */
    if (!params->child && pt->props) {
        const cJSON *ota = cJSON_GetObjectItem(pt->props, "ota");
        if (cJSON_IsObject(ota)) {
            const cJSON *file = cJSON_GetObjectItem(ota, "file");
            if (cJSON_IsString(file) && file->valuestring && file->valuestring[0]) {
                cJSON_AddStringToObject(params, "path", file->valuestring);
            } else {
                const cJSON *url = cJSON_GetObjectItem(ota, "url");
                if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
                    cJSON_AddStringToObject(params, "url", url->valuestring);
                }
            }
            const cJSON *ver = cJSON_GetObjectItem(ota, "version");
            if (cJSON_IsString(ver) && ver->valuestring) {
                cJSON_AddStringToObject(params, "version", ver->valuestring);
            }
        }
    }
    command.params = params;

    err = minihub_command_invoke(&command);
    cJSON_Delete(params);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        httpd_err_code_t code = HTTPD_500_INTERNAL_SERVER_ERROR;
        if (err == ESP_ERR_NOT_FOUND) {
            code = HTTPD_404_NOT_FOUND;
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            code = HTTPD_400_BAD_REQUEST;
        }
        return httpd_resp_send_err(req, code, esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
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
        return httpd_resp_send_err(req,
                                   err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND
                                                            : HTTPD_500_INTERNAL_SERVER_ERROR,
                                   esp_err_to_name(err));
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
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
            minihub_ota_upload_abort(minihub_ota_get_error());
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, minihub_ota_get_error());
        }
        remaining -= (size_t)n;
    }

    err = minihub_ota_upload_finish();
    if (err != ESP_OK) {
        minihub_ota_upload_abort(minihub_ota_get_error());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, minihub_ota_get_error());
    }

    /* Device will reboot shortly; still return JSON if possible. */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#if CONFIG_MINIHUB_ZIGBEE_SDK
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

    esp_err_t zb_err = minihub_zigbee_permit_join(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", zb_err == ESP_OK ? "ok" : "fail");
    if (zb_err != ESP_OK) {
        minihub_zigbee_json_add_permit_join_error(resp, zb_err);
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
#endif /* CONFIG_MINIHUB_ZIGBEE_SDK */

#if CONFIG_MINIHUB_BLE_MESH_SDK
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

    esp_err_t bm_err = minihub_ble_mesh_permit_provision(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "result", bm_err == ESP_OK ? "ok" : "fail");
    if (bm_err != ESP_OK) {
#if CONFIG_MINIHUB_ZIGBEE_SDK
        const bool zigbee_joining = minihub_zigbee_permit_join_active();
#else
        const bool zigbee_joining = false;
#endif
        const char *why = minihub_ble_mesh_permit_text(
            minihub_ble_mesh_permit_classify(bm_err, zigbee_joining), http_want_zh(req));
        cJSON_AddStringToObject(resp, "error", why ? why : esp_err_to_name(bm_err));
    }
    cJSON_AddBoolToObject(resp, "permit_provision", minihub_ble_mesh_permit_provision_active());
    cJSON_AddBoolToObject(resp, "ready", minihub_ble_mesh_running());
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}
#endif /* CONFIG_MINIHUB_BLE_MESH_SDK */

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
#if CONFIG_MINIHUB_ZIGBEE_SDK
    cJSON_AddBoolToObject(resp, "zigbee_available", true);
#else
    cJSON_AddBoolToObject(resp, "zigbee_available", false);
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    cJSON_AddBoolToObject(resp, "ble_mesh_available", true);
    cJSON_AddBoolToObject(resp, "ble_mesh_running", minihub_ble_mesh_running());
#else
    cJSON_AddBoolToObject(resp, "ble_mesh_available", false);
    cJSON_AddBoolToObject(resp, "ble_mesh_running", false);
#endif
#if CONFIG_MINIHUB_ZIGBEE_SDK
    const bool zb = minihub_zigbee_permit_join_active();
#else
    const bool zb = false;
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    const bool bm_permit = minihub_ble_mesh_permit_provision_active();
    const bool bm_busy = minihub_ble_mesh_provisioning_busy();
#else
    const bool bm_permit = false;
    const bool bm_busy = false;
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
    cJSON_AddBoolToObject(resp, "matter_available", true);
    cJSON_AddBoolToObject(resp, "matter_ready", minihub_matter_ready());
#else
    cJSON_AddBoolToObject(resp, "matter_available", false);
    cJSON_AddBoolToObject(resp, "matter_ready", false);
#endif
    cJSON_AddBoolToObject(resp, "zigbee_permit_join", zb);
    cJSON_AddBoolToObject(resp, "ble_mesh_permit_provision", bm_permit);
    cJSON_AddBoolToObject(resp, "ble_mesh_provisioning", bm_busy);
    cJSON_AddBoolToObject(resp, "busy", zb || bm_busy);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t send_points_remove_response(httpd_req_t *req, cJSON *resp)
{
    const cJSON *ok = cJSON_GetObjectItem(resp, "ok");
    if (cJSON_IsFalse(ok)) {
        const cJSON *err_j = cJSON_GetObjectItem(resp, "error");
        const char *code = cJSON_IsString(err_j) ? err_j->valuestring : "fail";
        cJSON_Delete(resp);
        if (strcmp(code, "NOT_FOUND") == 0) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, code);
        }
        if (strcmp(code, "INVALID_ARG") == 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, code);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, code);
    }
    esp_err_t err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_points_remove_json(httpd_req_t *req, cJSON *body)
{
    cJSON *resp = NULL;
    esp_err_t err = minihub_hub_points_remove(body, &resp);
    cJSON_Delete(body);
    if (err == ESP_ERR_NO_MEM || !resp) {
        cJSON_Delete(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    return send_points_remove_response(req, resp);
}

static esp_err_t handle_api_points_remove(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[384];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    return handle_points_remove_json(req, root);
}

static esp_err_t handle_api_points_rename(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char *buf = NULL;
    size_t buflen = 0;
    esp_err_t body_err = read_http_body(req, &buf, &buflen, 512);
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

    const cJSON *id_j = cJSON_GetObjectItem(root, "point_id");
    const cJSON *name_j = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(id_j) || !id_j->valuestring || id_j->valuestring[0] == '\0' ||
        strlen(id_j->valuestring) >= MINIHUB_POINT_ID_MAX ||
        !cJSON_IsString(name_j) || !name_j->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "point_id and name required");
    }

    char point_id[MINIHUB_POINT_ID_MAX];
    char name[49];
    strncpy(point_id, id_j->valuestring, sizeof(point_id) - 1);
    point_id[sizeof(point_id) - 1] = '\0';
    strncpy(name, name_j->valuestring, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    cJSON_Delete(root);

    minihub_point_t snap = {0};
    err = minihub_point_get_snapshot(point_id, &snap);
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    err = minihub_device_name_set(point_id, name);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(patch, "name", name);
    err = minihub_point_merge_props(point_id, NULL, patch);
    cJSON_Delete(patch);

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

#if CONFIG_MINIHUB_ZIGBEE_SDK || CONFIG_MINIHUB_BLE_MESH_SDK
static esp_err_t handle_legacy_adapter_devices_delete(httpd_req_t *req, const char *adapter)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    char buf[384];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    }
    if (!cJSON_GetObjectItem(root, "adapter")) {
        cJSON_AddStringToObject(root, "adapter", adapter);
    }
    return handle_points_remove_json(req, root);
}
#endif

#if CONFIG_MINIHUB_ZIGBEE_SDK
static esp_err_t handle_api_zigbee_devices(httpd_req_t *req)
{
    return handle_legacy_adapter_devices_delete(req, "zigbee");
}
#endif

#if CONFIG_MINIHUB_BLE_MESH_SDK
static esp_err_t handle_api_ble_mesh_devices(httpd_req_t *req)
{
    return handle_legacy_adapter_devices_delete(req, "ble_mesh");
}
#endif

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
    const cJSON *clear_guard = cJSON_GetObjectItem(root, "clear_load_guard");
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
    } else if (cJSON_IsTrue(clear_guard)) {
        err = minihub_module_clear_load_guard(id_copy);
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing enabled/delete/clear_load_guard");
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        (void)minihub_hub_reload_async();
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
        (void)minihub_hub_reload_async();
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
        (void)minihub_hub_reload_async();
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
        (void)minihub_hub_reload_async();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

static esp_err_t handle_api_store_reinstall(httpd_req_t *req)
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

    err = minihub_store_reinstall(kind->valuestring, id->valuestring);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        (void)minihub_hub_reload_async();
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
        (void)minihub_hub_reload_async();
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
        } else if (strcmp(disc->valuestring, "ble_thread") == 0 ||
                   strcmp(disc->valuestring, "ble-thread") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_THREAD;
        } else if (strcmp(disc->valuestring, "ble_ethernet") == 0 ||
                   strcmp(disc->valuestring, "ble-ethernet") == 0 ||
                   strcmp(disc->valuestring, "ethernet") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET;
        }
    }

    const char *ssid_s = cJSON_IsString(ssid) ? ssid->valuestring : NULL;
    const char *pass_s = cJSON_IsString(wpass) ? wpass->valuestring : NULL;

    /* Cancel an in-flight commission. The backend already implements
     * minihub_matter_commission_cancel(); expose it so the web UI can show a
     * "取消配网" button that matches BLE Mesh's "停止配网" / Zigbee's "停止入网". */
    const cJSON *cancel = cJSON_GetObjectItem(root, "cancel");
    if (cJSON_IsBool(cancel) && cJSON_IsTrue(cancel)) {
        esp_err_t cerr = minihub_matter_commission_cancel(node_id);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", cerr == ESP_OK ? "cancelled" : esp_err_to_name(cerr));
        if (cerr != ESP_OK && minihub_matter_last_error()) {
            cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
        }
        cJSON_Delete(root);
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }

    if (cJSON_IsString(code)) {
        err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery, ssid_s, pass_s);
    } else if (cJSON_IsNumber(pin)) {
        if (discovery == MINIHUB_MATTER_DISCOVERY_BLE_WIFI ||
            discovery == MINIHUB_MATTER_DISCOVERY_BLE_THREAD ||
            discovery == MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "BLE discovery requires pairing_code (not setup_pin)");
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
    *out_count = 1;
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
        if (!cJSON_GetObjectItem(root, "adapter")) {
            cJSON_AddStringToObject(root, "adapter", "matter");
        }
        return handle_points_remove_json(req, root);
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
        } else if (strcmp(disc->valuestring, "ble_thread") == 0 ||
                   strcmp(disc->valuestring, "ble-thread") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_THREAD;
        } else if (strcmp(disc->valuestring, "ble_ethernet") == 0 ||
                   strcmp(disc->valuestring, "ble-ethernet") == 0 ||
                   strcmp(disc->valuestring, "ethernet") == 0) {
            discovery = MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET;
        }
    }
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

    const bool want_commission = cJSON_IsString(code) && code->valuestring[0];
    if (want_commission) {
        /* Refuse before writing anything: commission_code_ex only reports
         * ESP_ERR_INVALID_STATE afterwards, so upserting first would leave a
         * permanent commission_phase:"failed" row — a card for a device that was
         * never attempted (typical when the Thread BR is off and the commissioner
         * cannot init). */
        /* Refuse before writing anything: commission_code_ex only reports failure
         * afterwards, so upserting first would leave a permanent
         * commission_phase:"failed" row — a card for a device that was never
         * attempted. The preflight mirrors the kickoff's own conditions (readiness
         * alone is not enough: the commissioner stays up while the Thread BR is off). */
        esp_err_t preflight = minihub_matter_commission_preflight(discovery, ssid_s);
        if (preflight != ESP_OK) {
            const char *why = minihub_matter_last_error();
            cJSON_Delete(root);
            cJSON *busy = cJSON_CreateObject();
            if (!busy) {
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            }
            cJSON_AddStringToObject(busy, "result", "error");
            cJSON_AddNumberToObject(busy, "node_id", (double)node_id);
            cJSON_AddStringToObject(busy, "commission", esp_err_to_name(preflight));
            cJSON_AddStringToObject(busy, "message", why ? why : esp_err_to_name(preflight));
            cJSON_AddBoolToObject(busy, "matter_ready", minihub_matter_ready());
            err = send_json(req, busy);
            cJSON_Delete(busy);
            return err;
        }
        /* Eager card: upsert as waiting, start commission, return immediately.
         * Never block httpd on commission_wait — that stalls /api/points. */
        size_t synced = 0;
        esp_err_t commission_err = minihub_matter_devices_upsert(node_id, endpoints, ep_count, merge_existing,
                                                                 &synced, transport);
        if (commission_err == ESP_OK) {
            commission_err = minihub_matter_devices_set_commission_phase(node_id, "waiting", NULL);
        }
        if (commission_err == ESP_OK && do_reload) {
            /* Republish in place. A full hub reload unregisters every adapter's
             * points and they only come back when start_all() finishes ~1.7 s
             * later, which reads as the card vanishing mid-commissioning. */
            (void)minihub_hub_resync_adapter_async("matter");
        }
        if (commission_err == ESP_OK) {
            commission_err = minihub_matter_commission_code_ex(node_id, code->valuestring, discovery,
                                                               ssid_s, pass_s);
        }
        if (commission_err == ESP_OK) {
            commission_err = minihub_matter_set_pending_device(node_id, endpoints, ep_count, merge_existing,
                                                              do_reload, transport);
        }
        if (commission_err != ESP_OK) {
            (void)minihub_matter_devices_set_commission_phase(node_id, "failed",
                                                              minihub_matter_last_error()
                                                                  ? minihub_matter_last_error()
                                                                  : esp_err_to_name(commission_err));
            if (do_reload) {
                (void)minihub_hub_resync_adapter_async("matter");
            }
        }
        cJSON_Delete(root);
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        if (commission_err != ESP_OK) {
            cJSON_AddStringToObject(resp, "result", "error");
            cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
            cJSON_AddNumberToObject(resp, "synced", (double)synced);
            cJSON_AddStringToObject(resp, "commission", esp_err_to_name(commission_err));
            if (minihub_matter_last_error()) {
                cJSON_AddStringToObject(resp, "message", minihub_matter_last_error());
            } else {
                cJSON_AddStringToObject(resp, "message", esp_err_to_name(commission_err));
            }
        } else {
            cJSON_AddStringToObject(resp, "result", "ok");
            cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
            cJSON_AddStringToObject(resp, "commission", "started");
            cJSON_AddBoolToObject(resp, "reloaded", do_reload);
            cJSON_AddNumberToObject(resp, "synced", (double)synced);
        }
        err = send_json(req, resp);
        cJSON_Delete(resp);
        return err;
    }

    /* No pairing_code: upsert only (no commission). */
    size_t synced = 0;
    err = minihub_matter_devices_upsert(node_id, endpoints, ep_count, merge_existing, &synced, transport);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    if (do_reload) {
        (void)minihub_hub_resync_adapter_async("matter");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddNumberToObject(resp, "node_id", (double)node_id);
    cJSON_AddNumberToObject(resp, "synced", (double)synced);
    cJSON_AddBoolToObject(resp, "reloaded", do_reload);
    cJSON_AddStringToObject(resp, "commission", "skipped");
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */

#if CONFIG_MINIHUB_THREAD_ADAPTER

static esp_err_t handle_api_thread_status(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *status = minihub_thread_status_json();
    if (!status) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no status");
    }
    err = send_json(req, status);
    cJSON_Delete(status);
    return err;
}

static esp_err_t handle_api_thread_start(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_thread_start();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK && minihub_thread_last_error() && minihub_thread_last_error()[0]) {
        cJSON_AddStringToObject(resp, "message", minihub_thread_last_error());
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t handle_api_thread_stop(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_thread_stop();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "ok" : esp_err_to_name(err));
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t handle_api_thread_dataset_get(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *tlvs = NULL;
    size_t len = 0;
    err = minihub_thread_dataset_get(&tlvs, &len);
    cJSON *resp = cJSON_CreateObject();
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "status", esp_err_to_name(err));
        if (minihub_thread_last_error() && minihub_thread_last_error()[0]) {
            cJSON_AddStringToObject(resp, "message", minihub_thread_last_error());
        }
    } else {
        char *hex = calloc(1, len * 2 + 1);
        if (!hex) {
            free(tlvs);
            cJSON_Delete(resp);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        static const char *h = "0123456789abcdef";
        for (size_t i = 0; i < len; i++) {
            hex[i * 2] = h[(tlvs[i] >> 4) & 0xf];
            hex[i * 2 + 1] = h[tlvs[i] & 0xf];
        }
        cJSON_AddStringToObject(resp, "dataset", hex);
        cJSON_AddNumberToObject(resp, "length", (double)len);
        free(hex);
        free(tlvs);
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

static esp_err_t handle_api_thread_dataset_post(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    char buf[1024];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
    }
    buf[received >= 0 ? received : 0] = '\0';

    uint8_t raw[256];
    size_t raw_len = 0;
    if (received > 0) {
        cJSON *root = cJSON_Parse(buf);
        if (!root) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        }
        const cJSON *ds = cJSON_GetObjectItem(root, "dataset");
        if (cJSON_IsString(ds) && ds->valuestring && ds->valuestring[0]) {
            size_t n = strlen(ds->valuestring);
            if (n % 2 != 0 || n / 2 > sizeof(raw)) {
                cJSON_Delete(root);
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad dataset hex");
            }
            for (size_t i = 0; i < n / 2; i++) {
                unsigned int v = 0;
                if (sscanf(ds->valuestring + i * 2, "%2x", &v) != 1) {
                    cJSON_Delete(root);
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad dataset hex");
                }
                raw[i] = (uint8_t)v;
            }
            raw_len = n / 2;
        }
        cJSON_Delete(root);
    }

    err = minihub_thread_dataset_set(raw_len ? raw : NULL, raw_len);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", err == ESP_OK ? "ok" : esp_err_to_name(err));
    if (err != ESP_OK && minihub_thread_last_error() && minihub_thread_last_error()[0]) {
        cJSON_AddStringToObject(resp, "message", minihub_thread_last_error());
    }
    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}
#endif /* CONFIG_MINIHUB_THREAD_ADAPTER */

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
        err = minihub_hub_reload_async();
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
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

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
    err = read_http_body(req, &body, &body_len, DEV_FS_UPLOAD_MAX_BYTES);
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

#if CONFIG_MINIHUB_WEB_UI
/* Host console HTML/CSS/JS under /data/web/ — curl without reflashing storage. */
#define DEV_WEB_UPLOAD_MAX_BYTES (512 * 1024)

static esp_err_t mkdir_web_parents(const char *full_path)
{
    char dir[MINIHUB_PATH_MAX];
    size_t n = strnlen(full_path, sizeof(dir) - 1);
    if (n == 0 || n >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dir, full_path, n);
    dir[n] = '\0';
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return ESP_OK;
    }
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                *p = '/';
                return ESP_FAIL;
            }
        }
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t handle_api_dev_web_put(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }

    static const char prefix[] = "/api/dev/web/";
    if (strncmp(req->uri, prefix, sizeof(prefix) - 1) != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const char *rel = req->uri + sizeof(prefix) - 1;
    if (!web_path_safe(rel)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    }

    char path[MINIHUB_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", MINIHUB_WEB_DIR, rel) >= (int)sizeof(path)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "path too long");
    }

    char *body = NULL;
    size_t body_len = 0;
    err = read_http_body(req, &body, &body_len, DEV_WEB_UPLOAD_MAX_BYTES);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    err = mkdir_web_parents(path);
    if (err != ESP_OK) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    if (body_len > 0 && fwrite(body, 1, body_len, f) != body_len) {
        fclose(f);
        free(body);
        unlink(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    fclose(f);
    free(body);
    ESP_LOGI(TAG, "dev web put %s (%u bytes)", path, (unsigned)body_len);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "path", path);
    cJSON_AddNumberToObject(resp, "bytes", (double)body_len);
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}

/* GET/PUT /api/dev/catalog/{devices.csv|parameters.csv}
 * Model-shaped CSVs → /data/config/model/, flatten to /data/config/devices.csv,
 * minihub_device_widget_reload(). Same api_auth gate as other REST. */
#define DEV_CATALOG_UPLOAD_MAX_BYTES (128 * 1024)

static bool catalog_name_from_uri(const char *uri, char *name_out, size_t name_len)
{
    static const char prefix[] = "/api/dev/catalog/";
    if (!uri || strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *rel = uri + sizeof(prefix) - 1;
    if (strcmp(rel, "devices.csv") != 0 && strcmp(rel, "parameters.csv") != 0) {
        return false;
    }
    if (strlen(rel) + 1 > name_len) {
        return false;
    }
    memcpy(name_out, rel, strlen(rel) + 1);
    return true;
}

static esp_err_t handle_api_dev_catalog_get(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    char name[32];
    if (!catalog_name_from_uri(req->uri, name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use devices.csv or parameters.csv");
    }
    char *body = NULL;
    size_t body_len = 0;
    err = minihub_device_widget_get_model(name, &body, &body_len);
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not uploaded yet");
    }
    if (err != ESP_OK || !body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
    }
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, body, body_len);
    free(body);
    return err;
}

static esp_err_t handle_api_dev_catalog_put(httpd_req_t *req)
{
    esp_err_t err = api_auth(req);
    if (err != ESP_OK) {
        return err;
    }
    char name[32];
    if (!catalog_name_from_uri(req->uri, name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use devices.csv or parameters.csv");
    }

    char *body = NULL;
    size_t body_len = 0;
    err = read_http_body(req, &body, &body_len, DEV_CATALOG_UPLOAD_MAX_BYTES);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    }

    bool rebuilt = false;
    char errbuf[192];
    errbuf[0] = '\0';
    err = minihub_device_widget_put_model(name, body, body_len, &rebuilt, errbuf, sizeof(errbuf));
    free(body);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   errbuf[0] ? errbuf : "invalid catalog csv");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   errbuf[0] ? errbuf : "catalog update failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "ok");
    cJSON_AddStringToObject(resp, "file", name);
    cJSON_AddBoolToObject(resp, "rebuilt", rebuilt);
    cJSON_AddNumberToObject(resp, "bytes", (double)body_len);
    if (!rebuilt && errbuf[0]) {
        cJSON_AddStringToObject(resp, "hint", errbuf);
    } else if (rebuilt) {
        cJSON_AddStringToObject(resp, "runtime", MINIHUB_DW_CSV_PATH);
        cJSON_AddNumberToObject(resp, "devices",
                                (double)minihub_device_widget_device_count());
    }
    err = send_json(req, resp);
    cJSON_Delete(resp);
    return err;
}
#endif /* CONFIG_MINIHUB_WEB_UI */

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
    s_index_uri = index_uri;
    s_index_override = (local.index_path && local.index_path[0]) ? local.index_path : NULL;
    s_server = server;

    httpd_uri_t root = { .uri = index_uri, .method = HTTP_GET, .handler = handle_root };
#if CONFIG_MINIHUB_WEB_UI
    httpd_uri_t login_page = { .uri = "/login", .method = HTTP_GET, .handler = handle_login_page };
    httpd_uri_t web_assets = { .uri = "/web/*", .method = HTTP_GET, .handler = handle_web_asset };
    httpd_uri_t device_images = { .uri = MINIHUB_IMAGES_URI_PREFIX "*", .method = HTTP_GET,
                                  .handler = handle_device_image };
#endif
#if CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB
    httpd_uri_t module_web = { .uri = "/modules/*", .method = HTTP_GET,
                               .handler = handle_module_web_asset };
#endif
    httpd_uri_t favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon };
    httpd_uri_t api = { .uri = "/api/", .method = HTTP_GET, .handler = handle_api_info };
    httpd_uri_t api_auth_status = { .uri = "/api/auth/status", .method = HTTP_GET,
                                    .handler = handle_api_auth_status };
    httpd_uri_t api_auth_setup = { .uri = "/api/auth/setup", .method = HTTP_POST,
                                   .handler = handle_api_auth_setup };
    httpd_uri_t api_auth_login = { .uri = "/api/auth/login", .method = HTTP_POST,
                                   .handler = handle_api_auth_login };
    httpd_uri_t api_auth_logout = { .uri = "/api/auth/logout", .method = HTTP_POST,
                                    .handler = handle_api_auth_logout };
    httpd_uri_t api_auth_change = { .uri = "/api/auth/change", .method = HTTP_POST,
                                    .handler = handle_api_auth_change };
    httpd_uri_t api_system = { .uri = "/api/system", .method = HTTP_GET, .handler = handle_api_system };
    httpd_uri_t api_system_reboot = { .uri = "/api/system/reboot", .method = HTTP_POST,
                                      .handler = handle_api_system_reboot };
    httpd_uri_t api_system_reset = { .uri = "/api/system/reset", .method = HTTP_POST,
                                     .handler = handle_api_system_reset };
    httpd_uri_t api_net_cfg = { .uri = "/api/net/config", .method = HTTP_POST, .handler = handle_api_net_config };
    httpd_uri_t api_net_wifi = { .uri = "/api/net/wifi", .method = HTTP_POST, .handler = handle_api_net_wifi };
    httpd_uri_t adapters_api = { .uri = "/api/adapters", .method = HTTP_GET, .handler = handle_api_adapters };
    httpd_uri_t config_adapters_get = { .uri = "/api/config/adapters", .method = HTTP_GET,
                                        .handler = handle_api_config_adapters_get };
    httpd_uri_t config_adapters_put = { .uri = "/api/config/adapters", .method = HTTP_PUT,
                                        .handler = handle_api_config_adapters_put };
    httpd_uri_t location_api = { .uri = "/api/location", .method = HTTP_GET, .handler = handle_api_location };
    httpd_uri_t points = { .uri = "/api/points", .method = HTTP_GET, .handler = handle_api_points };
    httpd_uri_t points_history = { .uri = "/api/points/history", .method = HTTP_GET,
                                   .handler = handle_api_points_history };
    httpd_uri_t points_remove = { .uri = "/api/points/remove", .method = HTTP_POST,
                                  .handler = handle_api_points_remove };
    httpd_uri_t points_rename = { .uri = "/api/points/rename", .method = HTTP_POST,
                                  .handler = handle_api_points_rename };
    httpd_uri_t command = { .uri = "/api/command", .method = HTTP_POST, .handler = handle_api_command };
    httpd_uri_t device_widgets = { .uri = "/api/device_widgets", .method = HTTP_GET, .handler = handle_api_device_widgets };
    httpd_uri_t devices_control = { .uri = "/api/devices/control", .method = HTTP_POST, .handler = handle_api_devices_control };
    httpd_uri_t devices_ota = { .uri = "/api/devices/ota", .method = HTTP_POST, .handler = handle_api_devices_ota };
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
#if CONFIG_MINIHUB_ZIGBEE_SDK
    httpd_uri_t zb_permit = { .uri = "/api/zigbee/permit_join", .method = HTTP_POST, .handler = handle_api_zigbee_permit };
    httpd_uri_t zb_install_code = { .uri = "/api/zigbee/install_code", .method = HTTP_POST,
                                    .handler = handle_api_zigbee_install_code };
    httpd_uri_t zb_devices = { .uri = "/api/zigbee/devices", .method = HTTP_POST, .handler = handle_api_zigbee_devices };
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    httpd_uri_t ble_mesh_permit = { .uri = "/api/ble_mesh/permit_provision", .method = HTTP_POST,
                                    .handler = handle_api_ble_mesh_permit };
    httpd_uri_t ble_mesh_devices = { .uri = "/api/ble_mesh/devices", .method = HTTP_POST,
                                     .handler = handle_api_ble_mesh_devices };
#endif
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
#if CONFIG_MINIHUB_THREAD_ADAPTER
    httpd_uri_t thread_status = { .uri = "/api/thread/status", .method = HTTP_GET,
                                  .handler = handle_api_thread_status };
    httpd_uri_t thread_start = { .uri = "/api/thread/start", .method = HTTP_POST,
                                 .handler = handle_api_thread_start };
    httpd_uri_t thread_stop = { .uri = "/api/thread/stop", .method = HTTP_POST,
                                .handler = handle_api_thread_stop };
    httpd_uri_t thread_dataset_get = { .uri = "/api/thread/dataset", .method = HTTP_GET,
                                       .handler = handle_api_thread_dataset_get };
    httpd_uri_t thread_dataset_post = { .uri = "/api/thread/dataset", .method = HTTP_POST,
                                        .handler = handle_api_thread_dataset_post };
#endif

    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &root), TAG, "index %s", index_uri);
#if CONFIG_MINIHUB_WEB_UI
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &login_page), TAG, "/login");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &web_assets), TAG, "/web/*");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &device_images), TAG, MINIHUB_IMAGES_URI_PREFIX "*");
#endif
#if CONFIG_MINIHUB_MODULE_THIRD_PARTY_UI_WEB
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_web), TAG, "/modules/*");
#endif
    if (local.register_favicon) {
        ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &favicon), TAG, "/favicon.ico");
    }
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api), TAG, "/api/");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_auth_status), TAG, "/api/auth/status");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_auth_setup), TAG, "/api/auth/setup");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_auth_login), TAG, "/api/auth/login");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_auth_logout), TAG, "/api/auth/logout");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_auth_change), TAG, "/api/auth/change");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_system), TAG, "/api/system");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_system_reboot), TAG, "/api/system/reboot");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_system_reset), TAG, "/api/system/reset");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_net_cfg), TAG, "/api/net/config");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &api_net_wifi), TAG, "/api/net/wifi");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &adapters_api), TAG, "/api/adapters");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &config_adapters_get), TAG, "/api/config/adapters GET");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &config_adapters_put), TAG, "/api/config/adapters PUT");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &location_api), TAG, "/api/location");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points), TAG, "/api/points");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points_history), TAG, "/api/points/history");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points_remove), TAG, "/api/points/remove");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &points_rename), TAG, "/api/points/rename");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &command), TAG, "/api/command");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &device_widgets), TAG, "/api/device_widgets");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &devices_control), TAG, "/api/devices/control");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &devices_ota), TAG, "/api/devices/ota");
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
#if CONFIG_MINIHUB_ZIGBEE_SDK
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_permit), TAG, "/api/zigbee/permit_join");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_install_code), TAG, "/api/zigbee/install_code");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &zb_devices), TAG, "/api/zigbee/devices");
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ble_mesh_permit), TAG, "/api/ble_mesh/permit_provision");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &ble_mesh_devices), TAG, "/api/ble_mesh/devices");
#endif
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &radio_prov), TAG, "/api/radio/provision");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &reload), TAG, "/api/hub/reload");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_rpc), TAG, "/api/modules/*");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &module_preview), TAG, "/api/modules/*/preview");
#if CONFIG_MINIHUB_MATTER_ADAPTER
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_status), TAG, "/api/matter/status");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_commission), TAG, "/api/matter/commission");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &matter_devices), TAG, "/api/matter/devices");
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &thread_status), TAG, "/api/thread/status");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &thread_start), TAG, "/api/thread/start");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &thread_stop), TAG, "/api/thread/stop");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &thread_dataset_get), TAG, "/api/thread/dataset GET");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &thread_dataset_post), TAG, "/api/thread/dataset POST");
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
    httpd_uri_t store_reinstall = { .uri = "/api/store/reinstall", .method = HTTP_POST,
                                    .handler = handle_api_store_reinstall };
    httpd_uri_t store_uninstall = { .uri = "/api/store/uninstall", .method = HTTP_POST,
                                    .handler = handle_api_store_uninstall };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_catalog), TAG, "/api/store/catalog");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_refresh), TAG, "/api/store/refresh");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_install), TAG, "/api/store/install");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_upgrade), TAG, "/api/store/upgrade");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &store_reinstall), TAG, "/api/store/reinstall");
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

#if CONFIG_MINIHUB_WEB_UI
    httpd_uri_t dev_web_put = {
        .uri = "/api/dev/web/*",
        .method = HTTP_PUT,
        .handler = handle_api_dev_web_put,
    };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &dev_web_put), TAG, "/api/dev/web/*");
    httpd_uri_t dev_catalog_get = {
        .uri = "/api/dev/catalog/*",
        .method = HTTP_GET,
        .handler = handle_api_dev_catalog_get,
    };
    httpd_uri_t dev_catalog_put = {
        .uri = "/api/dev/catalog/*",
        .method = HTTP_PUT,
        .handler = handle_api_dev_catalog_put,
    };
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &dev_catalog_get), TAG, "/api/dev/catalog GET");
    ESP_RETURN_ON_ERROR(minihub_web_add_uri(server, &dev_catalog_put), TAG, "/api/dev/catalog PUT");
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
        if (stat(MINIHUB_WEB_INDEX, &st) == 0 && S_ISREG(st.st_mode)) {
            index_state = "storage";
        } else if (stat(MINIHUB_PREINSTALL_WEB_INDEX, &st) == 0 && S_ISREG(st.st_mode)) {
            index_state = "legacy";
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
    config.max_uri_handlers = 68;
    config.max_open_sockets = 20;
    /* Store install/upgrade may HTTP-download .mhub on this task (esp_http_client
     * + local buffers). 8K overflows; 16K matches typical IDF httpd+client use. */
    config.stack_size = 16384;
    config.lru_purge_enable = true;
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
