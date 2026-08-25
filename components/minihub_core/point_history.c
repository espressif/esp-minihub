/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/** Global value-change ring (filtered by point_id on read). Keeps DRAM small. */
#define POINT_HISTORY_GLOBAL 160

typedef struct {
    int64_t ts_ms;
    char point_id[MINIHUB_POINT_ID_MAX];
    char old_value[MINIHUB_VALUE_MAX];
    char new_value[MINIHUB_VALUE_MAX];
} point_hist_entry_t;

static point_hist_entry_t s_hist[POINT_HISTORY_GLOBAL];
static size_t s_head;
static size_t s_count;
static SemaphoreHandle_t s_mutex;

static int64_t history_now_ms(void)
{
    time_t now = time(NULL);
    if (now > (time_t)1700000000) {
        return (int64_t)now * 1000;
    }
    return esp_timer_get_time() / 1000;
}

void minihub_point_history_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    memset(s_hist, 0, sizeof(s_hist));
    s_head = 0;
    s_count = 0;
}

void minihub_point_history_record(const char *point_id, const char *old_value, const char *new_value)
{
    if (!point_id || !point_id[0] || !old_value || !new_value) {
        return;
    }
    if (strcmp(old_value, new_value) == 0) {
        return;
    }
    if (!s_mutex) {
        minihub_point_history_init();
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    point_hist_entry_t *e = &s_hist[s_head % POINT_HISTORY_GLOBAL];
    e->ts_ms = history_now_ms();
    strncpy(e->point_id, point_id, sizeof(e->point_id) - 1);
    e->point_id[sizeof(e->point_id) - 1] = '\0';
    strncpy(e->old_value, old_value, sizeof(e->old_value) - 1);
    e->old_value[sizeof(e->old_value) - 1] = '\0';
    strncpy(e->new_value, new_value, sizeof(e->new_value) - 1);
    e->new_value[sizeof(e->new_value) - 1] = '\0';
    s_head = (s_head + 1) % POINT_HISTORY_GLOBAL;
    if (s_count < POINT_HISTORY_GLOBAL) {
        s_count++;
    }
    xSemaphoreGive(s_mutex);
}

cJSON *minihub_point_history_json(const char *point_id, size_t limit)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return NULL;
    }
    cJSON_AddItemToObject(root, "items", items);
    if (!point_id || !point_id[0]) {
        return root;
    }
    if (limit == 0 || limit > 64) {
        limit = 64;
    }
    if (!s_mutex) {
        minihub_point_history_init();
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return root;
    }
    size_t matched = 0;
    for (size_t i = 0; i < s_count && matched < limit; i++) {
        size_t idx = (s_head + POINT_HISTORY_GLOBAL - 1 - i) % POINT_HISTORY_GLOBAL;
        const point_hist_entry_t *e = &s_hist[idx];
        if (strcmp(e->point_id, point_id) != 0) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "ts_ms", (double)e->ts_ms);
        cJSON_AddStringToObject(item, "old", e->old_value);
        cJSON_AddStringToObject(item, "new", e->new_value);
        cJSON_AddStringToObject(item, "source", "local");
        cJSON_AddItemToArray(items, item);
        matched++;
    }
    xSemaphoreGive(s_mutex);
    return root;
}
