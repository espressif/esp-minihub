/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"
#include "device_names.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "point_registry";

#define HASH_SLOTS MINIHUB_MAX_POINTS

EXT_RAM_BSS_ATTR static minihub_point_t s_points[MINIHUB_MAX_POINTS];
static size_t s_point_count;
static int16_t s_hash_table[HASH_SLOTS];
static SemaphoreHandle_t s_mutex;

static uint32_t hash_point_id(const char *point_id)
{
    uint32_t hash = 5381;
    for (const char *p = point_id; *p; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)(*p);
    }
    return hash;
}

static void hash_clear(void)
{
    for (size_t i = 0; i < HASH_SLOTS; i++) {
        s_hash_table[i] = -1;
    }
}

static void hash_rebuild(void)
{
    hash_clear();
    for (size_t i = 0; i < s_point_count; i++) {
        uint32_t slot = hash_point_id(s_points[i].point_id) % HASH_SLOTS;
        for (size_t probe = 0; probe < HASH_SLOTS; probe++) {
            size_t pos = (slot + probe) % HASH_SLOTS;
            if (s_hash_table[pos] < 0) {
                s_hash_table[pos] = (int16_t)i;
                break;
            }
        }
    }
}

static void copy_point(minihub_point_t *dst, const minihub_point_t *src)
{
    strncpy(dst->point_id, src->point_id, sizeof(dst->point_id) - 1);
    strncpy(dst->type, src->type, sizeof(dst->type) - 1);
    strncpy(dst->value, src->value, sizeof(dst->value) - 1);
    strncpy(dst->adapter, src->adapter, sizeof(dst->adapter) - 1);
    dst->online = src->online;
    dst->updated_ms = src->updated_ms;
    if (dst->props) {
        cJSON_Delete(dst->props);
        dst->props = NULL;
    }
    if (src->props) {
        dst->props = cJSON_Duplicate(src->props, true);
    }
}

esp_err_t minihub_point_registry_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_point_count = 0;
    hash_clear();
    return ESP_OK;
}

static int find_index_locked(const char *point_id)
{
    uint32_t slot = hash_point_id(point_id) % HASH_SLOTS;
    for (size_t probe = 0; probe < HASH_SLOTS; probe++) {
        size_t pos = (slot + probe) % HASH_SLOTS;
        int16_t idx = s_hash_table[pos];
        if (idx < 0) {
            return -1;
        }
        if (strcmp(s_points[idx].point_id, point_id) == 0) {
            return idx;
        }
    }
    return -1;
}

esp_err_t minihub_point_register(const minihub_point_t *point)
{
    if (!point || point->point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_index_locked(point->point_id);
    if (idx >= 0) {
        copy_point(&s_points[idx], point);
        minihub_device_name_overlay(s_points[idx].point_id, &s_points[idx].props);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    if (s_point_count >= MINIHUB_MAX_POINTS) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "point limit reached");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_points[s_point_count], 0, sizeof(s_points[s_point_count]));
    copy_point(&s_points[s_point_count], point);
    minihub_device_name_overlay(s_points[s_point_count].point_id, &s_points[s_point_count].props);
    s_point_count++;
    hash_rebuild();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "registered %s (%s)", point->point_id, point->type);
    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "point_id", point->point_id);
        cJSON_AddStringToObject(data, "type", point->type);
        cJSON_AddStringToObject(data, "adapter", point->adapter);
        minihub_event_bus_publish("point.registered", data);
        cJSON_Delete(data);
    }
    return ESP_OK;
}

esp_err_t minihub_point_unregister(const char *point_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_index_locked(point_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_points[idx].props) {
        cJSON_Delete(s_points[idx].props);
    }
    char removed_id[MINIHUB_POINT_ID_MAX];
    strncpy(removed_id, s_points[idx].point_id, sizeof(removed_id) - 1);
    removed_id[sizeof(removed_id) - 1] = '\0';
    for (size_t i = (size_t)idx; i + 1 < s_point_count; i++) {
        s_points[i] = s_points[i + 1];
    }
    memset(&s_points[s_point_count - 1], 0, sizeof(minihub_point_t));
    s_point_count--;
    hash_rebuild();
    xSemaphoreGive(s_mutex);
    cJSON *data = cJSON_CreateObject();
    if (data) {
        cJSON_AddStringToObject(data, "point_id", removed_id);
        minihub_event_bus_publish("point.unregistered", data);
        cJSON_Delete(data);
    }
    return ESP_OK;
}

esp_err_t minihub_point_unregister_by_adapter(const char *adapter)
{
    if (!adapter || adapter[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char removed_ids[MINIHUB_MAX_POINTS][MINIHUB_POINT_ID_MAX];
    size_t removed = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_point_count; ) {
        if (strcmp(s_points[i].adapter, adapter) != 0) {
            i++;
            continue;
        }
        if (removed < MINIHUB_MAX_POINTS) {
            strncpy(removed_ids[removed], s_points[i].point_id, MINIHUB_POINT_ID_MAX - 1);
            removed_ids[removed][MINIHUB_POINT_ID_MAX - 1] = '\0';
            removed++;
        }
        if (s_points[i].props) {
            cJSON_Delete(s_points[i].props);
        }
        for (size_t j = i + 1; j < s_point_count; j++) {
            s_points[j - 1] = s_points[j];
        }
        memset(&s_points[s_point_count - 1], 0, sizeof(minihub_point_t));
        s_point_count--;
    }
    if (removed > 0) {
        hash_rebuild();
        ESP_LOGI(TAG, "unregistered %u points for adapter '%s'", (unsigned)removed, adapter);
    }
    xSemaphoreGive(s_mutex);

    /* Publish after unlock so RainMaker / Web can unmap devices (soft reload path). */
    for (size_t i = 0; i < removed; i++) {
        cJSON *data = cJSON_CreateObject();
        if (data) {
            cJSON_AddStringToObject(data, "point_id", removed_ids[i]);
            minihub_event_bus_publish("point.unregistered", data);
            cJSON_Delete(data);
        }
    }
    return ESP_OK;
}

esp_err_t minihub_point_set_online(const char *point_id, bool online)
{
    if (!point_id || point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_index_locked(point_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    s_points[idx].online = online;
    s_points[idx].updated_ms = esp_timer_get_time() / 1000;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

minihub_point_t *minihub_point_get(const char *point_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_index_locked(point_id);
    minihub_point_t *result = idx >= 0 ? &s_points[idx] : NULL;
    xSemaphoreGive(s_mutex);
    return result;
}

minihub_point_t *minihub_point_get_by_index(uint16_t index)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    minihub_point_t *result = (index < s_point_count) ? &s_points[index] : NULL;
    xSemaphoreGive(s_mutex);
    return result;
}

uint16_t minihub_point_index_of(const char *point_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = find_index_locked(point_id);
    xSemaphoreGive(s_mutex);
    return idx >= 0 ? (uint16_t)idx : UINT16_MAX;
}

static esp_err_t filter_points(bool (*match)(const minihub_point_t *, const void *), const void *ctx,
                               minihub_point_t **out, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t matched = 0;
    for (size_t i = 0; i < s_point_count; i++) {
        if (match(&s_points[i], ctx)) {
            matched++;
        }
    }

    if (matched == 0) {
        *out = NULL;
        *count = 0;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    minihub_point_t *buf = calloc(matched, sizeof(minihub_point_t));
    if (!buf) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t j = 0;
    for (size_t i = 0; i < s_point_count; i++) {
        if (match(&s_points[i], ctx)) {
            copy_point(&buf[j++], &s_points[i]);
        }
    }
    xSemaphoreGive(s_mutex);

    *out = buf;
    *count = matched;
    return ESP_OK;
}

static bool match_all(const minihub_point_t *p, const void *ctx)
{
    (void)p;
    (void)ctx;
    return true;
}

static bool match_type(const minihub_point_t *p, const void *ctx)
{
    return strcmp(p->type, (const char *)ctx) == 0;
}

static bool match_adapter(const minihub_point_t *p, const void *ctx)
{
    return strcmp(p->adapter, (const char *)ctx) == 0;
}

esp_err_t minihub_point_list(minihub_point_t **out, size_t *count)
{
    return filter_points(match_all, NULL, out, count);
}

esp_err_t minihub_point_list_by_type(const char *type, minihub_point_t **out, size_t *count)
{
    return filter_points(match_type, type, out, count);
}

esp_err_t minihub_point_list_by_adapter(const char *adapter, minihub_point_t **out, size_t *count)
{
    return filter_points(match_adapter, adapter, out, count);
}

/* Free a point list previously returned by minihub_point_list*(). The registry
 * deep-copies each point's props via cJSON_Duplicate, so callers must free them
 * too - a bare free() on the array leaks every props object. */
void minihub_point_list_free(minihub_point_t *list, size_t count)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (list[i].props) {
            cJSON_Delete(list[i].props);
            list[i].props = NULL;
        }
    }
    free(list);
}

/* ---- internal locked accessors ----
 * state_store's minihub_point_set must resolve an index, dereference the
 * pointer and write the value atomically against concurrent
 * register/unregister (which left-shifts the point array). These helpers let
 * it keep s_mutex held across that whole sequence. Callers MUST bracket them
 * with minihub_point_registry_lock()/unlock(). */
void minihub_point_registry_lock(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void minihub_point_registry_unlock(void)
{
    xSemaphoreGive(s_mutex);
}

int16_t minihub_point_index_of_locked(const char *point_id)
{
    return find_index_locked(point_id);
}

minihub_point_t *minihub_point_get_by_index_locked(uint16_t index)
{
    return (index < s_point_count) ? &s_points[index] : NULL;
}

static bool point_props_has_param(const cJSON *props, const char *parameter)
{
    if (!props || !parameter || parameter[0] == '\0') {
        return false;
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(props, "params");
    if (cJSON_IsObject(params)) {
        return cJSON_GetObjectItemCaseSensitive(params, parameter) != NULL;
    }
    if (cJSON_IsArray(params)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, params) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, parameter) == 0) {
                return true;
            }
        }
    }
    return false;
}

esp_err_t minihub_point_resolve_by_type_param(const char *type, const char *parameter,
                                              char *out, size_t size)
{
    if (!type || type[0] == '\0' || !parameter || parameter[0] == '\0' || !out || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Preferred: one device = one point, point_id "<protocol>.<device>", with
     * live params in props.params.<parameter>. Legacy: per-param ids
     * "<protocol>.<device>.<parameter>" still match by suffix. */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_point_count; i++) {
        const minihub_point_t *p = &s_points[i];
        if (strcmp(p->type, type) != 0) {
            continue;
        }
        bool match = false;
        if (point_props_has_param(p->props, parameter)) {
            match = true;
        } else {
            const char *first = strchr(p->point_id, '.');
            if (first) {
                const char *second = strchr(first + 1, '.');
                if (second && strcmp(second + 1, parameter) == 0) {
                    match = true;
                }
            }
        }
        if (!match) {
            continue;
        }
        size_t len = strlen(p->point_id);
        if (len >= size) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, p->point_id, len + 1);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}
