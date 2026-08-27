/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_config.h"
#include "minihub_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "sdkconfig.h"

#ifndef ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS ((esp_partition_subtype_t)0x83)
#endif

static const char *TAG = "minihub_config";

static cJSON *s_adapters_cfg;
static cJSON *s_rules_cfg;
static cJSON *s_scenes_cfg;

static char s_data_root[32];
static char s_config_dir[64];
static char s_adapters_path[96];
static char s_rules_path[96];
static char s_scenes_path[96];
static char s_packs_dir[80];
static char s_tmp_dir[80];

static bool dir_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void resolve_config_paths(void)
{
    const char *root = MINIHUB_VFS_MOUNT;
    /* claw_minihub mounts FAT `storage` at /fatfs; Host MiniHub uses LittleFS /data. */
    if (!dir_exists(MINIHUB_VFS_MOUNT) && dir_exists("/fatfs")) {
        root = "/fatfs";
        ESP_LOGI(TAG, "config root %s (no %s mount)", root, MINIHUB_VFS_MOUNT);
    }
    strlcpy(s_data_root, root, sizeof(s_data_root));
    snprintf(s_config_dir, sizeof(s_config_dir), "%s/config", root);
    snprintf(s_adapters_path, sizeof(s_adapters_path), "%s/adapters.json", s_config_dir);
    snprintf(s_rules_path, sizeof(s_rules_path), "%s/rules.json", s_config_dir);
    snprintf(s_scenes_path, sizeof(s_scenes_path), "%s/scenes.json", s_config_dir);
    snprintf(s_packs_dir, sizeof(s_packs_dir), "%s/packs", s_config_dir);
    snprintf(s_tmp_dir, sizeof(s_tmp_dir), "%s/tmp", root);
}

const char *minihub_paths_data_root(void)
{
    if (!s_data_root[0]) {
        resolve_config_paths();
    }
    return s_data_root[0] ? s_data_root : MINIHUB_VFS_MOUNT;
}

const char *minihub_paths_module_tmp_dir(void)
{
    if (!s_tmp_dir[0]) {
        resolve_config_paths();
    }
    return s_tmp_dir[0] ? s_tmp_dir : MINIHUB_MODULE_TMP_DIR;
}

const char *minihub_paths_packs_dir(void)
{
    if (!s_packs_dir[0]) {
        resolve_config_paths();
    }
    return s_packs_dir[0] ? s_packs_dir : MINIHUB_PACKS_DIR;
}

/**
 * Legacy adapters used key "thread" for Matter devices.
 * Migrate into matter and drop the unused thread key (adapters.json is
 * for real adapter control only — no reserved stubs / comments).
 */
static bool migrate_thread_to_matter(cJSON *adapters_root)
{
    cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!cJSON_IsObject(adapters)) {
        return false;
    }

    cJSON *legacy = cJSON_GetObjectItem(adapters, "thread");
    cJSON *matter = cJSON_GetObjectItem(adapters, "matter");
    if (!legacy) {
        return false;
    }

    const cJSON *legacy_devices = cJSON_GetObjectItem(legacy, "devices");
    const bool legacy_has_devices = cJSON_IsArray(legacy_devices) && cJSON_GetArraySize(legacy_devices) > 0;
    const bool legacy_enabled = cJSON_IsTrue(cJSON_GetObjectItem(legacy, "enabled"));
    bool changed = false;

    if (!matter) {
        matter = cJSON_CreateObject();
        if (!matter) {
            return false;
        }
        cJSON_AddItemToObject(adapters, "matter", matter);
        cJSON_AddBoolToObject(matter, "enabled", legacy_enabled);
        if (legacy_has_devices) {
            cJSON *dup = cJSON_Duplicate(legacy_devices, true);
            if (dup) {
                cJSON_AddItemToObject(matter, "devices", dup);
            } else {
                cJSON_AddItemToObject(matter, "devices", cJSON_CreateArray());
            }
        } else {
            cJSON_AddItemToObject(matter, "devices", cJSON_CreateArray());
        }
        changed = true;
    } else {
        cJSON *matter_devices = cJSON_GetObjectItem(matter, "devices");
        const bool matter_empty =
            !cJSON_IsArray(matter_devices) || cJSON_GetArraySize(matter_devices) == 0;
        if (matter_empty && legacy_has_devices) {
            cJSON_DeleteItemFromObject(matter, "devices");
            cJSON *dup = cJSON_Duplicate(legacy_devices, true);
            if (dup) {
                cJSON_AddItemToObject(matter, "devices", dup);
            }
            changed = true;
        }
        if (cJSON_HasObjectItem(matter, "comment")) {
            cJSON_DeleteItemFromObject(matter, "comment");
            changed = true;
        }
    }

    cJSON_DeleteItemFromObject(adapters, "thread");
    changed = true;
    return changed;
}

static const char *DEFAULT_ADAPTERS = "{\"adapters\":{}}";

static const char *DEFAULT_RULES = "{\"rules\":[]}";

static const char *DEFAULT_SCENES = "{\"scenes\":[]}";

static cJSON *read_json_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static cJSON *load_or_default(const char *path, const char *fallback)
{
    cJSON *json = read_json_file(path);
    if (json) {
        ESP_LOGI(TAG, "loaded %s", path);
        return json;
    }
    ESP_LOGW(TAG, "using embedded default for %s", path);
    return cJSON_Parse(fallback);
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0755) != 0) {
        ESP_LOGW(TAG, "could not create %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool partition_is_littlefs(const char *label)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!part) {
        return false;
    }
    return part->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS;
}

static esp_err_t mount_littlefs_partition(const char *label, const char *base_path, const char *tag)
{
    if (!partition_is_littlefs(label)) {
        ESP_LOGW(TAG, "skip LittleFS on '%s' (not a littlefs partition)", label);
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = label,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s mount failed: %s", tag, esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_info(label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s mounted at %s: %u / %u bytes used", tag, base_path, (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

static esp_err_t mount_littlefs(void)
{
    esp_err_t err = mount_littlefs_partition("storage", MINIHUB_VFS_MOUNT, "storage");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "config LittleFS unavailable, using embedded defaults until a writable root is found");
    }

#if CONFIG_MINIHUB_DYNAMIC_MODULES
    err = mount_littlefs_partition("modstore", MINIHUB_STORE_MOUNT, "modstore");
    if (err == ESP_OK) {
        ensure_dir(MINIHUB_STORE_MODULES_DIR);
        ensure_dir(MINIHUB_STORE_PACKS_DIR);
    } else {
        ESP_LOGW(TAG, "modstore partition not mounted; store downloads disabled");
    }
#endif

    resolve_config_paths();
    ensure_dir(s_config_dir);
    ensure_dir(s_packs_dir);
    ensure_dir(s_tmp_dir);
    return ESP_OK;
}

esp_err_t minihub_config_init(void)
{
    mount_littlefs();

    if (s_adapters_cfg) {
        cJSON_Delete(s_adapters_cfg);
    }
    if (s_rules_cfg) {
        cJSON_Delete(s_rules_cfg);
    }
    if (s_scenes_cfg) {
        cJSON_Delete(s_scenes_cfg);
    }

    s_adapters_cfg = load_or_default(s_adapters_path, DEFAULT_ADAPTERS);
    s_rules_cfg = load_or_default(s_rules_path, DEFAULT_RULES);
    s_scenes_cfg = load_or_default(s_scenes_path, DEFAULT_SCENES);

    if (!s_adapters_cfg || !s_rules_cfg || !s_scenes_cfg) {
        return ESP_FAIL;
    }
    if (migrate_thread_to_matter(s_adapters_cfg)) {
        esp_err_t mig_err = minihub_config_save_adapters(s_adapters_cfg);
        if (mig_err != ESP_OK) {
            ESP_LOGW(TAG, "matter/thread migrate save failed: %s", esp_err_to_name(mig_err));
        } else {
            ESP_LOGI(TAG, "migrated legacy adapters.thread → adapters.matter");
        }
    }
    return ESP_OK;
}

cJSON *minihub_config_get_adapters(void)
{
    return s_adapters_cfg;
}

cJSON *minihub_config_get_rules(void)
{
    return s_rules_cfg;
}

cJSON *minihub_config_get_scenes(void)
{
    return s_scenes_cfg;
}

esp_err_t minihub_config_reload(void)
{
    cJSON_Delete(s_adapters_cfg);
    cJSON_Delete(s_rules_cfg);
    cJSON_Delete(s_scenes_cfg);
    s_adapters_cfg = load_or_default(s_adapters_path, DEFAULT_ADAPTERS);
    s_rules_cfg = load_or_default(s_rules_path, DEFAULT_RULES);
    s_scenes_cfg = load_or_default(s_scenes_path, DEFAULT_SCENES);
    if (!s_adapters_cfg || !s_rules_cfg || !s_scenes_cfg) {
        return ESP_FAIL;
    }
    if (migrate_thread_to_matter(s_adapters_cfg)) {
        (void)minihub_config_save_adapters(s_adapters_cfg);
    }
    return ESP_OK;
}

esp_err_t minihub_config_save_adapters(cJSON *adapters_root)
{
    if (!adapters_root) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_Print(adapters_root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(s_adapters_path, "w");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "cannot write %s (errno=%d)", s_adapters_path, errno);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    /*
     * Keep the live tree when the caller saved s_adapters_cfg in place.
     * Delete+reload here races every holder of minihub_config_get_adapters()
     * (module sync → soft reload → cJSON_GetObjectItem UAF / Load access fault).
     */
    if (adapters_root == s_adapters_cfg) {
        ESP_LOGI(TAG, "saved %s", s_adapters_path);
        return ESP_OK;
    }

    if (s_adapters_cfg) {
        cJSON_Delete(s_adapters_cfg);
    }
    s_adapters_cfg = load_or_default(s_adapters_path, DEFAULT_ADAPTERS);
    if (!s_adapters_cfg) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s", s_adapters_path);
    return ESP_OK;
}

esp_err_t minihub_config_remove_adapter(const char *adapter_name)
{
    if (!adapter_name || !adapter_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_adapters_cfg) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *adapters = cJSON_GetObjectItem(s_adapters_cfg, "adapters");
    if (!cJSON_IsObject(adapters)) {
        return ESP_OK;
    }
    cJSON *slot = cJSON_GetObjectItem(adapters, adapter_name);
    if (!slot) {
        return ESP_OK;
    }
    cJSON_DeleteItemFromObject(adapters, adapter_name);
    esp_err_t err = minihub_config_save_adapters(s_adapters_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "removed adapters.json slot '%s'", adapter_name);
    }
    return err;
}

esp_err_t minihub_config_save_rules(cJSON *rules_root)
{
    if (!rules_root) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_Print(rules_root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(s_rules_path, "w");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "cannot write %s", s_rules_path);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    if (rules_root == s_rules_cfg) {
        ESP_LOGI(TAG, "saved %s", s_rules_path);
        return ESP_OK;
    }

    if (s_rules_cfg) {
        cJSON_Delete(s_rules_cfg);
    }
    s_rules_cfg = load_or_default(s_rules_path, DEFAULT_RULES);
    if (!s_rules_cfg) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s", s_rules_path);
    return ESP_OK;
}

esp_err_t minihub_config_save_scenes(cJSON *scenes_root)
{
    if (!scenes_root) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = cJSON_Print(scenes_root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(s_scenes_path, "w");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "cannot write %s", s_scenes_path);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    if (scenes_root == s_scenes_cfg) {
        ESP_LOGI(TAG, "saved %s", s_scenes_path);
        return ESP_OK;
    }

    if (s_scenes_cfg) {
        cJSON_Delete(s_scenes_cfg);
    }
    s_scenes_cfg = load_or_default(s_scenes_path, DEFAULT_SCENES);
    if (!s_scenes_cfg) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved %s", s_scenes_path);
    return ESP_OK;
}

static bool rel_path_is_safe(const char *rel_path)
{
    if (rel_path == NULL || rel_path[0] == '\0' || rel_path[0] == '/') {
        return false;
    }
    if (strstr(rel_path, "..") != NULL) {
        return false;
    }
    return true;
}

static esp_err_t mkdir_parents(const char *file_path)
{
    char dir[256];
    size_t len = strnlen(file_path, sizeof(dir) - 1);
    if (len == 0 || len >= sizeof(dir)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dir, file_path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (dir[i] != '/') {
            continue;
        }
        dir[i] = '\0';
        if (dir[0] != '\0' && strcmp(dir, MINIHUB_VFS_MOUNT) != 0) {
            if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir %s failed: %d", dir, errno);
                return ESP_FAIL;
            }
        }
        dir[i] = '/';
    }
    return ESP_OK;
}

esp_err_t minihub_fs_write_file(const char *rel_path, const void *data, size_t len)
{
    if (!rel_path_is_safe(rel_path) || (len > 0 && data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", MINIHUB_VFS_MOUNT, rel_path);
    if (n < 0 || n >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = mkdir_parents(path);
    if (err != ESP_OK) {
        return err;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    if (len > 0) {
        size_t written = fwrite(data, 1, len, f);
        if (written != len) {
            fclose(f);
            ESP_LOGE(TAG, "short write %s (%u/%u)", path, (unsigned)written, (unsigned)len);
            return ESP_FAIL;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "wrote %s (%u bytes)", path, (unsigned)len);
    return ESP_OK;
}
