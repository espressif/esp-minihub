/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"
#include "device_names.h"

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

#if CONFIG_MINIHUB_POINT_DEBOUNCE_MS > 0
/* Trailing-edge debounce. While updates arrive within the window we keep the
 * latest value in point->value but suppress the immediate point.updated event.
 * A periodic sweep publishes the FINAL value once the window has elapsed since
 * the last update, so subscribers (e.g. the RainMaker bridge) still receive the
 * eventual state instead of silently dropping it. */
static int64_t s_pending_ms[MINIHUB_MAX_POINTS];
static esp_timer_handle_t s_debounce_timer;

static void debounce_sweep_cb(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time() / 1000;
    for (uint16_t i = 0; i < MINIHUB_MAX_POINTS; i++) {
        if (s_pending_ms[i] == 0) {
            continue;
        }
        if ((now - s_pending_ms[i]) < CONFIG_MINIHUB_POINT_DEBOUNCE_MS) {
            continue;
        }
        char val[MINIHUB_VALUE_MAX];
        val[0] = '\0';
        minihub_point_registry_lock();
        minihub_point_t *point = minihub_point_get_by_index_locked(i);
        if (point) {
            strncpy(val, point->value, sizeof(val) - 1);
            val[sizeof(val) - 1] = '\0';
        }
        minihub_point_registry_unlock();
        s_pending_ms[i] = 0;
        if (point) {
            esp_err_t err = minihub_event_bus_publish_point_updated(i, val, val);
            if (err == ESP_OK) {
                s_last_publish_ms[i] = now;
            }
        }
    }
}

esp_err_t minihub_point_debounce_init(void)
{
    const int period_ms = (CONFIG_MINIHUB_POINT_DEBOUNCE_MS < 100)
                              ? CONFIG_MINIHUB_POINT_DEBOUNCE_MS : 100;
    esp_timer_create_args_t t = {
        .callback = debounce_sweep_cb,
        .name = "pt_debounce",
    };
    esp_err_t err = esp_timer_create(&t, &s_debounce_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_debounce_timer, (uint64_t)period_ms * 1000ULL);
    }
    return err;
}
#endif

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
    if (!point_id || point_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!value) {
        value = "";
    }

    /* Hold the registry lock for the whole index -> pointer -> write sequence so
     * a concurrent unregister (which left-shifts s_points) cannot move the slot
     * out from under us. Previously the lock was dropped between index_of and the
     * write, letting us store into the wrong point / double-free its props. */
    minihub_point_registry_lock();
    int16_t index = minihub_point_index_of_locked(point_id);
    if (index < 0) {
        minihub_point_registry_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    minihub_point_t *point = minihub_point_get_by_index_locked((uint16_t)index);
    if (!point) {
        minihub_point_registry_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    char old_value[MINIHUB_VALUE_MAX];
    strncpy(old_value, point->value, sizeof(old_value) - 1);
    old_value[sizeof(old_value) - 1] = '\0';

    bool changed = (strcmp(old_value, value) != 0);
    if (props) {
        cJSON *dup = cJSON_Duplicate(props, true);
        if (!dup) {
            minihub_point_registry_unlock();
            return ESP_ERR_NO_MEM;
        }
        minihub_device_name_overlay(point_id, &dup);
        if (!props_equal(point->props, dup)) {
            if (point->props) {
                cJSON_Delete(point->props);
            }
            point->props = dup;
            changed = true;
        } else {
            cJSON_Delete(dup);
        }
    }

    if (!changed) {
        minihub_point_registry_unlock();
        return ESP_OK;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool debounced = false;
#if CONFIG_MINIHUB_POINT_DEBOUNCE_MS > 0
    if (s_last_publish_ms[(uint16_t)index] != 0 &&
        (now_ms - s_last_publish_ms[(uint16_t)index]) < CONFIG_MINIHUB_POINT_DEBOUNCE_MS) {
        strncpy(point->value, value, sizeof(point->value) - 1);
        point->value[sizeof(point->value) - 1] = '\0';
        point->updated_ms = now_ms;
        debounced = true;
        /* Mark a pending trailing-edge publish; the sweep emits the final value
         * once the debounce window elapses since this (latest) update. */
        s_pending_ms[(uint16_t)index] = now_ms;
    }
#endif

    bool value_changed = (strcmp(old_value, value) != 0);
    if (!debounced) {
        strncpy(point->value, value, sizeof(point->value) - 1);
        point->value[sizeof(point->value) - 1] = '\0';
        point->updated_ms = now_ms;
        point->online = true;
    }
    minihub_point_registry_unlock();

    if (debounced) {
        return ESP_OK;
    }

    if (value_changed) {
        minihub_point_history_record(point_id, old_value, value);
    }

    esp_err_t err = minihub_event_bus_publish_point_updated((uint16_t)index, old_value, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event publish failed: %s", esp_err_to_name(err));
    } else {
        s_last_publish_ms[(uint16_t)index] = now_ms;
        s_pending_ms[(uint16_t)index] = 0;   /* cancel any trailing-edge pending */
    }
    return ESP_OK;
}

esp_err_t minihub_point_merge_props(const char *point_id, const char *value, cJSON *props_patch)
{
    if (!point_id || point_id[0] == '\0' || !props_patch) {
        return ESP_ERR_INVALID_ARG;
    }

    minihub_point_t snap = {0};
    esp_err_t err = minihub_point_get_snapshot(point_id, &snap);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *merged = snap.props ? cJSON_Duplicate(snap.props, true) : cJSON_CreateObject();
    if (snap.props) {
        cJSON_Delete(snap.props);
        snap.props = NULL;
    }
    if (!merged) {
        return ESP_ERR_NO_MEM;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, props_patch) {
        if (!item->string || !item->string[0]) {
            continue;
        }
        cJSON *dup = cJSON_Duplicate(item, true);
        if (!dup) {
            cJSON_Delete(merged);
            return ESP_ERR_NO_MEM;
        }
        cJSON_DeleteItemFromObject(merged, item->string);
        cJSON_AddItemToObject(merged, item->string, dup);
    }

    const char *use_value = value ? value : snap.value;
    err = minihub_point_set(point_id, use_value, merged);
    cJSON_Delete(merged);
    return err;
}

esp_err_t minihub_point_merge_params(const char *point_id, const char *value, cJSON *params)
{
    if (!point_id || point_id[0] == '\0' || !params) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *patch = cJSON_CreateObject();
    if (!patch) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *dup = cJSON_Duplicate(params, true);
    if (!dup) {
        cJSON_Delete(patch);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(patch, "params", dup);
    esp_err_t err = minihub_point_merge_props(point_id, value, patch);
    cJSON_Delete(patch);
    return err;
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
