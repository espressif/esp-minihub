/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display_name.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DISPLAY_NAME_PATH     "/data/config/display_name.json"
#define DISPLAY_NAME_TMP_PATH "/data/config/display_name.json.tmp"
#define DISPLAY_NAME_FILE_MAX 256

static SemaphoreHandle_t s_lock;
static char s_name[MINIHUB_DISPLAY_NAME_MAX + 1];
static bool s_loaded;

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
        if (++n > MINIHUB_DISPLAY_NAME_MAX) {
            return false;
        }
    }
    return true;
}

static bool lock_name(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

static void load_locked(void)
{
    if (s_loaded) {
        return;
    }
    s_name[0] = '\0';
    s_loaded = true;
    FILE *f = fopen(DISPLAY_NAME_PATH, "r");
    if (!f) {
        return;
    }
    char buf[DISPLAY_NAME_FILE_MAX];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (cJSON_IsString(name) && name_ok(name->valuestring)) {
        strncpy(s_name, name->valuestring, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }
    cJSON_Delete(root);
}

static esp_err_t save_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "name", s_name);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    FILE *f = fopen(DISPLAY_NAME_TMP_PATH, "w");
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
        remove(DISPLAY_NAME_TMP_PATH);
        return ESP_FAIL;
    }
    if (rename(DISPLAY_NAME_TMP_PATH, DISPLAY_NAME_PATH) != 0) {
        remove(DISPLAY_NAME_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t minihub_display_name_get(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!lock_name()) {
        return ESP_ERR_NO_MEM;
    }
    load_locked();
    if (s_name[0]) {
        strncpy(buf, s_name, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t minihub_display_name_set(const char *name)
{
    if (!name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock_name()) {
        return ESP_ERR_NO_MEM;
    }
    load_locked();
    char prev[sizeof(s_name)];
    memcpy(prev, s_name, sizeof(prev));
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    esp_err_t err = save_locked();
    if (err != ESP_OK) {
        memcpy(s_name, prev, sizeof(s_name));
    }
    xSemaphoreGive(s_lock);
    return err;
}
