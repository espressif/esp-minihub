/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "minihub_core.h"
#include "minihub_device_widget.h"
#include "minihub_net.h"
#include "minihub_ota.h"
#include "minihub_paths.h"
#include "minihub_store.h"
#include "minihub_update.h"
#include "sdkconfig.h"
#include "update_internal.h"

#ifndef MINIHUB_DW_STYLES_DIR
#define MINIHUB_DW_STYLES_DIR "/data/styles"
#endif

static const char *TAG = "minihub_update";
static const char *FW_STATE_PATH = MINIHUB_CONFIG_DIR "/update_firmware.json";

#define MH_UPDATE_HINT_PERIOD_MS (60 * 60 * 1000)
#define MH_UPDATE_HINT_WAIT_IP_MS 5000
#define MH_UPDATE_NOTE_MAX       8192

static SemaphoreHandle_t s_mu;

/* Strong definition of the weak symbol declared in minihub_device_widget.h: it pulls
 * the icon for a device type missing from the local registry from the deploy root.
 *
 * NOTE: this must live here, not in update_assets.c. Nothing else references
 * update_assets.c, so that object is not pulled out of the static library; with only
 * the weak stub present the stub would win and every fallback would return
 * NOT_SUPPORTED. This file is always linked, so the strong definition reliably
 * overrides the stub. Verify with nm after moving it. */
esp_err_t minihub_dw_cloud_fetch_default(const char *base_url, const char *device_id)
{
    char base[MINIHUB_STORE_BASE_URL_MAX];
    if (!base_url || !base_url[0]) {
        if (!minihub_store_copy_base_url(base, sizeof(base))) {
            ESP_LOGW(TAG, "no store URL configured for device asset fetch");
            return ESP_ERR_INVALID_STATE;
        }
        base_url = base;
    }
    /* device_id is a TYPE here (zigbee_adapter.c passes dev->type). ESP_FAIL is
     * the download helper's answer to a 404, i.e. this type ships no artwork. */
    esp_err_t err = minihub_update_fetch_device_icon(base_url, device_id);
    if (err != ESP_OK) {
        return err;
    }
    /* Notify the Web console so /api/device_widgets + /img/ pick up the new assets. */
    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "device", device_id);
        minihub_event_bus_publish("device_widgets.updated", data);
        cJSON_Delete(data);
    }
    return ESP_OK;
}

static int err_rc(esp_err_t err)
{
    if (err == ESP_OK) {
        return 0;
    }
    if ((int)err < 0) {
        return (int)err;
    }
    return -((int)err);
}

static bool lock_take(void)
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (!s_mu) {
        return false;
    }
    return xSemaphoreTake(s_mu, pdMS_TO_TICKS(30000)) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(s_mu);
}

static void cache_running_firmware_note(const mh_update_index_t *idx, const char *chip);

static void local_chip(char *out, size_t n)
{
    /* Same lowercase chip string as ?chip= on /api/catalog and Firmwares/<chip>/. */
    const char *t = CONFIG_IDF_TARGET;
    size_t i = 0;
    for (; t[i] && i + 1 < n; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static const char *local_board(void)
{
#ifdef CONFIG_MINIHUB_BOARD
    if (CONFIG_MINIHUB_BOARD[0]) {
        return CONFIG_MINIHUB_BOARD;
    }
#endif
    return NULL;
}

static bool firmware_match(const mh_update_item_t *item, const char *chip)
{
    return mh_update_target_match(item, chip) && mh_update_board_match(item, local_board());
}

typedef struct {
    bool present;
    bool armed;
    char version[32];
    char sha256[65];
    size_t size;
} fw_rec_t;

static bool load_fw_rec(fw_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    FILE *file = fopen(FW_STATE_PATH, "rb");
    if (!file) {
        return false;
    }
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return false;
    }
    cJSON *ver = cJSON_GetObjectItem(root, "version");
    cJSON *sha = cJSON_GetObjectItem(root, "sha256");
    cJSON *size = cJSON_GetObjectItem(root, "size");
    cJSON *armed = cJSON_GetObjectItem(root, "armed");
    if (cJSON_IsString(ver) && ver->valuestring) {
        snprintf(rec->version, sizeof(rec->version), "%s", ver->valuestring);
        rec->present = true;
    }
    if (cJSON_IsString(sha) && sha->valuestring) {
        snprintf(rec->sha256, sizeof(rec->sha256), "%s", sha->valuestring);
    }
    if (cJSON_IsNumber(size) && size->valuedouble >= 0) {
        rec->size = (size_t)size->valuedouble;
    }
    rec->armed = cJSON_IsTrue(armed);
    cJSON_Delete(root);
    return rec->present;
}

static void save_fw_rec(const fw_rec_t *rec)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "version", rec->version);
    cJSON_AddStringToObject(root, "sha256", rec->sha256);
    cJSON_AddNumberToObject(root, "size", (double)rec->size);
    cJSON_AddBoolToObject(root, "armed", rec->armed);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }
    FILE *file = fopen(FW_STATE_PATH, "wb");
    if (file) {
        fputs(text, file);
        fclose(file);
    }
    free(text);
}

/* Defined below: strip a leading "v"/"V" so "v0.1.1" and "0.1.1" compare equal.
 * mh_update_version_cmp() never consumes a stray 'v', so an un-normalized
 * compare ranks "v0.1.1" BELOW "0.1.0" and hides a newer firmware. */
static const char *ver_norm(const char *v);

static const char *running_version(void)
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
    return "";
}

static bool firmware_needs(const mh_update_item_t *best)
{
    const char *cur = running_version();
    fw_rec_t rec;
    bool have = load_fw_rec(&rec);
    if (have && rec.armed && strcmp(rec.version, cur) == 0) {
        rec.armed = false;
        save_fw_rec(&rec);
    }
    if (mh_update_version_cmp(ver_norm(best->version ? best->version : ""), ver_norm(cur)) > 0) {
        return true;
    }
    if (have && !rec.armed && strcmp(rec.version, cur) == 0) {
        if (best->size != 0 && rec.size != best->size) {
            return true;
        }
        if (best->sha256 && best->sha256[0] && rec.sha256[0] &&
            strcasecmp(rec.sha256, best->sha256) != 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t index_url(minihub_update_kind_t kind, const char *base, char *out, size_t n)
{
    const char *suffix = NULL;
    if (kind == MINIHUB_UPDATE_FIRMWARE) {
        /* OTA application images: GET /api/firmware (role=ota).
         * /api/v1/firmware is the website flasher list (plans / merged), not Hub OTA. */
        suffix = "/api/firmware";
    } else if (kind == MINIHUB_UPDATE_ASSET) {
        suffix = "/api/assets";
    } else if (kind == MINIHUB_UPDATE_WEB) {
        suffix = "/api/web";
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return mh_update_join_url(out, n, base, NULL, suffix);
}

static esp_err_t fetch_index(minihub_update_kind_t kind, const char *base, mh_update_index_t *idx)
{
    char url[MH_UPDATE_URL_MAX];
    esp_err_t err = index_url(kind, base, url, sizeof(url));
    if (err != ESP_OK) {
        return err;
    }
    /* mac=, chip=, board=, cur_version= — one heap URL so the HTTP client
     * never aims at a shorter-lived local. */
    char *full = minihub_net_url_with_device_queries(url, running_version());
    if (!full) {
        return ESP_ERR_NO_MEM;
    }
    char *body = NULL;
    size_t len = 0;
    err = mh_update_http_get(full, &body, &len, MH_UPDATE_INDEX_MAX);
    free(full);
    if (err != ESP_OK) {
        return err;
    }
    err = mh_update_parse_document(body, len, kind == MINIHUB_UPDATE_FIRMWARE, idx);
    free(body);
    return err;
}

static const mh_update_item_t *best_firmware(const mh_update_index_t *idx, const char *chip)
{
    const mh_update_item_t *best = NULL;
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *item = &idx->items[i];
        if (!item->url || !item->url[0] || !firmware_match(item, chip)) {
            continue;
        }
        if (!best || mh_update_version_cmp(item->version, best->version) > 0) {
            best = item;
        }
    }
    return best;
}

/* Defined below, after version_text_eq(): the published item matching the version
 * that is running, used by a forced re-install (repair, not upgrade). */
static const mh_update_item_t *running_firmware(const mh_update_index_t *idx, const char *chip);

/* Defined below, after read_web_named(): the web version whose files should be
 * applied. A forced re-install re-applies the running version (repair), a plain
 * update the newest one. */
static const char *web_version_target(const mh_update_index_t *idx, const char *chip, bool force);

/* Defined below: pull icons/styles for the device types this Hub actually runs
 * (GET /api/assets/device/<type>). Bulk ASSET sync skips images/ and styles/,
 * so this is the only way they ever arrive -- it must therefore also run on an
 * explicit asset apply, not just on the hourly pass. */
static void sync_used_device_assets(const char *base);

static const char *web_version_max(const mh_update_index_t *idx, const char *chip)
{
    const char *best = NULL;
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *item = &idx->items[i];
        if (!mh_update_target_match(item, chip) || !item->version) {
            continue;
        }
        if (!best || mh_update_version_cmp(item->version, best) > 0) {
            best = item->version;
        }
    }
    return best;
}

static esp_err_t dest_for(minihub_update_kind_t kind, const mh_update_item_t *item,
                          char *out, size_t n)
{
    const char *url = item->url;
    if (!url || url[0] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    const char *rel = url;
    const char *root = NULL;
    if (kind == MINIHUB_UPDATE_ASSET) {
        if (strncmp(url, "model/", 6) == 0) {
            root = MINIHUB_DW_MODEL_DIR;
            rel = url + 6;
        } else if (strncmp(url, "images/", 7) == 0) {
            root = MINIHUB_DW_IMAGES_DIR;
            rel = url + 7;
        } else if (strncmp(url, "styles/", 7) == 0) {
            root = MINIHUB_DW_STYLES_DIR;
            rel = url + 7;
        } else if (item->type && strcmp(item->type, "image") == 0) {
            root = MINIHUB_DW_IMAGES_DIR;
        } else {
            root = MINIHUB_DW_MODEL_DIR;
        }
    } else if (kind == MINIHUB_UPDATE_WEB) {
        root = MINIHUB_WEB_DIR;
        if (item->version && item->version[0]) {
            size_t vl = strlen(item->version);
            if (strncmp(url, item->version, vl) == 0 && url[vl] == '/') {
                rel = url + vl + 1;
            }
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mh_update_rel_ok(rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    int wrote = snprintf(out, n, "%s/%s", root, rel);
    if (wrote < 0 || (size_t)wrote >= n) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool file_matches(const char *path, const mh_update_item_t *item)
{
    char hex[65];
    size_t size = 0;
    if (mh_update_sha256_file(path, hex, &size) != ESP_OK) {
        return false;
    }
    if (item->size != 0 && size != item->size) {
        return false;
    }
    if (item->sha256 && item->sha256[0] && strcasecmp(hex, item->sha256) != 0) {
        return false;
    }
    return true;
}

/* Only the type catalogue (model/) is bulk-synced. Icons (images/) and
 * per-parameter styles (styles/) are fetched per device type -- see
 * sync_used_device_assets() -- so a Hub never downloads the icons of device
 * types it has no way of running. */
static bool asset_item_needed(const mh_update_item_t *item)
{
    const char *url = item->url;
    if (!url) {
        return false;
    }
    if (strncmp(url, "images/", 7) == 0 || strncmp(url, "styles/", 7) == 0) {
        return false;
    }
    return true;
}

static bool item_selected(minihub_update_kind_t kind, const mh_update_index_t *idx,
                          const mh_update_item_t *item, const char *chip, const char *web_ver)
{
    if (kind == MINIHUB_UPDATE_ASSET && !asset_item_needed(item)) {
        return false;
    }
    if (!item->url || !item->url[0] || !mh_update_target_match(item, chip)) {
        return false;
    }
    if (kind == MINIHUB_UPDATE_WEB) {
        if (!web_ver || !item->version || strcmp(item->version, web_ver) != 0) {
            return false;
        }
    }
    if (kind == MINIHUB_UPDATE_ASSET || kind == MINIHUB_UPDATE_WEB) {
        char dest[MH_UPDATE_URL_MAX];
        if (dest_for(kind, item, dest, sizeof(dest)) != ESP_OK) {
            return false;
        }
        for (size_t i = 0; i < idx->nitems; i++) {
            const mh_update_item_t *other = &idx->items[i];
            if (other == item || !mh_update_target_match(other, chip)) {
                continue;
            }
            char other_dest[MH_UPDATE_URL_MAX];
            if (dest_for(kind, other, other_dest, sizeof(other_dest)) != ESP_OK) {
                continue;
            }
            if (strcmp(dest, other_dest) == 0 &&
                mh_update_version_cmp(other->version, item->version) > 0) {
                return false;
            }
        }
    }
    return true;
}

static int count_pending(minihub_update_kind_t kind, const mh_update_index_t *idx, const char *chip, bool force)
{
    if (kind == MINIHUB_UPDATE_FIRMWARE) {
        const mh_update_item_t *best = best_firmware(idx, chip);
        if (!best) {
            return 0;
        }
        return (firmware_needs(best) || force) ? 1 : 0;
    }
    const char *web_ver = (kind == MINIHUB_UPDATE_WEB) ? web_version_target(idx, chip, force) : NULL;
    int pending = 0;
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *item = &idx->items[i];
        if (!item_selected(kind, idx, item, chip, web_ver)) {
            continue;
        }
        char dest[MH_UPDATE_URL_MAX];
        if (dest_for(kind, item, dest, sizeof(dest)) != ESP_OK) {
            return err_rc(ESP_ERR_INVALID_ARG);
        }
        if (!file_matches(dest, item) || force) {
            pending++;
        }
    }
    return pending;
}

static esp_err_t apply_model_reload(const char *dest)
{
    const char *slash = strrchr(dest, '/');
    const char *name = slash ? slash + 1 : dest;
    if (strcmp(name, "devices.csv") != 0 && strcmp(name, "parameters.csv") != 0) {
        return ESP_OK;
    }
    FILE *file = fopen(dest, "rb");
    if (!file) {
        return ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long sz = ftell(file);
    if (sz < 0 || sz > 128 * 1024) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(file);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)sz, file);
    fclose(file);
    if (n != (size_t)sz) {
        free(buf);
        return ESP_FAIL;
    }
    bool rebuilt = false;
    char errbuf[128];
    esp_err_t err = minihub_device_widget_put_model(name, buf, n, &rebuilt, errbuf, sizeof(errbuf));
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "model reload %s: %s", name, errbuf[0] ? errbuf : esp_err_to_name(err));
    }
    return err;
}

static esp_err_t apply_files(minihub_update_kind_t kind, const char *base, const mh_update_index_t *idx,
                             const char *chip, bool force)
{
    const char *web_ver = (kind == MINIHUB_UPDATE_WEB) ? web_version_target(idx, chip, force) : NULL;
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *item = &idx->items[i];
        if (!item_selected(kind, idx, item, chip, web_ver)) {
            continue;
        }
        char dest[MH_UPDATE_URL_MAX];
        esp_err_t err = dest_for(kind, item, dest, sizeof(dest));
        if (err != ESP_OK) {
            return err;
        }
        if (file_matches(dest, item) && !force) {
            continue;
        }
        char url[MH_UPDATE_URL_MAX];
        err = mh_update_join_url(url, sizeof(url), base, idx->base_url, item->url);
        if (err != ESP_OK) {
            return err;
        }
        char *full = minihub_net_url_with_device_queries(url, running_version());
        if (!full) {
            return ESP_ERR_NO_MEM;
        }
        err = mh_update_download_verified(full, dest, item->sha256, item->size);
        free(full);
        if (err != ESP_OK) {
            return err;
        }
        if (kind == MINIHUB_UPDATE_ASSET) {
            err = apply_model_reload(dest);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t apply_firmware(const char *base, const mh_update_item_t *best, bool force)
{
    if (!firmware_needs(best) && !force) {
        return ESP_OK;
    }
    char url[MH_UPDATE_URL_MAX];
    esp_err_t err = mh_update_join_url(url, sizeof(url), base, NULL, best->url);
    if (err != ESP_OK) {
        return err;
    }
    fw_rec_t rec = {
        .present = true,
        .armed = true,
        .size = best->size,
    };
    snprintf(rec.version, sizeof(rec.version), "%s", best->version ? best->version : "");
    snprintf(rec.sha256, sizeof(rec.sha256), "%s", best->sha256 ? best->sha256 : "");
    err = minihub_ota_start_url(url);
    if (err != ESP_OK) {
        return err;
    }
    save_fw_rec(&rec);
    ESP_LOGI(TAG, "firmware OTA started %s", url);
    return ESP_OK;
}

static int run(minihub_update_kind_t kind, const char *base_url, bool apply, bool force)
{
    if (!base_url || !base_url[0]) {
        return err_rc(ESP_ERR_INVALID_ARG);
    }
    if (!lock_take()) {
        return err_rc(ESP_ERR_NO_MEM);
    }
    char chip[16];
    local_chip(chip, sizeof(chip));
    mh_update_index_t idx;
    memset(&idx, 0, sizeof(idx));
    esp_err_t err = fetch_index(kind, base_url, &idx);
    int rc = 0;
    if (err == ESP_OK && kind == MINIHUB_UPDATE_FIRMWARE) {
        cache_running_firmware_note(&idx, chip);
    }
    if (err != ESP_OK) {
        rc = err_rc(err);
    } else if (!apply) {
        rc = count_pending(kind, &idx, chip, force);
    } else if (kind == MINIHUB_UPDATE_FIRMWARE) {
        /* A forced re-install repairs the version that is running; a plain update
         * moves to the newest one. Fall back to newest when the running version is
         * no longer published. */
        const mh_update_item_t *best = NULL;
        if (force) {
            best = running_firmware(&idx, chip);
        }
        if (!best) {
            best = best_firmware(&idx, chip);
        }
        err = (best ? apply_firmware(base_url, best, force) : ESP_OK);
        rc = err_rc(err);
    } else {
        err = apply_files(kind, base_url, &idx, chip, force);
        rc = err_rc(err);
        /* Icons and per-parameter styles are excluded from the bulk pass, so an
         * asset apply must fetch them per in-use device type -- otherwise the
         * console's "更新设备资源" could never repair a missing /data/images. */
        if (kind == MINIHUB_UPDATE_ASSET) {
            sync_used_device_assets(base_url);
        }
    }
    mh_update_index_free(&idx);
    lock_give();
    return rc;
}

int minihub_update_check(minihub_update_kind_t kind, const char *base_url)
{
    return run(kind, base_url, false, false);
}

int minihub_update_apply(minihub_update_kind_t kind, const char *base_url)
{
    return run(kind, base_url, true, false);
}

int minihub_update_apply_force(minihub_update_kind_t kind, const char *base_url, bool force)
{
    return run(kind, base_url, true, force);
}

static SemaphoreHandle_t s_hint_mu;
static cJSON *s_hint_fw;
static cJSON *s_hint_plugins;
static cJSON *s_hint_web;
static cJSON *s_hint_assets;
static char *s_fw_note;

static bool ensure_hint_mu(void)
{
    if (!s_hint_mu) {
        s_hint_mu = xSemaphoreCreateMutex();
    }
    return s_hint_mu != NULL;
}

static bool hint_lock(void)
{
    if (!s_hint_mu) {
        return false;
    }
    return xSemaphoreTake(s_hint_mu, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void hint_unlock(void)
{
    xSemaphoreGive(s_hint_mu);
}

static void hint_store(cJSON *fw, bool fw_ok, cJSON *plugins, bool plugins_ok, bool clear)
{
    if (!hint_lock()) {
        cJSON_Delete(fw);
        cJSON_Delete(plugins);
        return;
    }
    if (clear) {
        cJSON_Delete(s_hint_fw);
        s_hint_fw = NULL;
        cJSON_Delete(s_hint_plugins);
        s_hint_plugins = cJSON_CreateArray();
    } else {
        if (fw_ok) {
            cJSON_Delete(s_hint_fw);
            s_hint_fw = fw;
            fw = NULL;
        }
        if (plugins_ok) {
            cJSON_Delete(s_hint_plugins);
            s_hint_plugins = plugins;
            plugins = NULL;
        }
    }
    hint_unlock();
    cJSON_Delete(fw);
    cJSON_Delete(plugins);
}

static void hint_store_web(cJSON *web, bool ok)
{
    if (!hint_lock()) {
        cJSON_Delete(web);
        return;
    }
    if (ok) {
        cJSON_Delete(s_hint_web);
        s_hint_web = web;
        web = NULL;
    }
    hint_unlock();
    cJSON_Delete(web);
}

static void hint_store_assets(cJSON *assets, bool ok)
{
    if (!hint_lock()) {
        cJSON_Delete(assets);
        return;
    }
    if (ok) {
        cJSON_Delete(s_hint_assets);
        s_hint_assets = assets;
        assets = NULL;
    }
    hint_unlock();
    cJSON_Delete(assets);
}

static void hint_clear_all(void)
{
    if (!hint_lock()) {
        return;
    }
    cJSON_Delete(s_hint_fw);
    s_hint_fw = NULL;
    cJSON_Delete(s_hint_plugins);
    s_hint_plugins = cJSON_CreateArray();
    cJSON_Delete(s_hint_web);
    s_hint_web = NULL;
    cJSON_Delete(s_hint_assets);
    s_hint_assets = NULL;
    hint_unlock();
}

static bool version_text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    if ((a[0] == 'v' || a[0] == 'V') && strcmp(a + 1, b) == 0) {
        return true;
    }
    if ((b[0] == 'v' || b[0] == 'V') && strcmp(a, b + 1) == 0) {
        return true;
    }
    return false;
}

/* The published item whose version equals the one that is running. Used by a
 * forced re-install, which repairs the current version instead of upgrading. */
static const mh_update_item_t *running_firmware(const mh_update_index_t *idx, const char *chip)
{
    const char *cur = running_version();
    if (!cur[0]) {
        return NULL;
    }
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *item = &idx->items[i];
        if (!item->url || !item->url[0] || !item->version) {
            continue;
        }
        if (!firmware_match(item, chip)) {
            continue;
        }
        if (version_text_eq(item->version, cur)) {
            return item;
        }
    }
    return NULL;
}

static void set_cached_fw_note(const char *note)
{
    if (!ensure_hint_mu() || !hint_lock()) {
        return;
    }
    free(s_fw_note);
    s_fw_note = NULL;
    if (note && note[0]) {
        size_t n = strlen(note);
        if (n > MH_UPDATE_NOTE_MAX) {
            n = MH_UPDATE_NOTE_MAX;
        }
        while (n > 0 && (note[n - 1] == '\n' || note[n - 1] == '\r' || note[n - 1] == ' ' ||
                         note[n - 1] == '\t')) {
            n--;
        }
        s_fw_note = malloc(n + 1);
        if (s_fw_note) {
            memcpy(s_fw_note, note, n);
            s_fw_note[n] = '\0';
        }
    } else {
        s_fw_note = calloc(1, 1);
    }
    hint_unlock();
}

static void cache_running_firmware_note(const mh_update_index_t *idx, const char *chip)
{
    const char *cur = running_version();
    const char *note = "";
    if (cur[0] && idx) {
        for (size_t i = 0; i < idx->nitems; i++) {
            const mh_update_item_t *item = &idx->items[i];
            if (!firmware_match(item, chip) || !item->version) {
                continue;
            }
            if (!version_text_eq(item->version, cur)) {
                continue;
            }
            if (item->release_note && item->release_note[0]) {
                note = item->release_note;
            }
            break;
        }
    }
    set_cached_fw_note(note);
}

static char *read_text_capped(const char *path, size_t cap, bool first_line)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    char *buf = calloc(1, cap + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    size_t n = fread(buf, 1, cap, file);
    fclose(file);
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 3);
        n -= 3;
    }
    buf[n] = '\0';
    if (first_line) {
        char *nl = strpbrk(buf, "\r\n");
        if (nl) {
            *nl = '\0';
        }
    }
    char *start = buf;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != buf) {
        memmove(buf, start, strlen(start) + 1);
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ' ||
                       buf[len - 1] == '\t')) {
        buf[--len] = '\0';
    }
    if (buf[0] == '\0') {
        free(buf);
        return NULL;
    }
    return buf;
}

static char *read_web_named(const char *name, size_t cap, bool first_line)
{
    char path[96];
    const char *roots[] = {MINIHUB_WEB_DIR, MINIHUB_PREINSTALL_WEB_DIR};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        int wrote = snprintf(path, sizeof(path), "%s/%s", roots[i], name);
        if (wrote < 0 || (size_t)wrote >= sizeof(path)) {
            continue;
        }
        char *text = read_text_capped(path, cap, first_line);
        if (text) {
            return text;
        }
    }
    return NULL;
}

static esp_err_t probe_firmware_hint(const char *base, cJSON **out)
{
    *out = NULL;
    if (!lock_take()) {
        return ESP_ERR_TIMEOUT;
    }
    char chip[16];
    local_chip(chip, sizeof(chip));
    mh_update_index_t idx;
    memset(&idx, 0, sizeof(idx));
    esp_err_t err = fetch_index(MINIHUB_UPDATE_FIRMWARE, base, &idx);
    if (err == ESP_OK) {
        cache_running_firmware_note(&idx, chip);
        const mh_update_item_t *best = best_firmware(&idx, chip);
        if (best && firmware_needs(best)) {
            cJSON *obj = cJSON_CreateObject();
            if (!obj) {
                err = ESP_ERR_NO_MEM;
            } else {
                cJSON_AddStringToObject(obj, "version", best->version ? best->version : "");
                cJSON_AddStringToObject(obj, "current", running_version());
                if (best->release_note) {
                    cJSON_AddStringToObject(obj, "note", best->release_note);
                }
                *out = obj;
            }
        }
    }
    mh_update_index_free(&idx);
    lock_give();
    return err;
}

/* Deploy folders are named e.g. "v0.1.0" while the bundle's own version.txt
 * (and the baked-in window.mhWebVersion, and CONFIG_MINIHUB_VERSION) carries
 * no prefix. Ignore a leading "v"/"V" so the same release is not reported as
 * pending forever -- and so "v0.1.1" is not ranked below "0.1.0". */
static const char *ver_norm(const char *v)
{
    if (v && (v[0] == 'v' || v[0] == 'V') && v[1] >= '0' && v[1] <= '9') {
        return v + 1;
    }
    return v ? v : "";
}

/* Which published web version to apply: a forced re-install re-applies the
 * version that is running (repair), a plain update takes the newest one. Falls
 * back to newest when the running version is no longer published. */
static const char *web_version_target(const mh_update_index_t *idx, const char *chip, bool force)
{
    const char *latest = web_version_max(idx, chip);
    if (!force || !latest) {
        return latest;
    }
    char *cur = read_web_named("version.txt", 64, true);
    if (!cur) {
        return latest;
    }
    const char *pick = latest;
    for (size_t i = 0; i < idx->nitems; i++) {
        const mh_update_item_t *it = &idx->items[i];
        if (mh_update_target_match(it, chip) && it->version &&
            strcmp(ver_norm(it->version), ver_norm(cur)) == 0) {
            pick = it->version;
            break;
        }
    }
    free(cur);
    return pick;
}

static void probe_web_hint(const char *base, cJSON **out, bool *ok)
{
    *out = NULL;
    *ok = false;
    if (!lock_take()) {
        return;
    }
    char chip[16];
    local_chip(chip, sizeof(chip));
    mh_update_index_t idx;
    memset(&idx, 0, sizeof(idx));
    esp_err_t err = fetch_index(MINIHUB_UPDATE_WEB, base, &idx);
    if (err == ESP_OK) {
        const char *latest = web_version_max(&idx, chip);
        char *cur = read_web_named("version.txt", 64, true);
        if (latest && strcmp(ver_norm(latest), ver_norm(cur)) != 0) {
            cJSON *obj = cJSON_CreateObject();
            if (obj) {
                cJSON_AddStringToObject(obj, "version", latest);
                cJSON_AddStringToObject(obj, "current", cur ? cur : "");
                for (size_t i = 0; i < idx.nitems; i++) {
                    const mh_update_item_t *it = &idx.items[i];
                    if (mh_update_target_match(it, chip) && it->version &&
                        strcmp(it->version, latest) == 0 && it->release_note) {
                        cJSON_AddStringToObject(obj, "note", it->release_note);
                        break;
                    }
                }
                *out = obj;
            }
        }
        free(cur);
    }
    mh_update_index_free(&idx);
    lock_give();
    *ok = (err == ESP_OK);
}

static void hint_check_once(void)
{
    char base[MINIHUB_STORE_BASE_URL_MAX];
    if (!minihub_store_copy_base_url(base, sizeof(base))) {
        ESP_LOGW(TAG, "skip update hint: no deploy server URL");
        hint_clear_all();
        set_cached_fw_note("");
        return;
    }
    cJSON *fw = NULL;
    esp_err_t ferr = probe_firmware_hint(base, &fw);
    cJSON *plugins = NULL;
    esp_err_t perr = minihub_store_list_upgrades(&plugins);
    cJSON *web = NULL;
    bool wok = false;
    probe_web_hint(base, &web, &wok);
    int apending = minihub_update_check(MINIHUB_UPDATE_ASSET, base);
    bool aok = (apending >= 0);
    cJSON *assets = NULL;
    if (aok && apending > 0) {
        assets = cJSON_CreateObject();
        cJSON_AddNumberToObject(assets, "pending", apending);
    }
    if (ferr != ESP_OK && perr != ESP_OK && !wok && !aok) {
        ESP_LOGW(TAG, "update hint check failed: firmware %s, plugins %s, web %s, assets %s",
                 esp_err_to_name(ferr), esp_err_to_name(perr),
                 wok ? "ok" : "err", aok ? "ok" : "err");
        cJSON_Delete(fw);
        cJSON_Delete(plugins);
        cJSON_Delete(web);
        cJSON_Delete(assets);
        return;
    }
    if (ferr != ESP_OK) {
        ESP_LOGW(TAG, "firmware hint check failed: %s", esp_err_to_name(ferr));
    }
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "plugin hint check failed: %s", esp_err_to_name(perr));
    }
    if (!wok) {
        ESP_LOGW(TAG, "web hint check failed");
    }
    if (!aok) {
        ESP_LOGW(TAG, "asset hint check failed");
    }
    int nplug = (perr == ESP_OK && plugins) ? cJSON_GetArraySize(plugins) : -1;
    int npending = (aok && assets) ? cJSON_GetObjectItem(assets, "pending")->valueint : 0;
    ESP_LOGI(TAG, "update hint: firmware %s, plugins %d, web %s, assets %d",
             (ferr == ESP_OK && fw) ? "yes" : (ferr == ESP_OK ? "no" : "err"), nplug,
             (wok && web) ? "yes" : (wok ? "no" : "err"), npending);
    hint_store(fw, ferr == ESP_OK, plugins, perr == ESP_OK, false);
    hint_store_web(web, wok);
    hint_store_assets(assets, aok);
}

/* Fetch the avatar of every device type this Hub actually has and cache it under
 * /data/images/devices/. The file name is <type>.svg by convention, so this needs
 * no metadata request -- and, unlike the model-driven route, it also works for a
 * plugin-owned type. Icons of types the Hub cannot run (speaker, sensor, ...) are
 * never requested. */
static void sync_used_device_assets(const char *base)
{
    minihub_point_t *points = NULL;
    size_t n = 0;
    if (minihub_point_list(&points, &n) != ESP_OK || !points || n == 0) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const char *type = points[i].type;
        if (!type[0]) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(points[j].type, type) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        esp_err_t err = minihub_update_fetch_device_icon(base, type);
        if (err != ESP_OK) {
            /* 404 simply means this type ships no artwork: the glyph fallback
             * covers it, and asking again next hour is cheap. */
            ESP_LOGD(TAG, "asset sync: no icon for type '%s' (%s)", type, esp_err_to_name(err));
        }
    }
    minihub_point_list_free(points, n);
}

static void asset_sync_once(void)
{
    char base[MINIHUB_STORE_BASE_URL_MAX];
    if (!minihub_store_copy_base_url(base, sizeof(base))) {
        return;
    }
    /* Bulk part: the type catalogue (the CSV files under model/). Icons and styles
     * are NOT bulk-synced -- they are pulled per device type below, so only the
     * types this Hub runs are fetched. Runs hourly; no-op when the local model
     * already matches the index. */
    int pending = minihub_update_check(MINIHUB_UPDATE_ASSET, base);
    if (pending > 0) {
        ESP_LOGI(TAG, "asset sync: %d item(s) pending, applying", pending);
        minihub_update_apply(MINIHUB_UPDATE_ASSET, base);
    }
    sync_used_device_assets(base);
}

/* One-shot asset sync triggered when a store module is installed/upgraded.
 * Runs on its own task so the core_task event dispatcher (which invokes the
 * subscriber) is never blocked by network downloads. Guarded by a running flag
 * to avoid stacking redundant syncs on top of the hourly pass. */
static bool s_asset_sync_running;

static void asset_sync_task(void *arg)
{
    (void)arg;
    s_asset_sync_running = true;
    asset_sync_once();
    s_asset_sync_running = false;
    vTaskDelete(NULL);
}

static void asset_sync_trigger(void)
{
    if (s_asset_sync_running) {
        return;
    }
    if (xTaskCreate(asset_sync_task, "mh_asset_sync", 16384, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "asset sync task create failed");
    }
}

static void on_store_module_installed(const minihub_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    asset_sync_trigger();
}

/* The install event fires BEFORE the hub reload that actually registers the new
 * adapter's points, and that reload is asynchronous. Syncing there races it: the
 * point list can be empty (or missing the new type) at that moment, so
 * sync_used_device_assets() found nothing to fetch and the freshly installed
 * type got no icon until the next hourly pass.
 *
 * hub.reloaded is published once the reload has finished, so the point set is
 * final and this is the correct moment to pull the icons it needs. */
static void on_hub_reloaded(const minihub_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    asset_sync_trigger();
}

/* Single writer for the hint cache: the web never probes on its own task, it
 * notifies this one. `busy` spans periodic and manual probes alike, so
 * GET /api/update/check cannot report idle while a probe is in flight, and
 * `want` covers a request that has not been served yet -- notably while there
 * is no IP, when the probe cannot run at all: reporting idle there would let
 * the console toast "checked" against a cache nobody refreshed. */
static TaskHandle_t s_hint_task;
static volatile bool s_hint_refresh_busy;
static volatile bool s_hint_refresh_want;

static void hint_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!minihub_net_has_ip()) {
            /* Leave s_hint_refresh_want set: the request is still owed. */
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MH_UPDATE_HINT_WAIT_IP_MS));
            continue;
        }
        s_hint_refresh_busy = true;
        hint_check_once();
        /* Hints are written; the caller's wait ends here. Asset sync (catalog
         * apply + per-type icon pulls) can run for a while and must not keep
         * the console polling on a cache that is already current. */
        s_hint_refresh_busy = false;
        s_hint_refresh_want = false;
        asset_sync_once();
        /* Sleep out the period, or wake immediately when the console asks. */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(MH_UPDATE_HINT_PERIOD_MS));
    }
}

void minihub_update_hint_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    if (!ensure_hint_mu()) {
        ESP_LOGE(TAG, "update hint mutex create failed");
        return;
    }
    started = true;
    (void)minihub_event_bus_subscribe("store.module.installed", on_store_module_installed, NULL);
    (void)minihub_event_bus_subscribe("hub.reloaded", on_hub_reloaded, NULL);
    if (xTaskCreate(hint_task, "mh_upd_hint", 16384, NULL, 3, &s_hint_task) != pdPASS) {
        started = false;
        ESP_LOGE(TAG, "update hint task create failed");
    }
}

cJSON *minihub_update_hints_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    if (!hint_lock()) {
        cJSON_AddNullToObject(root, "firmware");
        cJSON_AddItemToObject(root, "plugins", cJSON_CreateArray());
        return root;
    }
    if (s_hint_fw) {
        cJSON *copy = cJSON_Duplicate(s_hint_fw, true);
        if (copy) {
            cJSON_AddItemToObject(root, "firmware", copy);
        } else {
            cJSON_AddNullToObject(root, "firmware");
        }
    } else {
        cJSON_AddNullToObject(root, "firmware");
    }
    cJSON *plugins = s_hint_plugins ? cJSON_Duplicate(s_hint_plugins, true) : cJSON_CreateArray();
    cJSON_AddItemToObject(root, "plugins", plugins ? plugins : cJSON_CreateArray());
    if (s_hint_web) {
        cJSON *copy = cJSON_Duplicate(s_hint_web, true);
        cJSON_AddItemToObject(root, "web", copy ? copy : cJSON_CreateNull());
    } else {
        cJSON_AddItemToObject(root, "web", cJSON_CreateNull());
    }
    if (s_hint_assets) {
        cJSON *copy = cJSON_Duplicate(s_hint_assets, true);
        cJSON_AddItemToObject(root, "assets", copy ? copy : cJSON_CreateNull());
    } else {
        cJSON_AddItemToObject(root, "assets", cJSON_CreateNull());
    }
    hint_unlock();
    return root;
}

cJSON *minihub_update_notes_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "firmware_version", running_version());
    char *fw_note = NULL;
    if (s_hint_mu && hint_lock()) {
        if (s_fw_note && s_fw_note[0]) {
            fw_note = strdup(s_fw_note);
        }
        hint_unlock();
    }
    cJSON_AddStringToObject(root, "firmware_note", fw_note ? fw_note : "");
    free(fw_note);

    char *web_ver = read_web_named("version.txt", 64, true);
    char *web_note = read_web_named("release_notes.txt", MH_UPDATE_NOTE_MAX, false);
    cJSON_AddStringToObject(root, "web_version", web_ver ? web_ver : "");
    cJSON_AddStringToObject(root, "web_note", web_note ? web_note : "");
    free(web_ver);
    free(web_note);
    return root;
}

/* Ask the resident hint task to probe now instead of running the probes here or
 * spawning a second task: hint_check_once() does blocking network I/O (firmware /
 * catalog / web / asset probes) and must never run on the httpd task, and two
 * concurrent probes would interleave writes into the hint cache. A request that
 * lands while a probe is running simply queues one more pass.
 * ESP_ERR_INVALID_STATE means the hint subsystem never started. */
esp_err_t minihub_update_hints_refresh_async(void)
{
    if (!s_hint_task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_hint_refresh_want = true;
    xTaskNotifyGive(s_hint_task);
    return ESP_OK;
}

void minihub_update_hints_refresh_status(bool *busy)
{
    if (busy) {
        *busy = s_hint_refresh_busy || s_hint_refresh_want;
    }
}
