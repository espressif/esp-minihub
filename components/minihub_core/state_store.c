/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifndef CONFIG_MINIHUB_POINT_DEBOUNCE_MS
#define CONFIG_MINIHUB_POINT_DEBOUNCE_MS 0
#endif

static const char *TAG = "state_store";

extern esp_err_t minihub_point_registry_init(void);

static int64_t s_last_publish_ms[MINIHUB_MAX_POINTS];

static bool props_equal(const cJSON *a, const cJSON *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    char *sa = cJSON_PrintUnformatted(a);
    char *sb = cJSON_PrintUnformatted(b);
    bool eq = (sa && sb && strcmp(sa, sb) == 0);
    free(sa);
    free(sb);
    return eq;
}

esp_err_t minihub_point_set(const char *point_id, const char *value, cJSON *props)
{
    uint16_t index = minihub_point_index_of(point_id);
    if (index == UINT16_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    minihub_point_t *point = minihub_point_get_by_index(index);
    if (!point) {
        return ESP_ERR_NOT_FOUND;
    }

    if (!value) {
        value = "";
    }

    char old_value[MINIHUB_VALUE_MAX];
    strncpy(old_value, point->value, sizeof(old_value) - 1);
    old_value[sizeof(old_value) - 1] = '\0';

    bool changed = (strcmp(old_value, value) != 0);
    if (props) {
        if (!props_equal(point->props, props)) {
            if (point->props) {
                cJSON_Delete(point->props);
            }
            point->props = cJSON_Duplicate(props, true);
            changed = true;
        }
    }

    if (!changed) {
        return ESP_OK;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
#if CONFIG_MINIHUB_POINT_DEBOUNCE_MS > 0
    if (s_last_publish_ms[index] != 0 &&
        (now_ms - s_last_publish_ms[index]) < CONFIG_MINIHUB_POINT_DEBOUNCE_MS) {
        strncpy(point->value, value, sizeof(point->value) - 1);
        point->value[sizeof(point->value) - 1] = '\0';
        point->updated_ms = now_ms;
        return ESP_OK;
    }
#endif

    bool value_changed = (strcmp(old_value, value) != 0);
    strncpy(point->value, value, sizeof(point->value) - 1);
    point->value[sizeof(point->value) - 1] = '\0';
    point->updated_ms = now_ms;
    point->online = true;

    if (value_changed) {
        minihub_point_history_record(point_id, old_value, value);
    }

    esp_err_t err = minihub_event_bus_publish_point_updated(index, old_value, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event publish failed: %s", esp_err_to_name(err));
    } else {
        s_last_publish_ms[index] = now_ms;
    }
    return ESP_OK;
}

esp_err_t minihub_point_get_snapshot(const char *point_id, minihub_point_t *out)
{
    minihub_point_t *point = minihub_point_get(point_id);
    if (!point || !out) {
        return point ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->point_id, point->point_id, sizeof(out->point_id) - 1);
    strncpy(out->type, point->type, sizeof(out->type) - 1);
    strncpy(out->value, point->value, sizeof(out->value) - 1);
    strncpy(out->adapter, point->adapter, sizeof(out->adapter) - 1);
    out->online = point->online;
    out->updated_ms = point->updated_ms;
    if (point->props) {
        out->props = cJSON_Duplicate(point->props, true);
    }
    return ESP_OK;
}

bool minihub_point_value_changed(const char *point_id, const char *old_value, const char *new_value)
{
    (void)point_id;
    return strcmp(old_value, new_value) != 0;
}
