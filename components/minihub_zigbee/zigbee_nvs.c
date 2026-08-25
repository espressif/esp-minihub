/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_nvs.h"
#include "zigbee_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "zb_nvs";
static const char *NS = "zb_map";
static const char *KEY = "devs";

static cJSON *devices_to_json(const minihub_zigbee_device_t *devs, size_t count)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const minihub_zigbee_device_t *d = &devs[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddStringToObject(obj, "point_id", d->point_id);
        cJSON_AddStringToObject(obj, "type", d->type);
        char ieee_s[24];
        snprintf(ieee_s, sizeof(ieee_s), "0x%016llx", (unsigned long long)d->ieee);
        cJSON_AddStringToObject(obj, "ieee", ieee_s);
        cJSON_AddNumberToObject(obj, "short_addr", d->short_addr);
        cJSON_AddNumberToObject(obj, "endpoint", d->endpoint);
        cJSON_AddNumberToObject(obj, "caps", d->caps);
        cJSON_AddItemToArray(arr, obj);
    }
    return arr;
}

static esp_err_t devices_from_json(const cJSON *arr, minihub_zigbee_device_t *out, size_t cap, size_t *count)
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
        minihub_zigbee_device_t *d = &out[n];
        memset(d, 0, sizeof(*d));
        const cJSON *point_id = cJSON_GetObjectItem(item, "point_id");
        const cJSON *type = cJSON_GetObjectItem(item, "type");
        const cJSON *ieee = cJSON_GetObjectItem(item, "ieee");
        const cJSON *short_addr = cJSON_GetObjectItem(item, "short_addr");
        const cJSON *endpoint = cJSON_GetObjectItem(item, "endpoint");
        const cJSON *caps = cJSON_GetObjectItem(item, "caps");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(type)) {
            continue;
        }
        strncpy(d->point_id, point_id->valuestring, sizeof(d->point_id) - 1);
        strncpy(d->type, type->valuestring, sizeof(d->type) - 1);
        if (cJSON_IsString(ieee)) {
            (void)minihub_zb_parse_ieee(ieee->valuestring, &d->ieee);
        }
        if (cJSON_IsNumber(short_addr)) {
            d->short_addr = (uint16_t)short_addr->valuedouble;
        }
        if (cJSON_IsNumber(endpoint)) {
            d->endpoint = (uint8_t)endpoint->valuedouble;
        } else {
            d->endpoint = 1;
        }
        if (cJSON_IsNumber(caps)) {
            d->caps = (uint8_t)caps->valuedouble;
        }
        n++;
    }
    *count = n;
    return ESP_OK;
}

esp_err_t minihub_zb_nvs_load(minihub_zigbee_device_t *out, size_t cap, size_t *count)
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
    ESP_LOGI(TAG, "loaded %u devices from NVS", (unsigned)*count);
    return err;
}

esp_err_t minihub_zb_nvs_save(const minihub_zigbee_device_t *devs, size_t count)
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
    return err;
}

esp_err_t minihub_zb_nvs_upsert(const minihub_zigbee_device_t *dev,
                                minihub_zigbee_device_t *table, size_t *count, size_t cap)
{
    if (!dev || !table || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < *count; i++) {
        if (table[i].ieee == dev->ieee && table[i].endpoint == dev->endpoint) {
            table[i] = *dev;
            return minihub_zb_nvs_save(table, *count);
        }
        if (dev->point_id[0] && strcmp(table[i].point_id, dev->point_id) == 0) {
            table[i] = *dev;
            return minihub_zb_nvs_save(table, *count);
        }
    }
    if (*count >= cap) {
        return ESP_ERR_NO_MEM;
    }
    table[(*count)++] = *dev;
    return minihub_zb_nvs_save(table, *count);
}

esp_err_t minihub_zb_nvs_remove(const char *point_id, minihub_zigbee_device_t *table, size_t *count)
{
    if (!point_id || !point_id[0] || !table || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(table[i].point_id, point_id) != 0) {
            continue;
        }
        if (i + 1 < *count) {
            memmove(&table[i], &table[i + 1], (*count - i - 1) * sizeof(table[0]));
        }
        (*count)--;
        memset(&table[*count], 0, sizeof(table[0]));
        return minihub_zb_nvs_save(table, *count);
    }
    return ESP_ERR_NOT_FOUND;
}
