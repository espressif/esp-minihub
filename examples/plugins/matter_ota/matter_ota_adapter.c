/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Matter OTA / cloud catalog demo (L3).
 *
 * One Matter endpoint = one Host point (light/matter_n*_ep*). This plugin never
 * point_register()s a second id. It lists adapter "matter" points, matches
 * props.manufacturer against local catalog.json, then fetches catalog_url for
 * offered version + firmware_url. Optionally streams a
 * firmware .bin to /data/matter_ota, and merge_props props.ota on the same
 * point_id. A manufacturer match sets props.owner to this adapter so the overview
 * moves the card under the vendor section. point.adapter stays "matter" so
 * radio commands still route to the Host. OTA Requestor/BDX remains a stub.
 */
#ifdef MINIHUB_PLUGIN_STANDALONE
#include "minihub_plugin_sdk.h"
#else
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "minihub_core.h"
#include "minihub_module.h"
#include "minihub_http_worker.h"
#include "minihub_net.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "matter_ota";

#define ZO_ADAPTER      "matter_ota"
#define ZO_ZB_ADAPTER   "matter"
#define ZO_CACHE_DIR    "/data/matter_ota"

extern const minihub_module_host_api_t *g_matter_ota_host;
extern cJSON *g_matter_ota_config;

static cJSON *s_local;   /* packed catalog.json: catalog_url + manufacturer */
static cJSON *s_catalog; /* cloud JSON: devices[].version / firmware_url */
static volatile bool s_need_scan;
static int s_poll_ticks;
static bool s_subscribed;

#define HOST_HAS(h, field) \
    ((h) && (h)->struct_size >= offsetof(minihub_module_host_api_t, field) + sizeof((h)->field))

static void ieee_norm(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!in || !out || out_len == 0) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    size_t i = 0;
    if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X')) {
        i = 2;
    }
    for (; in[i] && j + 1 < out_len; i++) {
        char c = in[i];
        if (c == ':' || c == '-' || c == ' ') {
            continue;
        }
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        return it->valuestring;
    }
    return NULL;
}

static bool ensure_cache_dir(void)
{
    struct stat st;
    if (stat(ZO_CACHE_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    if (mkdir(ZO_CACHE_DIR, 0755) == 0) {
        return true;
    }
    ESP_LOGW(TAG, "mkdir %s failed", ZO_CACHE_DIR);
    return false;
}

static void file_stem(const char *ieee_n, const char *point_id, char *out, size_t n)
{
    if (ieee_n && ieee_n[0]) {
        snprintf(out, n, "%s", ieee_n);
        return;
    }
    size_t j = 0;
    for (size_t i = 0; point_id && point_id[i] && j + 1 < n; i++) {
        char c = point_id[i];
        out[j++] = (c == '.' || c == '/') ? '_' : c;
    }
    out[j] = '\0';
}

static bool ota_same_version(const minihub_point_t *pt, const char *version)
{
    if (!pt || !pt->props || !version) {
        return false;
    }
    const cJSON *ota = cJSON_GetObjectItem(pt->props, "ota");
    if (!cJSON_IsObject(ota)) {
        return false;
    }
    const cJSON *ver = cJSON_GetObjectItem(ota, "version");
    const cJSON *avail = cJSON_GetObjectItem(ota, "available");
    return cJSON_IsTrue(avail) && cJSON_IsString(ver) && ver->valuestring &&
           strcmp(ver->valuestring, version) == 0;
}

static esp_err_t merge_ota(const char *point_id, const char *version, const char *url,
                           const char *file_path, size_t size)
{
    const minihub_module_host_api_t *h = g_matter_ota_host;
    if (!h || !point_id) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *patch = cJSON_CreateObject();
    cJSON *ota = cJSON_CreateObject();
    if (!patch || !ota) {
        cJSON_Delete(patch);
        cJSON_Delete(ota);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(ota, "available", true);
    if (version) {
        cJSON_AddStringToObject(ota, "version", version);
    }
    if (url && url[0]) {
        cJSON_AddStringToObject(ota, "url", url);
    }
    if (file_path && file_path[0]) {
        cJSON_AddStringToObject(ota, "file", file_path);
    }
    if (size > 0) {
        cJSON_AddNumberToObject(ota, "size", (double)size);
    }
    cJSON_AddItemToObject(patch, "ota", ota);

    esp_err_t err;
    if (HOST_HAS(h, point_merge_props) && h->point_merge_props) {
        err = h->point_merge_props(point_id, NULL, patch);
    } else {
        err = minihub_point_merge_props(point_id, NULL, patch);
    }
    cJSON_Delete(patch);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "merged props.ota on %s version=%s file=%s",
                 point_id, version ? version : "", file_path ? file_path : "");
    }
    return err;
}

typedef struct {
    const char *url;
    char *path;
    size_t size;
} zo_dl_t;

static esp_err_t http_stream_to_file(void *ctx)
{
    zo_dl_t *job = (zo_dl_t *)ctx;
    if (!job || !job->url || !job->path) {
        return ESP_ERR_INVALID_ARG;
    }

    unlink(job->path);

    FILE *fp = fopen(job->path, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "fopen %s failed", job->path);
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {
        .url = job->url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        fclose(fp);
        unlink(job->path);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http open %s: %s", job->url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        fclose(fp);
        unlink(job->path);
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "http status %d for %s", status, job->url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        fclose(fp);
        unlink(job->path);
        return ESP_FAIL;
    }

    int64_t clen = esp_http_client_get_content_length(client);
    char buf[512];
    int n;
    size_t total = 0;
    while ((n = esp_http_client_read(client, buf, (int)sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)n;
    }
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || n < 0 || (clen > 0 && (int64_t)total != clen)) {
        unlink(job->path);
        return ESP_FAIL;
    }
    job->size = total;
    return ESP_OK;
}

static esp_err_t download_firmware(const char *url, const char *path, size_t *out_size)
{
    zo_dl_t job = { .url = url, .path = (char *)path, .size = 0 };
    esp_err_t err;
    if (minihub_http_is_worker_task()) {
        err = http_stream_to_file(&job);
    } else {
        err = minihub_http_run_worker(http_stream_to_file, &job);
    }
    if (err == ESP_OK && out_size) {
        *out_size = job.size;
    }
    return err;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} zo_body_t;

static esp_err_t http_event_collect(esp_http_client_event_t *evt)
{
    if (!evt || evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) {
        return ESP_OK;
    }
    zo_body_t *b = (zo_body_t *)evt->user_data;
    size_t add = (size_t)evt->data_len;
    if (!evt->data || add == 0) {
        return ESP_OK;
    }
    if (b->len + add + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 1024;
        while (ncap < b->len + add + 1) {
            ncap *= 2;
        }
        if (ncap > 64 * 1024) {
            return ESP_ERR_NO_MEM;
        }
        char *nb = realloc(b->buf, ncap);
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, evt->data, add);
    b->len += add;
    b->buf[b->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_get_text(void *ctx)
{
    zo_body_t *job = (zo_body_t *)ctx;
    const char *url = (const char *)job->buf;
    /* job->buf initially holds the URL string we must not free until copy */
    char url_copy[256];
    strncpy(url_copy, url, sizeof(url_copy) - 1);
    url_copy[sizeof(url_copy) - 1] = '\0';
    job->buf = NULL;
    job->len = 0;
    job->cap = 0;

    zo_body_t body = {0};
    esp_http_client_config_t cfg = {
        .url = url_copy,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event_collect,
        .user_data = &body,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200 || !body.buf) {
        free(body.buf);
        return ESP_FAIL;
    }
    job->buf = body.buf;
    job->len = body.len;
    job->cap = body.cap;
    return ESP_OK;
}

static void catalog_replace(cJSON *root)
{
    if (s_catalog) {
        cJSON_Delete(s_catalog);
    }
    s_catalog = root;
}

static void local_replace(cJSON *root)
{
    if (s_local) {
        cJSON_Delete(s_local);
    }
    s_local = root;
}

static const char *resolve_catalog_url(void)
{
    const char *u = json_str(g_matter_ota_config, "catalog_url");
    if (u) {
        return u;
    }
    return json_str(s_local, "catalog_url");
}

static bool manufacturer_ok(const minihub_point_t *pt)
{
    const char *want = json_str(s_local, "manufacturer");
    if (!want) {
        return false;
    }
    const char *have = json_str(pt ? pt->props : NULL, "manufacturer");
    return have && strcasecmp(have, want) == 0;
}

/* Claim does not change point.adapter (command routing). Overview groups by owner. */
static void set_owner(const minihub_point_t *pt, bool claim)
{
    if (!pt || !pt->point_id[0]) {
        return;
    }
    const char *cur = json_str(pt->props, "owner");
    if (claim) {
        if (cur && strcmp(cur, ZO_ADAPTER) == 0) {
            return;
        }
    } else if (!cur || strcmp(cur, ZO_ADAPTER) != 0) {
        return;
    }

    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return;
    }
    cJSON_AddStringToObject(patch, "owner", claim ? ZO_ADAPTER : "");
    const minihub_module_host_api_t *h = g_matter_ota_host;
    esp_err_t err;
    if (HOST_HAS(h, point_merge_props) && h->point_merge_props) {
        err = h->point_merge_props(pt->point_id, NULL, patch);
    } else {
        err = minihub_point_merge_props(pt->point_id, NULL, patch);
    }
    cJSON_Delete(patch);
    if (err != ESP_OK) {
        return;
    }
    if (claim) {
        ESP_LOGI(TAG, "claimed %s under %s", pt->point_id, ZO_ADAPTER);
    } else {
        ESP_LOGI(TAG, "released %s", pt->point_id);
    }
}

static void load_bundled_catalog(void)
{
    if (s_local) {
        return;
    }
    const minihub_module_host_api_t *h = g_matter_ota_host;
    char *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (HOST_HAS(h, module_read_file) && h->module_read_file) {
        err = h->module_read_file(ZO_ADAPTER, "catalog.json", &raw, &raw_len);
    }
    if (err != ESP_OK) {
        err = minihub_module_read_file(ZO_ADAPTER, "catalog.json", &raw, &raw_len);
    }
    if (err != ESP_OK || !raw) {
        ESP_LOGW(TAG, "bundled catalog.json not found (%s)", esp_err_to_name(err));
        return;
    }
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        ESP_LOGW(TAG, "catalog.json parse failed");
        return;
    }
    local_replace(root);
    ESP_LOGI(TAG, "loaded local catalog.json (url=%s manufacturer=%s)",
             json_str(s_local, "catalog_url") ? json_str(s_local, "catalog_url") : "",
             json_str(s_local, "manufacturer") ? json_str(s_local, "manufacturer") : "");
}

static void load_catalog_url(const char *url)
{
    if (!url || !url[0]) {
        return;
    }
    zo_body_t job = { .buf = (char *)url };
    esp_err_t err;
    if (minihub_http_is_worker_task()) {
        err = http_get_text(&job);
    } else {
        err = minihub_http_run_worker(http_get_text, &job);
    }
    if (err != ESP_OK || !job.buf) {
        ESP_LOGW(TAG, "catalog_url fetch failed: %s", url);
        return;
    }
    cJSON *root = cJSON_Parse(job.buf);
    free(job.buf);
    if (!root) {
        ESP_LOGW(TAG, "catalog_url JSON parse failed");
        return;
    }
    catalog_replace(root);
    ESP_LOGI(TAG, "loaded catalog_url");
}

static const cJSON *match_catalog(const minihub_point_t *pt)
{
    if (!s_catalog || !pt) {
        return NULL;
    }
    const cJSON *devices = cJSON_GetObjectItem(s_catalog, "devices");
    if (!cJSON_IsArray(devices)) {
        return NULL;
    }

    char ieee_n[24] = {0};
    const char *ieee = json_str(pt->props, "node_id");
    if (!ieee) {
        ieee = json_str(pt->props, "ieee");
    }
    ieee_norm(ieee, ieee_n, sizeof(ieee_n));
    const char *model = json_str(pt->props, "model");
    const char *manuf = json_str(pt->props, "manufacturer");

    const cJSON *ent = NULL;
    cJSON_ArrayForEach(ent, devices) {
        if (!cJSON_IsObject(ent)) {
            continue;
        }
        const char *want_manuf = json_str(ent, "manufacturer");
        if (want_manuf) {
            if (!manuf || strcasecmp(manuf, want_manuf) != 0) {
                continue;
            }
        }
        const char *want_ieee = json_str(ent, "node_id");
        if (!want_ieee) {
            want_ieee = json_str(ent, "ieee");
        }
        if (want_ieee && strcmp(want_ieee, "*") != 0) {
            char want_n[24];
            ieee_norm(want_ieee, want_n, sizeof(want_n));
            if (!ieee_n[0] || strcmp(ieee_n, want_n) != 0) {
                continue;
            }
        }
        const char *want_model = json_str(ent, "model");
        if (want_model && (!model || strcmp(model, want_model) != 0)) {
            continue;
        }
        return ent;
    }
    return NULL;
}

static void apply_entry(const minihub_point_t *pt, const cJSON *ent)
{
    const char *version = json_str(ent, "version");
    const char *fw_url = json_str(ent, "firmware_url");
    if (!version) {
        return;
    }
    if (ota_same_version(pt, version)) {
        return;
    }

    char ieee_n[24] = {0};
    const char *id = json_str(pt->props, "node_id");
    if (!id) {
        id = json_str(pt->props, "ieee");
    }
    ieee_norm(id, ieee_n, sizeof(ieee_n));

    char stem[40];
    file_stem(ieee_n, pt->point_id, stem, sizeof(stem));
    char path[160] = {0};
    size_t size = 0;

    if (fw_url && fw_url[0] && ensure_cache_dir()) {
        snprintf(path, sizeof(path), "%s/%s.bin", ZO_CACHE_DIR, stem);
        struct stat st;
        bool have = (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
        if (have) {
            size = (size_t)st.st_size;
        } else if (minihub_net_has_ip()) {
            if (download_firmware(fw_url, path, &size) != ESP_OK) {
                path[0] = '\0';
                size = 0;
            }
        } else {
            path[0] = '\0';
        }
    }

    (void)merge_ota(pt->point_id, version, fw_url, path[0] ? path : NULL, size);
}

static void zo_scan(void)
{
    load_bundled_catalog();

    minihub_point_t *list = NULL;
    size_t count = 0;
    if (minihub_point_list_by_adapter(ZO_ZB_ADAPTER, &list, &count) != ESP_OK || !list) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].adapter, ZO_ZB_ADAPTER) != 0) {
            continue;
        }
        set_owner(&list[i], manufacturer_ok(&list[i]));
    }
    minihub_point_list_free(list, count);

    if (!s_catalog) {
        const char *curl = resolve_catalog_url();
        if (curl && minihub_net_has_ip()) {
            load_catalog_url(curl);
        }
    }
    if (!s_catalog) {
        return;
    }

    list = NULL;
    count = 0;
    if (minihub_point_list_by_adapter(ZO_ZB_ADAPTER, &list, &count) != ESP_OK || !list) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].adapter, ZO_ZB_ADAPTER) != 0) {
            continue;
        }
        if (!manufacturer_ok(&list[i])) {
            continue;
        }
        const cJSON *ent = match_catalog(&list[i]);
        if (ent) {
            apply_entry(&list[i], ent);
        }
    }
    minihub_point_list_free(list, count);
}

static void zo_release_all(void)
{
    minihub_point_t *list = NULL;
    size_t count = 0;
    if (minihub_point_list_by_adapter(ZO_ZB_ADAPTER, &list, &count) != ESP_OK || !list) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        set_owner(&list[i], false);
    }
    minihub_point_list_free(list, count);
}

static void on_point_event(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) {
        return;
    }
    s_need_scan = true;
}

static void subscribe_events(void)
{
    const minihub_module_host_api_t *h = g_matter_ota_host;
    if (s_subscribed) {
        return;
    }
    if (HOST_HAS(h, event_subscribe) && h->event_subscribe) {
        (void)h->event_subscribe("point.registered", on_point_event, NULL);
        (void)h->event_subscribe("point.updated", on_point_event, NULL);
    } else {
        (void)minihub_event_bus_subscribe("point.registered", on_point_event, NULL);
        (void)minihub_event_bus_subscribe("point.updated", on_point_event, NULL);
    }
    s_subscribed = true;
}

static esp_err_t zo_rpc(const char *method, const cJSON *params, cJSON **out);

static esp_err_t zo_init(const cJSON *config)
{
    (void)config;
    return ESP_OK;
}

static esp_err_t zo_start(void)
{
    (void)minihub_module_register_rpc(ZO_ADAPTER, zo_rpc);
    subscribe_events();
    s_need_scan = true;
    zo_scan();
    return ESP_OK;
}

static esp_err_t zo_stop(void)
{
    zo_release_all();
    return ESP_OK;
}

static esp_err_t zo_deinit(void)
{
    catalog_replace(NULL);
    local_replace(NULL);
    s_subscribed = false;
    return ESP_OK;
}

static esp_err_t zo_handle_command(const minihub_command_t *cmd)
{
    if (!cmd || cmd->cmd[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(cmd->cmd, "refresh") == 0) {
        catalog_replace(NULL);
        s_need_scan = true;
        zo_scan();
        return ESP_OK;
    }
    if (strcmp(cmd->cmd, "download") == 0) {
        s_need_scan = true;
        zo_scan();
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t zo_poll(void)
{
    s_poll_ticks++;
    if (s_need_scan || (s_poll_ticks % 30) == 0) {
        s_need_scan = false;
        zo_scan();
    }
    return ESP_OK;
}

static esp_err_t zo_rpc(const char *method, const cJSON *params, cJSON **out)
{
    (void)params;
    if (!method) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(method, "refresh") == 0 || strcmp(method, "download") == 0) {
        catalog_replace(NULL);
        zo_scan();
        if (out) {
            *out = cJSON_CreateObject();
            if (*out) {
                cJSON_AddStringToObject(*out, "result", "ok");
            }
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const minihub_adapter_t matter_ota_adapter = {
    .name = ZO_ADAPTER,
    .version = "0.1.0",
    .init = zo_init,
    .start = zo_start,
    .stop = zo_stop,
    .deinit = zo_deinit,
    .handle_command = zo_handle_command,
    .poll = zo_poll,
};
