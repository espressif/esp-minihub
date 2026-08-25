/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_pack.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "minihub_paths.h"
#include "esp_log.h"

static const char *TAG = "minihub_pack";
static const char *PACK_DIR = MINIHUB_PACKS_DIR;

#define MAX_PACKS 16

typedef struct {
    char id[32];
    char vendor[32];
    char name[64];
    char version[16];
    char inject_adapter[16];
    char path[320];
    bool enabled;
    cJSON *raw;
} pack_entry_t;

static pack_entry_t s_packs[MAX_PACKS];
static size_t s_pack_count;

static void clear_packs(void)
{
    for (size_t i = 0; i < s_pack_count; i++) {
        if (s_packs[i].raw) {
            cJSON_Delete(s_packs[i].raw);
        }
    }
    memset(s_packs, 0, sizeof(s_packs));
    s_pack_count = 0;
}

static esp_err_t load_pack_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 16384) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "invalid pack JSON: %s", path);
        return ESP_FAIL;
    }

    if (s_pack_count >= MAX_PACKS) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    pack_entry_t *entry = &s_packs[s_pack_count];
    entry->raw = root;
    entry->enabled = true;
    strncpy(entry->path, path, sizeof(entry->path) - 1);

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *vendor = cJSON_GetObjectItem(root, "vendor");
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *inject = cJSON_GetObjectItem(root, "inject");

    if (cJSON_IsString(id)) {
        strncpy(entry->id, id->valuestring, sizeof(entry->id) - 1);
    }
    if (cJSON_IsString(vendor)) {
        strncpy(entry->vendor, vendor->valuestring, sizeof(entry->vendor) - 1);
    }
    if (cJSON_IsString(name)) {
        strncpy(entry->name, name->valuestring, sizeof(entry->name) - 1);
    }
    if (cJSON_IsString(version)) {
        strncpy(entry->version, version->valuestring, sizeof(entry->version) - 1);
    }
    if (entry->version[0] == '\0') {
        strncpy(entry->version, "0.0.0", sizeof(entry->version) - 1);
    }
    if (cJSON_IsBool(enabled)) {
        entry->enabled = cJSON_IsTrue(enabled);
    }
    if (cJSON_IsObject(inject)) {
        const cJSON *adapter = cJSON_GetObjectItem(inject, "adapter");
        if (cJSON_IsString(adapter)) {
            strncpy(entry->inject_adapter, adapter->valuestring, sizeof(entry->inject_adapter) - 1);
        }
    }

    s_pack_count++;
    ESP_LOGI(TAG, "loaded pack '%s' (%s) -> adapter %s", entry->name, entry->id, entry->inject_adapter);
    return ESP_OK;
}

static esp_err_t scan_pack_directory(void)
{
    DIR *dir = opendir(PACK_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "pack dir %s not found, skip", PACK_DIR);
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t name_len = strlen(ent->d_name);
        if (name_len < 6 || strcmp(ent->d_name + name_len - 5, ".json") != 0) {
            continue;
        }
        char path[320];
        if (strlen(PACK_DIR) + 1 + name_len >= sizeof(path)) {
            ESP_LOGW(TAG, "pack path too long: %s", ent->d_name);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", PACK_DIR, ent->d_name);
        load_pack_file(path);
    }
    closedir(dir);
    return ESP_OK;
}

esp_err_t minihub_pack_init(void)
{
    clear_packs();
    return scan_pack_directory();
}

size_t minihub_pack_count(void)
{
    return s_pack_count;
}

cJSON *minihub_pack_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_pack_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", s_packs[i].id);
        cJSON_AddStringToObject(item, "vendor", s_packs[i].vendor);
        cJSON_AddStringToObject(item, "name", s_packs[i].name);
        cJSON_AddStringToObject(item, "version", s_packs[i].version[0] ? s_packs[i].version : "0.0.0");
        cJSON_AddStringToObject(item, "inject_adapter", s_packs[i].inject_adapter);
        cJSON_AddBoolToObject(item, "enabled", s_packs[i].enabled);
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}

static pack_entry_t *find_pack(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_pack_count; i++) {
        if (strcmp(s_packs[i].id, id) == 0) {
            return &s_packs[i];
        }
    }
    return NULL;
}

static esp_err_t save_pack_entry(pack_entry_t *entry)
{
    if (!entry || !entry->raw || entry->path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *enabled = cJSON_GetObjectItem(entry->raw, "enabled");
    if (enabled) {
        cJSON_ReplaceItemInObject(entry->raw, "enabled", cJSON_CreateBool(entry->enabled));
    } else {
        cJSON_AddBoolToObject(entry->raw, "enabled", entry->enabled);
    }

    char *json = cJSON_Print(entry->raw);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(entry->path, "w");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "cannot write %s", entry->path);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    ESP_LOGI(TAG, "saved pack '%s' enabled=%d", entry->id, entry->enabled);
    return ESP_OK;
}

esp_err_t minihub_pack_set_enabled(const char *id, bool enabled)
{
    pack_entry_t *entry = find_pack(id);
    if (!entry) {
        return ESP_ERR_NOT_FOUND;
    }
    entry->enabled = enabled;
    return save_pack_entry(entry);
}

esp_err_t minihub_pack_delete(const char *id)
{
    pack_entry_t *entry = find_pack(id);
    if (!entry) {
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->path[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (unlink(entry->path) != 0) {
        ESP_LOGE(TAG, "cannot delete %s", entry->path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "deleted pack '%s'", entry->id);
    return minihub_pack_init();
}

static esp_err_t merge_devices(cJSON *adapter_cfg, const cJSON *devices)
{
    if (!cJSON_IsArray(devices)) {
        return ESP_OK;
    }

    cJSON *dst = cJSON_GetObjectItem(adapter_cfg, "devices");
    if (!dst) {
        dst = cJSON_CreateArray();
        cJSON_AddItemToObject(adapter_cfg, "devices", dst);
    } else if (!cJSON_IsArray(dst)) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        cJSON_AddItemToArray(dst, cJSON_Duplicate(item, true));
    }
    return ESP_OK;
}

static esp_err_t merge_json_array_append(cJSON *dst_obj, const char *key, const cJSON *src_arr)
{
    if (!cJSON_IsArray(src_arr)) {
        return ESP_OK;
    }
    cJSON *dst = cJSON_GetObjectItem(dst_obj, key);
    if (!dst) {
        dst = cJSON_CreateArray();
        if (!dst) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(dst_obj, key, dst);
    } else if (!cJSON_IsArray(dst)) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, src_arr) {
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(dst, dup);
    }
    return ESP_OK;
}

/** Deep-merge provision: append uuid_filters / bind_models; other keys replace. */
static esp_err_t merge_provision(cJSON *adapter_cfg, const cJSON *prov_src)
{
    if (!cJSON_IsObject(prov_src)) {
        return ESP_OK;
    }
    cJSON *dst = cJSON_GetObjectItem(adapter_cfg, "provision");
    if (!dst) {
        cJSON *dup = cJSON_Duplicate(prov_src, true);
        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(adapter_cfg, "provision", dup);
        return ESP_OK;
    }
    if (!cJSON_IsObject(dst)) {
        return ESP_ERR_INVALID_STATE;
    }

    const cJSON *filters = cJSON_GetObjectItem(prov_src, "uuid_filters");
    if (filters) {
        esp_err_t err = merge_json_array_append(dst, "uuid_filters", filters);
        if (err != ESP_OK) {
            return err;
        }
    }
    const cJSON *binds = cJSON_GetObjectItem(prov_src, "bind_models");
    if (binds) {
        esp_err_t err = merge_json_array_append(dst, "bind_models", binds);
        if (err != ESP_OK) {
            return err;
        }
    }

    const cJSON *child = prov_src->child;
    while (child) {
        if (child->string &&
            strcmp(child->string, "uuid_filters") != 0 &&
            strcmp(child->string, "bind_models") != 0) {
            cJSON *dup = cJSON_Duplicate(child, true);
            if (!dup) {
                return ESP_ERR_NO_MEM;
            }
            cJSON *existing = cJSON_GetObjectItem(dst, child->string);
            if (existing) {
                cJSON_ReplaceItemInObject(dst, child->string, dup);
            } else {
                cJSON_AddItemToObject(dst, child->string, dup);
            }
        }
        child = child->next;
    }
    return ESP_OK;
}

static esp_err_t merge_adapter_fields(cJSON *adapter_cfg, const cJSON *fields)
{
    if (!cJSON_IsObject(fields)) {
        return ESP_OK;
    }

    const cJSON *devices = cJSON_GetObjectItem(fields, "devices");
    if (devices) {
        merge_devices(adapter_cfg, devices);
    }

    const cJSON *provision = cJSON_GetObjectItem(fields, "provision");
    if (provision) {
        esp_err_t err = merge_provision(adapter_cfg, provision);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Flat uuid_filters / bind_models on inject (pack convenience). */
    const cJSON *flat_filters = cJSON_GetObjectItem(fields, "uuid_filters");
    if (flat_filters) {
        cJSON *prov = cJSON_GetObjectItem(adapter_cfg, "provision");
        if (!prov) {
            prov = cJSON_CreateObject();
            cJSON_AddItemToObject(adapter_cfg, "provision", prov);
        }
        if (cJSON_IsObject(prov)) {
            (void)merge_json_array_append(prov, "uuid_filters", flat_filters);
        }
    }
    const cJSON *flat_binds = cJSON_GetObjectItem(fields, "bind_models");
    if (flat_binds) {
        cJSON *prov = cJSON_GetObjectItem(adapter_cfg, "provision");
        if (!prov) {
            prov = cJSON_CreateObject();
            cJSON_AddItemToObject(adapter_cfg, "provision", prov);
        }
        if (cJSON_IsObject(prov)) {
            (void)merge_json_array_append(prov, "bind_models", flat_binds);
        }
    }

    const cJSON *child = fields->child;
    while (child) {
        if (child->string &&
            strcmp(child->string, "devices") != 0 &&
            strcmp(child->string, "provision") != 0 &&
            strcmp(child->string, "uuid_filters") != 0 &&
            strcmp(child->string, "bind_models") != 0 &&
            strcmp(child->string, "adapter") != 0) {
            cJSON *dup = cJSON_Duplicate(child, true);
            cJSON *existing = cJSON_GetObjectItem(adapter_cfg, child->string);
            if (existing) {
                cJSON_ReplaceItemInObject(adapter_cfg, child->string, dup);
            } else {
                cJSON_AddItemToObject(adapter_cfg, child->string, dup);
            }
        }
        child = child->next;
    }
    return ESP_OK;
}

esp_err_t minihub_pack_merge_into_adapters(cJSON *adapters_root)
{
    if (!adapters_root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!adapters) {
        adapters = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters_root, "adapters", adapters);
    }

    for (size_t i = 0; i < s_pack_count; i++) {
        const pack_entry_t *pack = &s_packs[i];
        if (!pack->enabled || pack->inject_adapter[0] == '\0' || !pack->raw) {
            continue;
        }

        const cJSON *inject = cJSON_GetObjectItem(pack->raw, "inject");
        if (!cJSON_IsObject(inject)) {
            continue;
        }

        cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, pack->inject_adapter);
        if (adapter_cfg) {
            const cJSON *enabled = cJSON_GetObjectItem(adapter_cfg, "enabled");
            if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) {
                ESP_LOGI(TAG, "skip pack '%s': adapter '%s' disabled", pack->id, pack->inject_adapter);
                continue;
            }
        } else {
            adapter_cfg = cJSON_CreateObject();
            cJSON_AddBoolToObject(adapter_cfg, "enabled", true);
            cJSON_AddItemToObject(adapters, pack->inject_adapter, adapter_cfg);
            ESP_LOGI(TAG, "created adapter '%s' for pack '%s'", pack->inject_adapter, pack->id);
        }

        merge_adapter_fields(adapter_cfg, inject);
        ESP_LOGI(TAG, "merged pack '%s' into adapter '%s'", pack->id, pack->inject_adapter);
    }

    return ESP_OK;
}

esp_err_t minihub_pack_reload(cJSON *adapters_root)
{
    esp_err_t err = minihub_pack_init();
    if (err != ESP_OK) {
        return err;
    }
    return minihub_pack_merge_into_adapters(adapters_root);
}
