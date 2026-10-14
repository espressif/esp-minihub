/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "minihub_paths.h"
#include "minihub_pack.h"
#include "minihub_net.h"
#include "minihub_module.h"
#include "minihub_module_abi.h"
#include "minihub_module_abi_check.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "hub_zip.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <stdbool.h>

static const char *TAG = "minihub_store";

/* Current running firmware version (Kconfig override, else app descriptor). */
static const char *store_running_version(void)
{
#ifdef CONFIG_MINIHUB_VERSION
    if (CONFIG_MINIHUB_VERSION[0]) {
        return CONFIG_MINIHUB_VERSION;
    }
#endif
    const esp_app_desc_t *app = esp_app_get_description();
    if (app && app->version[0]) {
        return app->version;
    }
    return NULL;
}

#define STORE_NVS_NS "mh_store"

static void cfg_load(char *custom_url, size_t custom_url_len, bool *disable_default)
{
    if (custom_url && custom_url_len) {
        custom_url[0] = '\0';
    }
    if (disable_default) {
        *disable_default = false;
    }
    nvs_handle_t h;
    if (nvs_open(STORE_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    if (custom_url && custom_url_len) {
        size_t len = custom_url_len;
        if (nvs_get_str(h, "custom_url", custom_url, &len) != ESP_OK) {
            custom_url[0] = '\0';
        }
    }
    uint8_t dis = 0;
    if (nvs_get_u8(h, "dis_def", &dis) == ESP_OK && disable_default) {
        *disable_default = (dis != 0);
    }
    nvs_close(h);
}

static esp_err_t cfg_save(const char *custom_url, bool disable_default)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STORE_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (custom_url && custom_url[0]) {
        err = nvs_set_str(h, "custom_url", custom_url);
    } else {
        err = nvs_erase_key(h, "custom_url");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "dis_def", disable_default ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool looks_like_http_url(const char *s)
{
    return s && (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static void normalize_base_url(char *url)
{
    if (!url) {
        return;
    }
    size_t n = strlen(url);
    while (n > 0 && (url[n - 1] == '/' || url[n - 1] == ' ')) {
        url[--n] = '\0';
    }
}

static void store_default_base_url(char *out, size_t out_len)
{
    if (!out || !out_len) {
        return;
    }
#if CONFIG_MINIHUB_STORE
    snprintf(out, out_len, "http://%s:%d",
             CONFIG_MINIHUB_STORE_SERVER,
             CONFIG_MINIHUB_STORE_PORT);
#else
    out[0] = '\0';
#endif
}

esp_err_t minihub_store_get_config(char *custom_url, size_t custom_url_len, bool *disable_default,
                                   char *default_url, size_t default_url_len)
{
    cfg_load(custom_url, custom_url_len, disable_default);
    if (default_url && default_url_len) {
        store_default_base_url(default_url, default_url_len);
    }
    return ESP_OK;
}

esp_err_t minihub_store_set_config(const char *custom_url, bool disable_default)
{
    char buf[MINIHUB_STORE_BASE_URL_MAX];
    buf[0] = '\0';
    if (custom_url && custom_url[0]) {
        while (*custom_url == ' ') {
            custom_url++;
        }
        if (!looks_like_http_url(custom_url)) {
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(buf, custom_url, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        normalize_base_url(buf);
        if (strlen(buf) < 10) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (disable_default && buf[0] == '\0') {
        return ESP_ERR_INVALID_ARG; /* need custom when default disabled */
    }
    esp_err_t err = cfg_save(buf[0] ? buf : NULL, disable_default);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved custom_url='%s' disable_default=%d",
                 buf[0] ? buf : "", (int)disable_default);
    }
    return err;
}

#if !CONFIG_MINIHUB_STORE
cJSON *minihub_store_fetch_catalog(void)
{
    return cJSON_CreateArray();
}

esp_err_t minihub_store_install(const char *kind, const char *id)
{
    (void)kind;
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_store_uninstall(const char *kind, const char *id)
{
    (void)kind;
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_store_upgrade(const char *kind, const char *id)
{
    (void)kind;
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_store_reinstall(const char *kind, const char *id)
{
    (void)kind;
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

cJSON *minihub_store_catalog_merged(void)
{
    return cJSON_CreateArray();
}

esp_err_t minihub_store_refresh_catalog(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_store_refresh_catalog_async(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void minihub_store_refresh_catalog_status(bool *busy, bool *finished, esp_err_t *result)
{
    if (busy) {
        *busy = false;
    }
    if (finished) {
        *finished = false;
    }
    if (result) {
        *result = ESP_ERR_NOT_SUPPORTED;
    }
}

bool minihub_store_copy_base_url(char *out, size_t out_len)
{
    if (out && out_len) {
        out[0] = '\0';
    }
    return false;
}

esp_err_t minihub_store_list_upgrades(cJSON **out)
{
    if (out) {
        *out = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#else

#define MINIHUB_STORE_CATALOG_PATH MINIHUB_STORE_MOUNT "/catalog.json"
#define MINIHUB_STORE_FILES_DIR    MINIHUB_STORE_MOUNT "/files"

static esp_err_t http_download_file(const char *url, const char *path);
static esp_err_t http_download_to_psram(const char *url, uint8_t **out, size_t *out_len, size_t max_len);
static esp_err_t http_get_body(const char *url, char **out_body, size_t *out_len);
static void append_chip_query(char *url, size_t len);
static void store_base_url(char *out, size_t out_len);
static esp_err_t store_abs_url(char *out, size_t out_len, const char *path);
static esp_err_t write_file_atomic(const char *path, const char *data);
static cJSON *http_get_catalog_array(const char *base);
static void catalog_rewrite_relative_urls(cJSON *arr, const char *base);
static cJSON *catalog_merge_prefer_override(cJSON *base_arr, cJSON *override_arr);
static cJSON *fetch_merged_remote_catalog(void);

static cJSON *load_bundled_catalog(void)
{
    FILE *f = fopen(MINIHUB_STORE_CATALOG_PATH, "r");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len <= 0 || len > 65536) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    ESP_LOGI(TAG, "using bundled catalog (%d items)", cJSON_GetArraySize(root));
    return root;
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    return mkdir(path, 0755) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t copy_file_path(const char *src, const char *dst)
{
    if (!src || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *in = fopen(src, "rb");
    if (!in) {
        ESP_LOGE(TAG, "copy open src failed %s: errno=%d", src, errno);
        return ESP_ERR_NOT_FOUND;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        ESP_LOGE(TAG, "copy open dst failed %s: errno=%d", dst, errno);
        fclose(in);
        return ESP_FAIL;
    }
    char chunk[1024];
    esp_err_t err = ESP_OK;
    while (true) {
        size_t n = fread(chunk, 1, sizeof(chunk), in);
        if (n == 0) {
            break;
        }
        if (fwrite(chunk, 1, n, out) != n) {
            ESP_LOGE(TAG, "copy write failed %s: errno=%d", dst, errno);
            err = ESP_FAIL;
            break;
        }
    }
    fclose(in);
    fclose(out);
    if (err != ESP_OK) {
        unlink(dst);
    }
    return err;
}

static esp_err_t copy_store_file_url(const char *url_path, const char *dest)
{
    if (!url_path || !dest || strncmp(url_path, "/files/", 7) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    char src[MINIHUB_PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", MINIHUB_STORE_FILES_DIR, url_path + 7);
    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "install from bundled store file %s", src);
    return copy_file_path(src, dest);
}

static esp_err_t download_or_copy_store_file(const char *url, const char *dest)
{
    if (!url || !dest) {
        return ESP_ERR_INVALID_ARG;
    }
    if (url[0] == '/') {
        esp_err_t err = copy_store_file_url(url, dest);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        /* Bundled miss: fetch from configured store server. */
        char full[MINIHUB_STORE_ABS_URL_MAX];
        esp_err_t jerr = store_abs_url(full, sizeof(full), url);
        if (jerr != ESP_OK) {
            return jerr;
        }
        ESP_LOGI(TAG, "bundled miss, downloading %s", full);
        return http_download_file(full, dest);
    }
    return http_download_file(url, dest);
}

/** Prefer custom URL when set; else firmware default (unless disabled with no custom — empty). */
static void store_base_url(char *out, size_t out_len)
{
    char custom[MINIHUB_STORE_BASE_URL_MAX];
    bool dis = false;
    cfg_load(custom, sizeof(custom), &dis);
    if (custom[0]) {
        snprintf(out, out_len, "%s", custom);
        return;
    }
    if (dis) {
        out[0] = '\0';
        return;
    }
    store_default_base_url(out, out_len);
}

static bool chip_token_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    size_t i = 0;
    for (; a[i] && b[i]; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return false;
        }
    }
    return a[i] == '\0' && b[i] == '\0';
}

static bool catalog_entry_targets_chip(const cJSON *entry, const char *chip)
{
    const cJSON *targets = cJSON_GetObjectItem(entry, "targets");
    if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) == 0) {
        return true; /* legacy entry without targets: passthrough */
    }
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, targets) {
        if (!cJSON_IsString(t) || !t->valuestring) {
            continue;
        }
        if (strcmp(t->valuestring, "*") == 0 || chip_token_eq(t->valuestring, "ALL") ||
            chip_token_eq(t->valuestring, chip)) {
            return true;
        }
    }
    return false;
}

/* Drop catalog entries built for another chip. The store server filters on
 * ?chip= as well; this is defense in depth, like the per-plugin ABI block. */
static void catalog_filter_by_chip(cJSON *arr)
{
    if (!cJSON_IsArray(arr)) {
        return;
    }
    const char *chip = CONFIG_IDF_TARGET;
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (item && !catalog_entry_targets_chip(item, chip)) {
            cJSON *id = cJSON_GetObjectItem(item, "id");
            ESP_LOGI(TAG, "catalog: skip %s (targets exclude %s)",
                     (id && cJSON_IsString(id)) ? id->valuestring : "?", chip);
            cJSON_DeleteItemFromArray(arr, i);
        }
    }
}

static cJSON *http_get_catalog_array(const char *base)
{
    if (!base || !base[0]) {
        return NULL;
    }
    char url[MINIHUB_STORE_ABS_URL_MAX];
    snprintf(url, sizeof(url), "%s/api/catalog", base);
    char with_mac[MINIHUB_STORE_ABS_URL_MAX + 64];
    const char *get_url = url;
    if (minihub_net_copy_url_with_mac(url, with_mac, sizeof(with_mac)) == ESP_OK) {
        get_url = with_mac;
    }
    /* Identify the device to the store server.
       - chip: lets the server drop catalog entries aimed at another chip
         (defense in depth; the device also filters locally per
         CONFIG_IDF_TARGET).
       - cur_version: lets the server flag a newer published firmware build
         (firmware_upgrade.newer).
       L3/native ABI (abi_version / abi_total_hash) is unsupported and is
       never sent. */
    char *full = NULL;
    int n = asprintf(&full, "%s%schip=%s",
                      get_url, strchr(get_url, '?') ? "&" : "?", CONFIG_IDF_TARGET);
    if (n < 0 || !full) {
        return NULL;
    }
    const char *ver = store_running_version();
    if (ver && ver[0]) {
        char *prev = full;
        n = asprintf(&full, "%s%scur_version=%s",
                      prev, strchr(prev, '?') ? "&" : "?", ver);
        free(prev);
        if (n < 0 || !full) {
            return NULL;
        }
    }
    get_url = full;
    char *body = NULL;
    esp_err_t err = http_get_body(get_url, &body, NULL);
    free(full);
    if (err != ESP_OK || !body) {
        ESP_LOGW(TAG, "catalog fetch failed from %s: %s", url, esp_err_to_name(err));
        free(body);
        return NULL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "invalid catalog JSON from %s", url);
        return NULL;
    }
    /* The store server replies with either a bare plugin array (legacy) or an
     * object {"status":"ok","plugins":[...]} (current, carrying ABI filtering).
     * Hand the caller a plain array either way so the rest of the code is
     * unchanged. */
    cJSON *arr;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else {
        cJSON *pl = cJSON_GetObjectItem(root, "plugins");
        if (!pl || !cJSON_IsArray(pl)) {
            ESP_LOGW(TAG, "invalid catalog JSON from %s", url);
            cJSON_Delete(root);
            return NULL;
        }
        arr = cJSON_Duplicate(pl, 1);
        cJSON_Delete(root);
        if (!arr) {
            return NULL;
        }
    }
    catalog_filter_by_chip(arr);
    return arr;
}

static void catalog_rewrite_relative_urls(cJSON *arr, const char *base)
{
    if (!arr || !base || !base[0]) {
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *url = cJSON_GetObjectItem(item, "url");
        if (!cJSON_IsString(url) || !url->valuestring) {
            continue;
        }
        const char *u = url->valuestring;
        if (looks_like_http_url(u)) {
            continue;
        }
        char abs[MINIHUB_STORE_ABS_URL_MAX];
        if (u[0] == '/') {
            snprintf(abs, sizeof(abs), "%s%s", base, u);
        } else {
            snprintf(abs, sizeof(abs), "%s/%s", base, u);
        }
        cJSON_SetValuestring(url, abs);
    }
}

static cJSON *catalog_merge_prefer_override(cJSON *base_arr, cJSON *override_arr)
{
    cJSON *out = cJSON_CreateArray();
    cJSON *by_id = cJSON_CreateObject();
    if (!out || !by_id) {
        cJSON_Delete(out);
        cJSON_Delete(by_id);
        return NULL;
    }

    const cJSON *item = NULL;
    if (base_arr) {
        cJSON_ArrayForEach(item, base_arr) {
            const cJSON *id = cJSON_GetObjectItem(item, "id");
            if (!cJSON_IsString(id) || !id->valuestring) {
                continue;
            }
            cJSON *copy = cJSON_Duplicate(item, true);
            if (copy) {
                cJSON_AddItemToObject(by_id, id->valuestring, copy);
            }
        }
    }
    if (override_arr) {
        cJSON_ArrayForEach(item, override_arr) {
            const cJSON *id = cJSON_GetObjectItem(item, "id");
            if (!cJSON_IsString(id) || !id->valuestring) {
                continue;
            }
            cJSON *copy = cJSON_Duplicate(item, true);
            if (!copy) {
                continue;
            }
            cJSON_DeleteItemFromObject(by_id, id->valuestring);
            cJSON_AddItemToObject(by_id, id->valuestring, copy);
        }
    }

    cJSON_ArrayForEach(item, by_id) {
        cJSON *copy = cJSON_Duplicate(item, true);
        if (copy) {
            cJSON_AddItemToArray(out, copy);
        }
    }
    cJSON_Delete(by_id);
    return out;
}

/** Fetch default (+ optional custom) catalogs and merge (custom overrides same id). */
static cJSON *fetch_merged_remote_catalog(void)
{
    char custom[MINIHUB_STORE_BASE_URL_MAX];
    bool dis = false;
    cfg_load(custom, sizeof(custom), &dis);

    cJSON *def_arr = NULL;
    cJSON *cust_arr = NULL;
    if (!dis) {
        char def_base[MINIHUB_STORE_BASE_URL_MAX];
        store_default_base_url(def_base, sizeof(def_base));
        def_arr = http_get_catalog_array(def_base);
        catalog_rewrite_relative_urls(def_arr, def_base);
    }
    if (custom[0]) {
        cust_arr = http_get_catalog_array(custom);
        catalog_rewrite_relative_urls(cust_arr, custom);
    }

    if (!def_arr && !cust_arr) {
        return NULL;
    }
    cJSON *merged = catalog_merge_prefer_override(def_arr, cust_arr);
    cJSON_Delete(def_arr);
    cJSON_Delete(cust_arr);
    return merged;
}

/** Join store base URL with an absolute path (e.g. "/files/x.mhub"). */
static esp_err_t store_abs_url(char *out, size_t out_len, const char *path)
{
    if (!out || out_len == 0 || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (looks_like_http_url(path)) {
        size_t pl = strnlen(path, out_len);
        if (pl >= out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, path, pl);
        out[pl] = '\0';
        return ESP_OK;
    }
    char base[MINIHUB_STORE_BASE_URL_MAX];
    store_base_url(base, sizeof(base));
    if (!base[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bl = strnlen(base, sizeof(base));
    size_t pl = strnlen(path, MINIHUB_STORE_URL_MAX);
    if (pl >= MINIHUB_STORE_URL_MAX || bl + pl + 1 > out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, base, bl);
    memcpy(out + bl, path, pl);
    out[bl + pl] = '\0';
    return ESP_OK;
}

static esp_err_t http_get_body(const char *url, char **out_body, size_t *out_len)
{
    if (!url || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
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
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s HTTP %d", url, status);
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
        if (total + 512 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buf = nb;
        }
        int n = esp_http_client_read(client, buf + total, (int)(cap - total - 1));
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
    buf[total] = '\0';
    *out_body = buf;
    if (out_len) {
        *out_len = total;
    }
    return ESP_OK;
}

static void append_chip_query(char *url, size_t len)
{
    if (!url || !url[0] || len == 0) {
        return;
    }
    size_t used = strnlen(url, len);
    if (used >= len) {
        return;
    }
    const char *sep = strchr(url, '?') ? "&" : "?";
    snprintf(url + used, len - used, "%schip=%s", sep, CONFIG_IDF_TARGET);
}

static esp_err_t http_download_file(const char *url, const char *path)
{
    char with_queries[MINIHUB_STORE_ABS_URL_MAX + 128];
    if (url) {
        snprintf(with_queries, sizeof(with_queries), "%s", url);
        (void)minihub_net_copy_url_with_mac(url, with_queries, sizeof(with_queries));
        append_chip_query(with_queries, sizeof(with_queries));
        url = with_queries;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 1024,
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

    FILE *out = fopen(path, "wb");
    if (!out) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Heap chunk — avoid large stack use when called from httpd (install API). */
    char *chunk = malloc(1024);
    if (!chunk) {
        fclose(out);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        unlink(path);
        return ESP_ERR_NO_MEM;
    }
    while (true) {
        int n = esp_http_client_read(client, chunk, 1024);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        if (fwrite(chunk, 1, (size_t)n, out) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
    }
    free(chunk);
    fclose(out);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        unlink(path);
    }
    return err;
}

cJSON *minihub_store_fetch_catalog(void)
{
    /* Prefer local /store/catalog.json — never block UI/httpd on LAN store server. */
    cJSON *local = load_bundled_catalog();
    if (local) {
        return local;
    }

    cJSON *merged = fetch_merged_remote_catalog();
    if (!merged) {
        ESP_LOGW(TAG, "catalog fetch failed from configured store URL(s)");
    }
    return merged;
}

static volatile bool s_refresh_busy;
static volatile bool s_refresh_finished;
static volatile esp_err_t s_refresh_result = ESP_FAIL;

static void store_refresh_task(void *arg)
{
    (void)arg;
    s_refresh_result = minihub_store_refresh_catalog();
    s_refresh_finished = true;
    s_refresh_busy = false;
    vTaskDelete(NULL);
}

esp_err_t minihub_store_refresh_catalog_async(void)
{
    if (s_refresh_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    s_refresh_busy = true;
    if (xTaskCreate(store_refresh_task, "store_cat", 12288, NULL, 5, NULL) != pdPASS) {
        s_refresh_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void minihub_store_refresh_catalog_status(bool *busy, bool *finished, esp_err_t *result)
{
    if (busy) {
        *busy = s_refresh_busy;
    }
    if (finished) {
        *finished = s_refresh_finished;
    }
    if (result) {
        *result = s_refresh_result;
    }
}

esp_err_t minihub_store_refresh_catalog(void)
{
    cJSON *root = fetch_merged_remote_catalog();
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "catalog refresh failed from configured store URL(s)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Persist merged catalog (absolute item urls) for offline UI/install. */
    char *printed = cJSON_PrintUnformatted(root);
    int count = cJSON_GetArraySize(root);
    cJSON_Delete(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ensure_dir(MINIHUB_STORE_MOUNT);
    if (err == ESP_OK) {
        err = write_file_atomic(MINIHUB_STORE_CATALOG_PATH, printed);
    }
    free(printed);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "catalog refresh: save failed %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "catalog refreshed (%d items) -> %s", count, MINIHUB_STORE_CATALOG_PATH);
    return ESP_OK;
}

static bool catalog_item_config_only(const cJSON *item)
{
    if (!item) {
        return false;
    }
    const cJSON *co = cJSON_GetObjectItem(item, "config_only");
    if (cJSON_IsBool(co)) {
        return cJSON_IsTrue(co);
    }
    const cJSON *url = cJSON_GetObjectItem(item, "url");
    if (cJSON_IsString(url) && url->valuestring) {
        const char *u = url->valuestring;
        size_t n = strlen(u);
        if (n >= 5 && strcmp(u + n - 5, ".mhub") == 0) {
            return false;
        }
        if (n >= 5 && strcmp(u + n - 5, ".json") == 0) {
            return true;
        }
    }
    return false;
}

static const cJSON *catalog_find(const cJSON *catalog, const char *kind, const char *id)
{
    if (!catalog || !kind || !id) {
        return NULL;
    }
    const cJSON *item = NULL;
    const cJSON *fallback = NULL;
    cJSON_ArrayForEach(item, catalog) {
        const cJSON *k = cJSON_GetObjectItem(item, "type");
        const cJSON *i = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(k) || !cJSON_IsString(i) || strcmp(i->valuestring, id) != 0) {
            continue;
        }
        if (strcmp(k->valuestring, kind) == 0) {
            return item;
        }
        /* pack/module interchangeable only for JSON-only (config) modules */
        if (!fallback && catalog_item_config_only(item) &&
            ((strcmp(kind, "pack") == 0 && strcmp(k->valuestring, "module") == 0) ||
             (strcmp(kind, "module") == 0 && strcmp(k->valuestring, "pack") == 0))) {
            fallback = item;
        }
    }
    return fallback;
}

static esp_err_t write_file_atomic(const char *path, const char *data)
{
    char tmp[MINIHUB_PATH_MAX + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ESP_LOGE(TAG, "write open failed %s: errno=%d", tmp, errno);
        return ESP_FAIL;
    }
    if (fputs(data, f) == EOF) {
        fclose(f);
        unlink(tmp);
        ESP_LOGE(TAG, "write failed %s: errno=%d", tmp, errno);
        return ESP_FAIL;
    }
    fclose(f);
    unlink(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed: errno=%d", tmp, path, errno);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static int version_parse_part(const char **cursor)
{
    const char *s = *cursor;
    int value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
    }
    *cursor = s;
    return value;
}

/** @return >0 if a>b, 0 if equal, <0 if a<b (semver major.minor.patch prefix). */
static int version_compare(const char *a, const char *b)
{
    if (!a || !a[0]) {
        a = "0";
    }
    if (!b || !b[0]) {
        b = "0";
    }
    const char *pa = a;
    const char *pb = b;
    for (int i = 0; i < 3; i++) {
        int va = version_parse_part(&pa);
        int vb = version_parse_part(&pb);
        if (va != vb) {
            return va - vb;
        }
    }
    return strcmp(a, b);
}

static cJSON *load_json_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len <= 0 || len > 65536) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

#if CONFIG_MINIHUB_DYNAMIC_MODULES
static void module_pkg_dir(const char *id, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", MINIHUB_MODULES_DIR, id);
}

/** Prefer /store/modules/<id>/module.json; fall back to legacy <id>.json. */
static bool module_manifest_path(const char *id, char *out, size_t out_len)
{
    if (!id || !out || out_len == 0) {
        return false;
    }
    struct stat st;
    snprintf(out, out_len, "%s/%s/module.json", MINIHUB_MODULES_DIR, id);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        return true;
    }
    snprintf(out, out_len, "%s/%s.json", MINIHUB_MODULES_DIR, id);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

static char *installed_module_version(const char *id, char *out, size_t out_len)
{
    if (!id || !out || out_len == 0) {
        return NULL;
    }
    char path[MINIHUB_PATH_MAX];
    if (!module_manifest_path(id, path, sizeof(path))) {
        return NULL;
    }
    cJSON *root = load_json_file(path);
    if (!root) {
        return NULL;
    }
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(version)) {
        strncpy(out, version->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        strncpy(out, "0", out_len - 1);
    }
    cJSON_Delete(root);
    return out;
}

static cJSON *installed_module_manifest(const char *id)
{
    char path[MINIHUB_PATH_MAX];
    if (!module_manifest_path(id, path, sizeof(path))) {
        return NULL;
    }
    return load_json_file(path);
}

static void module_uninstall_adapter_name(const char *id, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    cJSON *manifest = installed_module_manifest(id);
    if (manifest) {
        const cJSON *adapter = cJSON_GetObjectItem(manifest, "adapter");
        if (cJSON_IsString(adapter) && adapter->valuestring[0]) {
            strncpy(out, adapter->valuestring, out_len - 1);
            out[out_len - 1] = '\0';
        }
        cJSON_Delete(manifest);
    }
    if (!out[0] && id) {
        strncpy(out, id, out_len - 1);
        out[out_len - 1] = '\0';
    }
}
#endif

static char *installed_pack_version(const char *id, char *out, size_t out_len)
{
    if (!id || !out || out_len == 0) {
        return NULL;
    }
    char path[MINIHUB_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.json", MINIHUB_PACKS_DIR, id);
    cJSON *root = load_json_file(path);
    if (!root) {
        return NULL;
    }
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    if (cJSON_IsString(version)) {
        strncpy(out, version->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
    } else {
        strncpy(out, "0", out_len - 1);
    }
    cJSON_Delete(root);
    return out;
}

static esp_err_t validate_mhub_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    minihub_module_image_header_t hdr;
    size_t n = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr) || hdr.magic != MINIHUB_MODULE_MAGIC) {
        return ESP_ERR_INVALID_CRC;
    }
    if (hdr.abi_version != MINIHUB_MODULE_ABI_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static void manifest_merge_preserve(cJSON *manifest, const cJSON *existing)
{
    if (!manifest || !existing) {
        return;
    }
    static const char *k_preserve[] = {"ui", "capabilities"};
    for (size_t i = 0; i < sizeof(k_preserve) / sizeof(k_preserve[0]); i++) {
        const char *key = k_preserve[i];
        if (cJSON_GetObjectItem(manifest, key)) {
            continue;
        }
        const cJSON *old = cJSON_GetObjectItem(existing, key);
        if (cJSON_IsObject(old) || cJSON_IsArray(old)) {
            cJSON_AddItemToObject(manifest, key, cJSON_Duplicate(old, 1));
        }
    }
    const cJSON *enabled = cJSON_GetObjectItem(existing, "enabled");
    if (cJSON_IsBool(enabled)) {
        cJSON_DeleteItemFromObject(manifest, "enabled");
        cJSON_AddBoolToObject(manifest, "enabled", cJSON_IsTrue(enabled));
    }
}

#if CONFIG_MINIHUB_DYNAMIC_MODULES
static bool module_installed(const char *id)
{
    char path[MINIHUB_PATH_MAX];
    return module_manifest_path(id, path, sizeof(path));
}
#endif

static bool pack_installed(const char *id)
{
    char path[MINIHUB_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.json", MINIHUB_PACKS_DIR, id);
    struct stat st;
    return stat(path, &st) == 0;
}

#if CONFIG_MINIHUB_DYNAMIC_MODULES

static size_t hub_max_bytes(void)
{
#if defined(CONFIG_MINIHUB_HUB_MAX_BYTES)
    return (size_t)CONFIG_MINIHUB_HUB_MAX_BYTES;
#else
    return 1048576u;
#endif
}

static esp_err_t read_file_to_psram(const char *path, uint8_t **out, size_t *out_len, size_t max_len)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)st.st_size == 0 || (size_t)st.st_size > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if ((size_t)st.st_size > largest) {
        ESP_LOGE(TAG, "PSRAM too small for %s need=%ld largest=%u",
                 path, (long)st.st_size, (unsigned)largest);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *buf = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = n;
    return ESP_OK;
}

static esp_err_t http_download_to_psram(const char *url, uint8_t **out, size_t *out_len, size_t max_len)
{
    char with_queries[MINIHUB_STORE_ABS_URL_MAX + 128];
    if (url) {
        snprintf(with_queries, sizeof(with_queries), "%s", url);
        (void)minihub_net_copy_url_with_mac(url, with_queries, sizeof(with_queries));
        append_chip_query(with_queries, sizeof(with_queries));
        url = with_queries;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 1024,
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

    int content_len = esp_http_client_get_content_length(client);
    size_t cap;
    if (content_len > 0) {
        if ((size_t)content_len > max_len) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_SIZE;
        }
        cap = (size_t)content_len;
    } else {
        cap = max_len;
    }

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (cap > largest) {
        ESP_LOGE(TAG, "PSRAM too small for download need=%u largest=%u",
                 (unsigned)cap, (unsigned)largest);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < cap) {
        int n = esp_http_client_read(client, (char *)buf + total, (int)(cap - total));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        total += (size_t)n;
        if (content_len <= 0 && total == cap) {
            /* Unknown length hit max — reject as oversize if more data follows */
            char probe[1];
            int extra = esp_http_client_read(client, probe, 1);
            if (extra > 0) {
                err = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || total == 0) {
        heap_caps_free(buf);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    if (content_len > 0 && total != (size_t)content_len) {
        heap_caps_free(buf);
        return ESP_ERR_INVALID_SIZE;
    }
    *out = buf;
    *out_len = total;
    return ESP_OK;
}

static esp_err_t fetch_hub_to_psram(const char *url, uint8_t **out, size_t *out_len, size_t max_len)
{
    if (!url || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (url[0] == '/') {
        char src[MINIHUB_PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", MINIHUB_STORE_FILES_DIR, url + 7);
        esp_err_t err = read_file_to_psram(src, out, out_len, max_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "hub from bundled %s (%u bytes)", src, (unsigned)*out_len);
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        char full[MINIHUB_STORE_ABS_URL_MAX];
        esp_err_t jerr = store_abs_url(full, sizeof(full), url);
        if (jerr != ESP_OK) {
            return jerr;
        }
        ESP_LOGI(TAG, "bundled hub miss, downloading %s", full);
        return http_download_to_psram(full, out, out_len, max_len);
    }
    return http_download_to_psram(url, out, out_len, max_len);
}

typedef enum {
    HUB_KIND_PACK = 0,
    HUB_KIND_JS,
    HUB_KIND_NATIVE,
} hub_kind_t;

static hub_kind_t classify_staging(const char *staging_dir, const char *expect_id, cJSON **out_manifest)
{
    char manifest_path[MINIHUB_MODULE_FILE_PATH_MAX];
    snprintf(manifest_path, sizeof(manifest_path), "%s/module.json", staging_dir);
    cJSON *root = load_json_file(manifest_path);
    if (!root) {
        return HUB_KIND_NATIVE; /* caller checks null */
    }
    if (out_manifest) {
        *out_manifest = root;
    }

    const cJSON *co = cJSON_GetObjectItem(root, "config_only");
    const cJSON *rt = cJSON_GetObjectItem(root, "runtime");
    if (cJSON_IsTrue(co) || (cJSON_IsString(rt) && strcmp(rt->valuestring, "pack") == 0)) {
        return HUB_KIND_PACK;
    }
    if (cJSON_IsString(rt) && strcmp(rt->valuestring, "js") == 0) {
        return HUB_KIND_JS;
    }

    char so_path[MINIHUB_MODULE_FILE_PATH_MAX];
    char js_path[MINIHUB_MODULE_FILE_PATH_MAX];
    snprintf(so_path, sizeof(so_path), "%s/%s.so", staging_dir, expect_id);
    snprintf(js_path, sizeof(js_path), "%s/main.js", staging_dir);
    struct stat st_so, st_js;
    bool has_so = (stat(so_path, &st_so) == 0 && S_ISREG(st_so.st_mode));
    bool has_js = (stat(js_path, &st_js) == 0 && S_ISREG(st_js.st_mode));
    /* runtime:hybrid → install as native path (.so); companion main.js via minihub_js */
    if (cJSON_IsString(rt) && strcmp(rt->valuestring, "hybrid") == 0) {
        return HUB_KIND_NATIVE;
    }
    if (has_so && has_js) {
        /* Ambiguous dual artifacts — validate_staging rejects without hybrid+entry */
        return HUB_KIND_NATIVE;
    }
    if (has_js) {
        return HUB_KIND_JS;
    }
    if (cJSON_IsString(rt) && strcmp(rt->valuestring, "native") == 0) {
        return HUB_KIND_NATIVE;
    }
    return HUB_KIND_NATIVE;
}

static esp_err_t validate_staging_module(const char *staging_dir, const char *expect_id,
                                         hub_kind_t *out_kind)
{
    cJSON *root = NULL;
    hub_kind_t kind = classify_staging(staging_dir, expect_id, &root);
    if (!root) {
        ESP_LOGE(TAG, "staging missing/invalid module.json");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *id_j = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(id_j) || strcmp(id_j->valuestring, expect_id) != 0) {
        ESP_LOGE(TAG, "staging id mismatch");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char so_path[MINIHUB_MODULE_FILE_PATH_MAX];
    char js_path[MINIHUB_MODULE_FILE_PATH_MAX];
    snprintf(so_path, sizeof(so_path), "%s/%s.so", staging_dir, expect_id);
    snprintf(js_path, sizeof(js_path), "%s/main.js", staging_dir);
    struct stat st_so, st_js;
    bool has_so = (stat(so_path, &st_so) == 0 && S_ISREG(st_so.st_mode));
    bool has_js = (stat(js_path, &st_js) == 0 && S_ISREG(st_js.st_mode));
    const cJSON *rt = cJSON_GetObjectItem(root, "runtime");
    const cJSON *entry = cJSON_GetObjectItem(root, "entry");
    const bool rt_hybrid = cJSON_IsString(rt) && strcmp(rt->valuestring, "hybrid") == 0;
    const bool entry_native = cJSON_IsString(entry) && strcmp(entry->valuestring, "native") == 0;
    const bool entry_js = cJSON_IsString(entry) && strcmp(entry->valuestring, "js") == 0;

    /* Both artifacts imply two possible entries — must declare exactly one via entry= */
    if (has_so && has_js) {
        if (!rt_hybrid) {
            ESP_LOGE(TAG, "invalid package: both %s.so and main.js require runtime=hybrid + entry=",
                     expect_id);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (entry_native == entry_js) {
            /* missing or invalid — not exactly one of native|js */
            ESP_LOGE(TAG, "invalid package: hybrid needs exactly one entry (native|js), got %s",
                     cJSON_IsString(entry) ? entry->valuestring : "(missing)");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    } else if (rt_hybrid) {
        ESP_LOGE(TAG, "invalid package: runtime=hybrid needs both %s.so and main.js", expect_id);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    } else if (cJSON_IsString(entry)) {
        ESP_LOGE(TAG, "invalid package: entry= only valid with runtime=hybrid");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (kind == HUB_KIND_PACK) {
        cJSON_Delete(root);
        if (out_kind) {
            *out_kind = HUB_KIND_PACK;
        }
        return ESP_OK;
    }
    if (kind == HUB_KIND_JS) {
        const cJSON *api = cJSON_GetObjectItem(root, "host_api");
        if (cJSON_IsNumber(api) && (int)api->valuedouble != 0) {
            ESP_LOGE(TAG, "unsupported host_api %.0f (need 0)", api->valuedouble);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_VERSION;
        }
        if (!has_js) {
            ESP_LOGE(TAG, "L2 package missing main.js");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (has_so) {
            ESP_LOGE(TAG, "invalid package: runtime=js must not include %s.so (use hybrid)", expect_id);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_Delete(root);
        if (out_kind) {
            *out_kind = HUB_KIND_JS;
        }
        return ESP_OK;
    }

    if (rt_hybrid) {
        const cJSON *api = cJSON_GetObjectItem(root, "host_api");
        if (cJSON_IsNumber(api) && (int)api->valuedouble != 0) {
            ESP_LOGE(TAG, "unsupported host_api %.0f (need 0)", api->valuedouble);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_VERSION;
        }
        /* entry already validated above when has_so && has_js */
    } else if (!has_so) {
        ESP_LOGE(TAG, "native package missing %s.so", expect_id);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    } else if (has_js) {
        ESP_LOGE(TAG, "invalid package: runtime=native but also has main.js (use hybrid + entry=)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);
    esp_err_t err = validate_mhub_file(so_path);
    if (err != ESP_OK) {
        return err;
    }
    if (out_kind) {
        *out_kind = HUB_KIND_NATIVE;
    }
    return ESP_OK;
}

static esp_err_t install_pack_from_staging(const char *id, const char *staging_dir,
                                            const cJSON *item, bool upgrade)
{
    bool already = pack_installed(id);
    if (already && !upgrade) {
        return ESP_ERR_INVALID_STATE;
    }
    if (upgrade && !already) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = ensure_dir(MINIHUB_PACKS_DIR);
    if (err != ESP_OK) {
        return err;
    }

    char src[MINIHUB_MODULE_FILE_PATH_MAX];
    snprintf(src, sizeof(src), "%s/module.json", staging_dir);
    char config_path[MINIHUB_PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/%s.json", MINIHUB_PACKS_DIR, id);

    cJSON *pack = load_json_file(src);
    if (!pack) {
        return ESP_FAIL;
    }
    const cJSON *version_j = cJSON_GetObjectItem(item, "version");
    if (cJSON_IsString(version_j)) {
        cJSON_DeleteItemFromObject(pack, "version");
        cJSON_AddStringToObject(pack, "version", version_j->valuestring);
    }
    if (!cJSON_GetObjectItem(pack, "enabled")) {
        cJSON_AddBoolToObject(pack, "enabled", true);
    }
    char *json = cJSON_Print(pack);
    cJSON_Delete(pack);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    err = write_file_atomic(config_path, json);
    free(json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s pack %s -> %s", upgrade ? "upgraded" : "installed", id, config_path);
    }
    return err;
}

static esp_err_t install_module(const cJSON *item, bool upgrade)
{
    const cJSON *id_j = cJSON_GetObjectItem(item, "id");
    const cJSON *url_j = cJSON_GetObjectItem(item, "url");
    if (!cJSON_IsString(id_j) || !cJSON_IsString(url_j)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *id = id_j->valuestring;
    if (strlen(id) >= 48) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Offline ABI compatibility (defense in depth). The store server already
     * filtered by the device's abi_version entry (per-version symbol set, optional
     * per-version abi_total_hash) before serving this catalog; re-check against the
     * baked host table so a sideloaded or stale package is also rejected locally.
     * The catalog `abi` block is generated by tools/sync_store_catalog.py. */
    const cJSON *abi_j = cJSON_GetObjectItem(item, "abi");
    if (cJSON_IsObject(abi_j)) {
        esp_err_t abi_err = minihub_module_abi_check_symbols(abi_j);
        if (abi_err != ESP_OK) {
            ESP_LOGE(TAG, "module %s abi symbols incompatible with this firmware; install denied", id);
            return ESP_ERR_INVALID_VERSION;
        }
    }

    const char *rel = url_j->valuestring;
    size_t rel_n = strlen(rel);
    if (rel_n < 5 || strcmp(rel + rel_n - 5, ".mhub") != 0) {
        ESP_LOGE(TAG, "module %s url must be .mhub (got %s)", id, rel);
        return ESP_ERR_INVALID_ARG;
    }

    const bool as_pack = catalog_item_config_only(item);
    bool already = as_pack ? pack_installed(id) : module_installed(id);
    if (already && !upgrade) {
        ESP_LOGW(TAG, "%s %s already installed (use upgrade)", as_pack ? "pack" : "module", id);
        return ESP_ERR_INVALID_STATE;
    }
    if (upgrade && !already) {
        ESP_LOGW(TAG, "%s %s not installed", as_pack ? "pack" : "module", id);
        return ESP_ERR_NOT_FOUND;
    }

    char pkg_dir[MINIHUB_MODULE_PKG_PATH_MAX];
    module_pkg_dir(id, pkg_dir, sizeof(pkg_dir));
    char staging_dir[MINIHUB_MODULE_STAGING_PATH_MAX];
    snprintf(staging_dir, sizeof(staging_dir), "%s/%s.staging", MINIHUB_STORE_MODULES_DIR, id);

    cJSON *existing = (!as_pack && upgrade) ? installed_module_manifest(id) : NULL;

    char url[MINIHUB_STORE_URL_MAX];
    if (rel[0] == '/') {
        strncpy(url, rel, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else {
        char base[MINIHUB_STORE_BASE_URL_MAX];
        store_base_url(base, sizeof(base));
        snprintf(url, sizeof(url), "%s", rel);
        if (strncmp(rel, "http://", 7) != 0 && strncmp(rel, "https://", 8) != 0) {
            snprintf(url, sizeof(url), "%s/%s", base, rel);
        }
    }

    size_t max_bytes = hub_max_bytes();
    const cJSON *size_j = cJSON_GetObjectItem(item, "size");
    if (cJSON_IsNumber(size_j) && size_j->valuedouble > 0) {
        if ((size_t)size_j->valuedouble > max_bytes) {
            ESP_LOGE(TAG, "hub %s size %.0f > max %u", id, size_j->valuedouble, (unsigned)max_bytes);
            cJSON_Delete(existing);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    ESP_LOGI(TAG, "%s hub %s from %s", upgrade ? "upgrading" : "downloading", id, url);

    esp_err_t err = ensure_dir(MINIHUB_STORE_MODULES_DIR);
    if (err != ESP_OK) {
        cJSON_Delete(existing);
        return err;
    }

    (void)minihub_hub_remove_tree(staging_dir);

    uint8_t *hub_buf = NULL;
    size_t hub_len = 0;
    err = fetch_hub_to_psram((rel[0] == '/') ? rel : url, &hub_buf, &hub_len, max_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fetch hub %s failed: %s", id, esp_err_to_name(err));
        cJSON_Delete(existing);
        return err;
    }

    if (cJSON_IsNumber(size_j) && size_j->valuedouble > 0 &&
        hub_len != (size_t)size_j->valuedouble) {
        ESP_LOGE(TAG, "hub %s size mismatch: got %u catalog %ld",
                 id, (unsigned)hub_len, (long)size_j->valuedouble);
        heap_caps_free(hub_buf);
        cJSON_Delete(existing);
        return ESP_ERR_INVALID_SIZE;
    }

    err = minihub_hub_zip_extract(hub_buf, hub_len, staging_dir);
    heap_caps_free(hub_buf);
    if (err != ESP_OK) {
        (void)minihub_hub_remove_tree(staging_dir);
        cJSON_Delete(existing);
        return err;
    }

    hub_kind_t kind = HUB_KIND_NATIVE;
    err = validate_staging_module(staging_dir, id, &kind);
    if (err != ESP_OK) {
        (void)minihub_hub_remove_tree(staging_dir);
        cJSON_Delete(existing);
        return err;
    }

    if (kind == HUB_KIND_PACK) {
        err = install_pack_from_staging(id, staging_dir, item, upgrade);
        (void)minihub_hub_remove_tree(staging_dir);
        cJSON_Delete(existing);
        return err;
    }

    if (existing) {
        char staging_manifest[MINIHUB_MODULE_FILE_PATH_MAX];
        snprintf(staging_manifest, sizeof(staging_manifest), "%s/module.json", staging_dir);
        cJSON *manifest = load_json_file(staging_manifest);
        if (manifest) {
            manifest_merge_preserve(manifest, existing);
            char *json = cJSON_Print(manifest);
            cJSON_Delete(manifest);
            if (json) {
                (void)write_file_atomic(staging_manifest, json);
                free(json);
            }
        }
        cJSON_Delete(existing);
        existing = NULL;
    }

    (void)minihub_hub_remove_tree(pkg_dir);
    if (rename(staging_dir, pkg_dir) != 0) {
        ESP_LOGE(TAG, "rename staging -> %s failed: errno=%d", pkg_dir, errno);
        (void)minihub_hub_remove_tree(staging_dir);
        return ESP_FAIL;
    }

    char legacy_json[MINIHUB_PATH_MAX];
    char legacy_bin[MINIHUB_PATH_MAX];
    snprintf(legacy_json, sizeof(legacy_json), "%s/%s.json", MINIHUB_MODULES_DIR, id);
    snprintf(legacy_bin, sizeof(legacy_bin), "%s/%s.mhub", MINIHUB_MODULES_DIR, id);
    unlink(legacy_json);
    unlink(legacy_bin);

    const cJSON *adapter_j = cJSON_GetObjectItem(item, "adapter");
    /* L2 JS and L3 native both need an adapters.json slot so Overview
     * adapterEnabled() shows their points (chips + card filter). */
    if ((kind == HUB_KIND_NATIVE || kind == HUB_KIND_JS) && cJSON_IsString(adapter_j)) {
        /* Fresh install force-enables; upgrade leaves an existing slot alone. */
        (void)minihub_config_ensure_adapter_slot(adapter_j->valuestring, !upgrade);
    } else if (kind == HUB_KIND_JS && (!adapter_j || !cJSON_IsString(adapter_j))) {
        /* Catalog omitted adapter — fall back to package id (usual L2 convention). */
        (void)minihub_config_ensure_adapter_slot(id, !upgrade);
    }

    {
        char main_js[MINIHUB_MODULE_FILE_PATH_MAX];
        snprintf(main_js, sizeof(main_js), "%s/main.js", pkg_dir);
        struct stat st_js;
        bool hybrid = (kind == HUB_KIND_NATIVE &&
                       stat(main_js, &st_js) == 0 && S_ISREG(st_js.st_mode));
        ESP_LOGI(TAG, "%s %s hub -> %s (%s)",
                 upgrade ? "upgraded" : "installed", id, pkg_dir,
                 kind == HUB_KIND_JS ? "js" : (hybrid ? "hybrid" : "native"));
    }
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_DYNAMIC_MODULES */

static esp_err_t install_pack(const cJSON *item, bool upgrade, bool force)
{
    const cJSON *id_j = cJSON_GetObjectItem(item, "id");
    const cJSON *url_j = cJSON_GetObjectItem(item, "url");
    const cJSON *version_j = cJSON_GetObjectItem(item, "version");
    if (!cJSON_IsString(id_j) || !cJSON_IsString(url_j)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool already = pack_installed(id_j->valuestring);
    if (already && !upgrade) {
        ESP_LOGW(TAG, "pack %s already installed (use upgrade)", id_j->valuestring);
        return ESP_ERR_INVALID_STATE;
    }
    if (upgrade && !already) {
        return ESP_ERR_NOT_FOUND;
    }

    if (upgrade && !force && cJSON_IsString(version_j)) {
        char cur[32];
        if (installed_pack_version(id_j->valuestring, cur, sizeof(cur)) &&
            version_compare(version_j->valuestring, cur) <= 0) {
            ESP_LOGI(TAG, "pack %s already at v%s", id_j->valuestring, cur);
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t err = ensure_dir(MINIHUB_PACKS_DIR);
    if (err != ESP_OK) {
        return err;
    }

    char config_path[MINIHUB_PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/%s.json", MINIHUB_PACKS_DIR, id_j->valuestring);

    char tmp_path[MINIHUB_PATH_MAX + 4];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config_path);

    char url[MINIHUB_STORE_URL_MAX];
    const char *rel = url_j->valuestring;
    if (rel[0] == '/') {
        strncpy(url, rel, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else {
        char base[MINIHUB_STORE_BASE_URL_MAX];
        store_base_url(base, sizeof(base));
        snprintf(url, sizeof(url), "%s/%s", base, rel);
    }

    ESP_LOGI(TAG, "%s pack %s from %s",
             upgrade ? "upgrading" : "downloading", id_j->valuestring, url);
    err = download_or_copy_store_file(rel[0] == '/' ? rel : url, tmp_path);
    if (err != ESP_OK) {
        return err;
    }

    if (cJSON_IsString(version_j)) {
        cJSON *pack = load_json_file(tmp_path);
        if (pack) {
            cJSON_DeleteItemFromObject(pack, "version");
            cJSON_AddStringToObject(pack, "version", version_j->valuestring);
            char *json = cJSON_Print(pack);
            cJSON_Delete(pack);
            if (json) {
                FILE *wf = fopen(tmp_path, "w");
                if (wf) {
                    fputs(json, wf);
                    fclose(wf);
                }
                free(json);
            }
        }
    }

    unlink(config_path);
    if (rename(tmp_path, config_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s pack %s -> %s",
             upgrade ? "upgraded" : "installed", id_j->valuestring, config_path);
    return ESP_OK;
}

static esp_err_t store_install_item(const cJSON *item, const char *kind, bool upgrade, bool force)
{
    const cJSON *url_j = cJSON_GetObjectItem(item, "url");
    bool mhub_url = false;
    if (cJSON_IsString(url_j) && url_j->valuestring) {
        size_t n = strlen(url_j->valuestring);
        mhub_url = (n >= 5 && strcmp(url_j->valuestring + n - 5, ".mhub") == 0);
    }

    /* Unified .mhub (zip) install for L1 pack / L2 js / L3 native */
    if (mhub_url) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        return install_module(item, upgrade);
#else
        (void)upgrade;
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }

    /* Legacy bare JSON pack URL (pre-unified catalog) */
    if (catalog_item_config_only(item) || strcmp(kind, "pack") == 0) {
        return install_pack(item, upgrade, force);
    }
    if (strcmp(kind, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        return install_module(item, upgrade);
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t store_install_from_catalog(const char *kind, const char *id, bool upgrade, bool force)
{
    if (!kind || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *catalog = minihub_store_fetch_catalog();
    if (!catalog) {
        return ESP_FAIL;
    }

    const cJSON *item = catalog_find(catalog, kind, id);
    if (!item) {
        cJSON_Delete(catalog);
        return ESP_ERR_NOT_FOUND;
    }

    if (upgrade && !force) {
        const cJSON *version_j = cJSON_GetObjectItem(item, "version");
        char cur[32];
        bool have_cur = false;
        bool allow = false;
        bool config_only = catalog_item_config_only(item);
        if (config_only || strcmp(kind, "pack") == 0) {
            have_cur = installed_pack_version(id, cur, sizeof(cur)) != NULL;
        } else if (strcmp(kind, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
            have_cur = installed_module_version(id, cur, sizeof(cur)) != NULL;
#endif
        }
        if (have_cur && cJSON_IsString(version_j) &&
            version_compare(version_j->valuestring, cur) > 0) {
            allow = true;
        }
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        if (!config_only && strcmp(kind, "module") == 0) {
            cJSON *manifest = installed_module_manifest(id);
            if (manifest) {
                const cJSON *bin_size = cJSON_GetObjectItem(manifest, "binary_size");
                /* Compare MHMD .so sizes — catalog "size" is the .mhub zip. */
                const cJSON *cat_bin = cJSON_GetObjectItem(item, "binary_size");
                if (cJSON_IsNumber(bin_size) && cJSON_IsNumber(cat_bin) &&
                    cat_bin->valuedouble > 0 &&
                    bin_size->valuedouble != cat_bin->valuedouble) {
                    allow = true;
                }
                const cJSON *inst_av = cJSON_GetObjectItem(manifest, "adapter_version");
                const cJSON *cat_av = cJSON_GetObjectItem(item, "adapter_version");
                if (cJSON_IsString(inst_av) && cJSON_IsString(cat_av) &&
                    strcmp(inst_av->valuestring, cat_av->valuestring) != 0) {
                    allow = true;
                }
                cJSON_Delete(manifest);
            }
        }
#endif
        if (!allow) {
            cJSON_Delete(catalog);
            ESP_LOGI(TAG, "%s %s already at v%s", kind, id, have_cur ? cur : "?");
            return ESP_ERR_INVALID_STATE;
        }
    }

    esp_err_t err = store_install_item(item, kind, upgrade, force);
    if (err == ESP_OK) {
        /* A freshly installed/upgraded module may reference device types whose
         * icon or parameter style is not yet on the device. Notify the update
         * layer so it kicks an asset sync immediately (instead of waiting for
         * the next hourly pass); the subscription runs the sync on a separate
         * task, so this publish never blocks the install HTTP request. */
        cJSON *d = cJSON_CreateObject();
        if (d) {
            cJSON_AddStringToObject(d, "id", id);
            cJSON_AddStringToObject(d, "type", kind);
            cJSON_AddBoolToObject(d, "upgrade", upgrade);
            minihub_event_bus_publish("store.module.installed", d);
        }
    }
    cJSON_Delete(catalog);
    return err;
}

esp_err_t minihub_store_install(const char *kind, const char *id)
{
    return store_install_from_catalog(kind, id, false, false);
}

esp_err_t minihub_store_upgrade(const char *kind, const char *id)
{
    return store_install_from_catalog(kind, id, true, false);
}

esp_err_t minihub_store_reinstall(const char *kind, const char *id)
{
    /* Same preserve path as upgrade (adapters.json slot and module.json
     * enabled stay). Unlike upgrade, the catalog version may be unchanged.
     * Unlike uninstall, the adapter slot — account and config — is not removed. */
    return store_install_from_catalog(kind, id, true, true);
}

esp_err_t minihub_store_uninstall(const char *kind, const char *id)
{
    if (!kind || !id) {
        return ESP_ERR_INVALID_ARG;
    }

    /* JSON-only modules live under packs/; binary modules under modules/ */
    bool as_pack = (strcmp(kind, "pack") == 0) ||
                   (strcmp(kind, "module") == 0 && pack_installed(id)
#if CONFIG_MINIHUB_DYNAMIC_MODULES
                    && !module_installed(id)
#endif
                   );
    if (as_pack) {
        char config_path[MINIHUB_PATH_MAX];
        snprintf(config_path, sizeof(config_path), "%s/%s.json", MINIHUB_PACKS_DIR, id);
        unlink(config_path);
        char store_path[MINIHUB_PATH_MAX];
        snprintf(store_path, sizeof(store_path), "%s/%s.json", MINIHUB_STORE_PACKS_DIR, id);
        unlink(store_path);
        return minihub_pack_delete(id);
    }
    if (strcmp(kind, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        char adapter_name[MINIHUB_MODULE_ID_MAX + 1];
        module_uninstall_adapter_name(id, adapter_name, sizeof(adapter_name));

        char pkg_dir[MINIHUB_MODULE_PKG_PATH_MAX];
        module_pkg_dir(id, pkg_dir, sizeof(pkg_dir));
        struct stat st_pkg;
        bool pkg_exists = stat(pkg_dir, &st_pkg) == 0;
        bool on_disk = module_installed(id);

        if (!on_disk && !pkg_exists) {
            (void)minihub_module_delete(id);
            (void)minihub_module_forget_host_rpc(id);
            (void)minihub_config_remove_adapter(adapter_name);
            return ESP_OK;
        }

        /* Best-effort loader delete; always remove_tree for a full store uninstall. */
        esp_err_t del = minihub_module_delete(id);
        if (del == ESP_ERR_NOT_SUPPORTED) {
            if (!pkg_exists) {
                ESP_LOGW(TAG, "uninstall '%s': firmware builtin — use adapters enabled=false", id);
                return ESP_ERR_NOT_SUPPORTED;
            }
            ESP_LOGW(TAG, "uninstall '%s': partial delete (%s), removing package dir",
                     id, esp_err_to_name(del));
        } else if (del != ESP_OK && del != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "uninstall '%s': delete returned %s, removing package dir",
                     id, esp_err_to_name(del));
        }

        (void)minihub_hub_remove_tree(pkg_dir);
        char staging_dir[MINIHUB_MODULE_STAGING_PATH_MAX];
        snprintf(staging_dir, sizeof(staging_dir), "%s.staging", pkg_dir);
        (void)minihub_hub_remove_tree(staging_dir);
        /* Legacy flat layout */
        char legacy_bin[MINIHUB_PATH_MAX];
        snprintf(legacy_bin, sizeof(legacy_bin), "%s/%s.mhub", MINIHUB_STORE_MODULES_DIR, id);
        unlink(legacy_bin);
        char legacy_json[MINIHUB_PATH_MAX];
        snprintf(legacy_json, sizeof(legacy_json), "%s/%s.json", MINIHUB_STORE_MODULES_DIR, id);
        unlink(legacy_json);
        return ESP_OK;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t minihub_store_factory_reset(void)
{
    /* Remove every Store-installed module and pack. Factory preinstall
     * (/store/preinstall) and the Web console (/data/web) live elsewhere and
     * are intentionally preserved. */
    (void)minihub_hub_remove_tree(MINIHUB_MODULES_DIR);
    (void)minihub_hub_remove_tree(MINIHUB_PACKS_DIR);
    (void)minihub_hub_remove_tree(MINIHUB_STORE_PACKS_DIR);
    esp_err_t err = ESP_OK;
    if (ensure_dir(MINIHUB_MODULES_DIR) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (ensure_dir(MINIHUB_PACKS_DIR) != ESP_OK) {
        err = ESP_FAIL;
    }
    if (ensure_dir(MINIHUB_STORE_PACKS_DIR) != ESP_OK) {
        err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "factory reset cleared store modules and packs");
    return err;
}

cJSON *minihub_store_catalog_merged(void)
{
    cJSON *merged = cJSON_CreateArray();
    cJSON *remote = minihub_store_fetch_catalog();
    if (!remote) {
        return merged;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, remote) {
        cJSON *copy = cJSON_Duplicate(item, true);
        if (!copy) {
            continue;
        }
        const cJSON *kind = cJSON_GetObjectItem(item, "type");
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        bool installed = false;
        bool config_only = catalog_item_config_only(item);
        if (cJSON_IsString(kind) && cJSON_IsString(id)) {
            if (config_only || strcmp(kind->valuestring, "pack") == 0) {
                installed = pack_installed(id->valuestring);
            } else if (strcmp(kind->valuestring, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
                installed = module_installed(id->valuestring);
#endif
            }
        }
        cJSON_AddBoolToObject(copy, "installed", installed);
        if (installed && cJSON_IsString(kind) && cJSON_IsString(id)) {
            char cur_ver[32];
            const char *installed_ver = NULL;
            if (config_only || strcmp(kind->valuestring, "pack") == 0) {
                if (installed_pack_version(id->valuestring, cur_ver, sizeof(cur_ver))) {
                    installed_ver = cur_ver;
                }
            } else if (strcmp(kind->valuestring, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
                if (installed_module_version(id->valuestring, cur_ver, sizeof(cur_ver))) {
                    installed_ver = cur_ver;
                }
#endif
            }
            if (installed_ver) {
                cJSON_AddStringToObject(copy, "installed_version", installed_ver);
            }
            const cJSON *catalog_ver = cJSON_GetObjectItem(item, "version");
            bool upgrade = false;
            if (installed_ver && cJSON_IsString(catalog_ver)) {
                upgrade = version_compare(catalog_ver->valuestring, installed_ver) > 0;
            }
#if CONFIG_MINIHUB_DYNAMIC_MODULES
            if (!config_only && strcmp(kind->valuestring, "module") == 0) {
                cJSON *manifest = installed_module_manifest(id->valuestring);
                if (manifest) {
                    const cJSON *bin_size = cJSON_GetObjectItem(manifest, "binary_size");
                    /* Compare MHMD .so sizes — catalog "size" is the .mhub zip. */
                    const cJSON *cat_bin = cJSON_GetObjectItem(item, "binary_size");
                    if (cJSON_IsNumber(bin_size) && cJSON_IsNumber(cat_bin) &&
                        cat_bin->valuedouble > 0 &&
                        bin_size->valuedouble != cat_bin->valuedouble) {
                        upgrade = true;
                    }
                    const cJSON *inst_av = cJSON_GetObjectItem(manifest, "adapter_version");
                    const cJSON *cat_av = cJSON_GetObjectItem(item, "adapter_version");
                    if (cJSON_IsString(inst_av) && cJSON_IsString(cat_av) &&
                        strcmp(inst_av->valuestring, cat_av->valuestring) != 0) {
                        upgrade = true;
                    }
                    cJSON_Delete(manifest);
                }
            }
#endif
            cJSON_AddBoolToObject(copy, "upgrade_available", upgrade);
        } else {
            cJSON_AddBoolToObject(copy, "upgrade_available", false);
        }
        cJSON_AddItemToArray(merged, copy);
    }
    cJSON_Delete(remote);
    return merged;
}

bool minihub_store_copy_base_url(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    store_base_url(out, out_len);
    return out[0] != '\0';
}

static bool installed_item_version(const cJSON *item, char *out, size_t out_len)
{
    const cJSON *kind = cJSON_GetObjectItem(item, "type");
    const cJSON *id = cJSON_GetObjectItem(item, "id");
    if (!cJSON_IsString(id) || !id->valuestring[0]) {
        return false;
    }
    bool pack = catalog_item_config_only(item) ||
                (cJSON_IsString(kind) && strcmp(kind->valuestring, "pack") == 0);
    if (pack) {
        return installed_pack_version(id->valuestring, out, out_len) != NULL;
    }
    if (cJSON_IsString(kind) && strcmp(kind->valuestring, "module") == 0) {
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        return installed_module_version(id->valuestring, out, out_len) != NULL;
#else
        return false;
#endif
    }
    return false;
}

esp_err_t minihub_store_list_upgrades(cJSON **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    char base[MINIHUB_STORE_BASE_URL_MAX];
    store_base_url(base, sizeof(base));
    if (!base[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *catalog = http_get_catalog_array(base);
    if (!catalog) {
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        cJSON_Delete(catalog);
        return ESP_ERR_NO_MEM;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, catalog) {
        char cur[32];
        if (!installed_item_version(item, cur, sizeof(cur))) {
            continue;
        }
        const cJSON *ver = cJSON_GetObjectItem(item, "version");
        if (!cJSON_IsString(ver) || !ver->valuestring ||
            version_compare(ver->valuestring, cur) <= 0) {
            continue;
        }
        cJSON *row = cJSON_CreateObject();
        if (!row) {
            continue;
        }
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *kind = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(id) && id->valuestring) {
            cJSON_AddStringToObject(row, "id", id->valuestring);
        }
        if (cJSON_IsString(name) && name->valuestring && name->valuestring[0]) {
            cJSON_AddStringToObject(row, "name", name->valuestring);
        } else if (cJSON_IsString(id) && id->valuestring) {
            cJSON_AddStringToObject(row, "name", id->valuestring);
        }
        if (cJSON_IsString(kind) && kind->valuestring) {
            cJSON_AddStringToObject(row, "type", kind->valuestring);
        }
        cJSON_AddStringToObject(row, "installed", cur);
        cJSON_AddStringToObject(row, "version", ver->valuestring);
        cJSON_AddItemToArray(arr, row);
    }
    cJSON_Delete(catalog);
    *out = arr;
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_STORE */
