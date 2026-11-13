/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_nvs.h"
#include "ble_mesh_map.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "bm_nvs";
static const char *NS = "bm_map";
static const char *KEY = "devs";

static cJSON *devices_to_json(const minihub_ble_mesh_device_t *devs, size_t count)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const minihub_ble_mesh_device_t *d = &devs[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(obj, "point_id", d->point_id);
        cJSON_AddStringToObject(obj, "type", d->type);
        cJSON_AddStringToObject(obj, "uuid", d->uuid_hex);
        cJSON_AddNumberToObject(obj, "unicast", d->unicast);
        cJSON_AddNumberToObject(obj, "elem_num", d->elem_num);
        cJSON_AddNumberToObject(obj, "caps", d->caps);
        cJSON_AddNumberToObject(obj, "cid", d->cid);
        cJSON_AddNumberToObject(obj, "pid", d->pid);
        cJSON_AddNumberToObject(obj, "vid", d->vid);
        cJSON *props = cJSON_CreateArray();
        if (props) {
            for (size_t p = 0; p < d->prop_count && p < MINIHUB_BM_MAX_PROPS; p++) {
                cJSON_AddNumberToObject(props, NULL, d->props[p]);
            }
            cJSON_AddItemToObject(obj, "props", props);
        }
        cJSON *binds = cJSON_CreateArray();
        if (binds) {
            for (size_t b = 0; b < d->bind_count && b < MINIHUB_BM_BIND_MODEL_MAX; b++) {
                cJSON *row = cJSON_CreateObject();
                if (!row) {
                    break;
                }
                cJSON_AddNumberToObject(row, "model", d->binds[b].model_id);
                cJSON_AddNumberToObject(row, "elem", d->binds[b].elem_addr);
                cJSON_AddItemToArray(binds, row);
            }
            cJSON_AddItemToObject(obj, "binds", binds);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

static esp_err_t devices_from_json(const cJSON *arr, minihub_ble_mesh_device_t *out, size_t cap, size_t *count)
{
    if (!cJSON_IsArray(arr) || !out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (n >= cap) {
            break;
        }
        minihub_ble_mesh_device_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *uuid = cJSON_GetObjectItem(item, "uuid");
        const cJSON *unicast = cJSON_GetObjectItem(item, "unicast");
        const cJSON *elem_num = cJSON_GetObjectItem(item, "elem_num");
        const cJSON *caps = cJSON_GetObjectItem(item, "caps");
        const cJSON *cid = cJSON_GetObjectItem(item, "cid");
        const cJSON *pid = cJSON_GetObjectItem(item, "pid");
        const cJSON *vid = cJSON_GetObjectItem(item, "vid");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }
        strncpy(d->point_id, point_id->valuestring, sizeof(d->point_id) - 1);
        strncpy(d->type, type->valuestring, sizeof(d->type) - 1);
        if (cJSON_IsString(uuid)) {
            strncpy(d->uuid_hex, uuid->valuestring, sizeof(d->uuid_hex) - 1);
        }
        if (cJSON_IsNumber(unicast)) {
            d->unicast = (uint16_t)unicast->valuedouble;
        }
        if (cJSON_IsNumber(elem_num)) {
            d->elem_num = (uint8_t)elem_num->valuedouble;
        } else {
            d->elem_num = 1;
        }
        if (cJSON_IsNumber(caps)) {
            d->caps = (uint16_t)caps->valuedouble;
        } else {
            /* No stored caps means unknown, not OnOff: classify decides, and an
             * unclassifiable node must not masquerade as a controllable switch. */
            d->caps = 0;
        }
        if (cJSON_IsNumber(cid)) {
            d->cid = (uint16_t)cid->valuedouble;
        }
        if (cJSON_IsNumber(pid)) {
            d->pid = (uint16_t)pid->valuedouble;
        }
        if (cJSON_IsNumber(vid)) {
            d->vid = (uint16_t)vid->valuedouble;
        }
        const cJSON *props = cJSON_GetObjectItem(item, "props");
        if (cJSON_IsArray(props)) {
            const cJSON *pid = NULL;
            cJSON_ArrayForEach(pid, props) {
                if (d->prop_count >= MINIHUB_BM_MAX_PROPS) {
                    break;
                }
                if (cJSON_IsNumber(pid)) {
                    d->props[d->prop_count++] = (uint16_t)pid->valuedouble;
                }
            }
        }
        const cJSON *binds = cJSON_GetObjectItem(item, "binds");
        if (cJSON_IsArray(binds)) {
            const cJSON *row = NULL;
            cJSON_ArrayForEach(row, binds) {
                if (d->bind_count >= MINIHUB_BM_BIND_MODEL_MAX) {
                    break;
                }
                const cJSON *model = cJSON_GetObjectItem(row, "model");
                const cJSON *elem = cJSON_GetObjectItem(row, "elem");
                if (!cJSON_IsNumber(model) || !cJSON_IsNumber(elem)) {
                    continue;
                }
                d->binds[d->bind_count].model_id = (uint16_t)model->valuedouble;
                d->binds[d->bind_count].elem_addr = (uint16_t)elem->valuedouble;
                d->bind_count++;
            }
        }
        n++;
    }
    *count = n;
    return ESP_OK;
}

static esp_err_t nvs_save(const minihub_ble_mesh_device_t *devs, size_t count)
{
    cJSON *arr = devices_to_json(devs, count);
    if (!arr) {
        return ESP_ERR_NO_MEM;
    }
    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        free(printed);
        return err;
    }
    err = nvs_set_str(h, KEY, printed);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    free(printed);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t minihub_bm_nvs_load(minihub_ble_mesh_device_t *out, size_t cap, size_t *count)
{
    if (!out || !count || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    size_t len = 0;
    err = nvs_get_str(h, KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    char *buf = calloc(1, len);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(h, KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return ESP_FAIL;
    }
    err = devices_from_json(root, out, cap, count);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }
    /* Migrate legacy "{type}/bm_XXXX" → "ble_mesh.bm_XXXX" so UI sees dotted ids. */
    bool dirty = false;
    for (size_t i = 0; i < *count; i++) {
        if (minihub_bm_normalize_point_id(out[i].point_id, sizeof(out[i].point_id))) {
            dirty = true;
            ESP_LOGI(TAG, "migrated point_id → %s", out[i].point_id);
        }
    }
    if (dirty) {
        esp_err_t serr = nvs_save(out, *count);
        if (serr != ESP_OK) {
            ESP_LOGW(TAG, "NVS rewrite after migrate: %s", esp_err_to_name(serr));
        }
    }
    ESP_LOGI(TAG, "loaded %u devices from NVS", (unsigned)*count);
    return ESP_OK;
}

esp_err_t minihub_bm_nvs_upsert(const minihub_ble_mesh_device_t *dev,
                                minihub_ble_mesh_device_t *table, size_t *count, size_t cap)
{
    if (!dev || !table || !count || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < *count; i++) {
        bool same_uuid = dev->uuid_hex[0] && strcmp(table[i].uuid_hex, dev->uuid_hex) == 0;
        bool same_point = strcmp(table[i].point_id, dev->point_id) == 0;
        bool same_addr = dev->unicast != 0 && table[i].unicast == dev->unicast;
        if (same_uuid || same_point || same_addr) {
            table[i] = *dev;
            return nvs_save(table, *count);
        }
    }
    if (*count >= cap) {
        return ESP_ERR_NO_MEM;
    }
    table[*count] = *dev;
    (*count)++;
    return nvs_save(table, *count);
}

esp_err_t minihub_bm_nvs_remove(const char *point_id, minihub_ble_mesh_device_t *table, size_t *count)
{
    if (!point_id || !point_id[0] || !table || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Exact match first; then match by stable "bm_XXXX" so legacy slash ids
     * and dotted ids for the same unicast can still be deleted. */
    const char *short_id = minihub_bm_stable_suffix(point_id);
    int found = -1;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(table[i].point_id, point_id) == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0 && short_id && short_id[0]) {
        for (size_t i = 0; i < *count; i++) {
            const char *tshort = minihub_bm_stable_suffix(table[i].point_id);
            if (tshort && strcmp(tshort, short_id) == 0) {
                found = (int)i;
                break;
            }
        }
    }
    if (found < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t i = (size_t)found;
    if (i + 1 < *count) {
        memmove(&table[i], &table[i + 1], (*count - i - 1) * sizeof(table[0]));
    }
    (*count)--;
    memset(&table[*count], 0, sizeof(table[0]));
    return nvs_save(table, *count);
}
