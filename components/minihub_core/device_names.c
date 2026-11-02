/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "device_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DEVICE_NAMES_PATH     "/data/config/device_names.json"
#define DEVICE_NAMES_TMP_PATH "/data/config/device_names.json.tmp"
#define DEVICE_NAME_MAX       48
#define DEVICE_NAMES_FILE_MAX 32768

static SemaphoreHandle_t s_lock;
static cJSON *s_names;

static bool name_ok(const char *name)
{
    if (!name || name[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20) {
            return false;
        }
        if (++n > DEVICE_NAME_MAX) {
            return false;
        }
    }
    return true;
}

static void load_locked(void)
{
    if (s_names) {
        return;
    }
    FILE *f = fopen(DEVICE_NAMES_PATH, "r");
    if (!f) {
        s_names = cJSON_CreateObject();
        return;
    }
    char *buf = malloc(DEVICE_NAMES_FILE_MAX);
    if (!buf) {
        fclose(f);
        s_names = cJSON_CreateObject();
        return;
    }
    size_t n = fread(buf, 1, DEVICE_NAMES_FILE_MAX - 1, f);
    fclose(f);
    buf[n] = '\0';
    s_names = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(s_names)) {
        cJSON_Delete(s_names);
        s_names = cJSON_CreateObject();
    }
}

static esp_err_t save_locked(void)
{
    char *text = cJSON_PrintUnformatted(s_names);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(DEVICE_NAMES_TMP_PATH, "w");
    if (!f) {
        free(text);
        return ESP_FAIL;
    }
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    int bad = ferror(f);
    fclose(f);
    free(text);
    if (w != n || bad) {
        remove(DEVICE_NAMES_TMP_PATH);
        return ESP_FAIL;
    }
    if (rename(DEVICE_NAMES_TMP_PATH, DEVICE_NAMES_PATH) != 0) {
        remove(DEVICE_NAMES_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool lock_names(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_locked();
    if (!s_names) {
        xSemaphoreGive(s_lock);
        return false;
    }
    return true;
}

esp_err_t minihub_device_name_set(const char *point_id, const char *name)
{
    if (!point_id || point_id[0] == '\0' || !name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_names()) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *prev = cJSON_DetachItemFromObject(s_names, point_id);
    cJSON_AddStringToObject(s_names, point_id, name);
    esp_err_t err = save_locked();
    if (err != ESP_OK) {
        cJSON_DeleteItemFromObject(s_names, point_id);
        if (prev) {
            cJSON_AddItemToObject(s_names, point_id, prev);
            prev = NULL;
        }
    }
    cJSON_Delete(prev);
    xSemaphoreGive(s_lock);
    return err;
}

void minihub_device_name_overlay(const char *point_id, cJSON **props)
{
    if (!point_id || point_id[0] == '\0' || !props || !lock_names()) {
        return;
    }
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(s_names, point_id);
    if (cJSON_IsString(n) && n->valuestring && n->valuestring[0] != '\0') {
        if (!*props) {
            *props = cJSON_CreateObject();
        }
        if (*props) {
            cJSON_DeleteItemFromObject(*props, "name");
            cJSON_AddStringToObject(*props, "name", n->valuestring);
        }
    }
    xSemaphoreGive(s_lock);
}
