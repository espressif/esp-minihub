/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"

#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "event_bus";

typedef struct {
    char event_type[MINIHUB_EVENT_MAX];
    minihub_event_handler_t handler;
    void *user_data;
} subscription_t;

static subscription_t s_subs[MINIHUB_MAX_SUBS];
static size_t s_sub_count;
static SemaphoreHandle_t s_sub_mutex;
static QueueHandle_t s_event_queue;
static uint32_t s_dropped_events;

static bool event_matches(const char *pattern, const char *event_type)
{
    if (strcmp(pattern, "*") == 0) {
        return true;
    }
    return strcmp(pattern, event_type) == 0;
}

static QueueHandle_t s_event_queue;
static uint32_t s_dropped_events;

#define EVENT_HISTORY 16

typedef struct {
    char event_type[MINIHUB_EVENT_MAX];
    int64_t timestamp_ms;
    char detail[96];
} event_history_t;

static event_history_t s_history[EVENT_HISTORY];
static size_t s_history_head;

static void record_history(const minihub_event_t *event)
{
    event_history_t *h = &s_history[s_history_head % EVENT_HISTORY];
    s_history_head++;
    strncpy(h->event_type, event->event_type, sizeof(h->event_type) - 1);
    h->timestamp_ms = event->timestamp_ms;
    h->detail[0] = '\0';

    if (strcmp(event->event_type, "point.updated") == 0 && event->data == NULL) {
        snprintf(h->detail, sizeof(h->detail), "%u %s->%s",
                 (unsigned)event->point_index, event->old_value, event->new_value);
    } else if (event->data) {
        char *printed = cJSON_PrintUnformatted(event->data);
        if (printed) {
            strncpy(h->detail, printed, sizeof(h->detail) - 1);
            free(printed);
        }
    }
}

static void dispatch_event(minihub_event_t *event)
{
    record_history(event);
    subscription_t subs_copy[MINIHUB_MAX_SUBS];
    size_t count = 0;

    xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_sub_count && count < MINIHUB_MAX_SUBS; i++) {
        if (event_matches(s_subs[i].event_type, event->event_type)) {
            subs_copy[count++] = s_subs[i];
        }
    }
    xSemaphoreGive(s_sub_mutex);

    for (size_t i = 0; i < count; i++) {
        subs_copy[i].handler(event, subs_copy[i].user_data);
    }

    if (event->data) {
        cJSON_Delete(event->data);
        event->data = NULL;
    }
}

static esp_err_t enqueue_event(minihub_event_t *event)
{
    if (!s_event_queue) {
        dispatch_event(event);
        return ESP_OK;
    }

    if (xQueueSend(s_event_queue, event, 0) != pdTRUE) {
        s_dropped_events++;
        ESP_LOGW(TAG, "event queue full, dropped %s", event->event_type);
        if (event->data) {
            cJSON_Delete(event->data);
            event->data = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t minihub_event_bus_init(void)
{
    s_sub_mutex = xSemaphoreCreateMutex();
    if (!s_sub_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_sub_count = 0;
    s_dropped_events = 0;
    s_event_queue = xQueueCreate(MINIHUB_EVENT_QUEUE_DEPTH, sizeof(minihub_event_t));
    if (!s_event_queue) {
        ESP_LOGW(TAG, "event queue unavailable, using sync dispatch");
    }
    return ESP_OK;
}

QueueHandle_t minihub_event_bus_queue(void)
{
    return s_event_queue;
}

void minihub_event_bus_dispatch_one(minihub_event_t *event)
{
    dispatch_event(event);
}

esp_err_t minihub_event_bus_subscribe(const char *event_type, minihub_event_handler_t handler, void *user_data)
{
    if (!event_type || !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
    if (s_sub_count >= MINIHUB_MAX_SUBS) {
        xSemaphoreGive(s_sub_mutex);
        return ESP_ERR_NO_MEM;
    }

    subscription_t *sub = &s_subs[s_sub_count++];
    strncpy(sub->event_type, event_type, sizeof(sub->event_type) - 1);
    sub->handler = handler;
    sub->user_data = user_data;
    xSemaphoreGive(s_sub_mutex);
    return ESP_OK;
}

esp_err_t minihub_event_bus_unsubscribe(const char *event_type, minihub_event_handler_t handler)
{
    xSemaphoreTake(s_sub_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_sub_count; i++) {
        if (s_subs[i].handler == handler && strcmp(s_subs[i].event_type, event_type) == 0) {
            for (size_t j = i; j + 1 < s_sub_count; j++) {
                s_subs[j] = s_subs[j + 1];
            }
            s_sub_count--;
            xSemaphoreGive(s_sub_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_sub_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_event_bus_publish(const char *event_type, cJSON *data)
{
    if (!event_type) {
        return ESP_ERR_INVALID_ARG;
    }

    minihub_event_t event = {0};
    strncpy(event.event_type, event_type, sizeof(event.event_type) - 1);
    event.data = data ? cJSON_Duplicate(data, true) : NULL;
    event.timestamp_ms = esp_timer_get_time() / 1000;
    return enqueue_event(&event);
}

esp_err_t minihub_event_bus_publish_point_updated(uint16_t point_index, const char *old_value, const char *new_value)
{
    minihub_event_t event = {0};
    strncpy(event.event_type, "point.updated", sizeof(event.event_type) - 1);
    event.timestamp_ms = esp_timer_get_time() / 1000;
    event.point_index = point_index;
    strncpy(event.old_value, old_value, sizeof(event.old_value) - 1);
    strncpy(event.new_value, new_value, sizeof(event.new_value) - 1);
    event.data = NULL;
    return enqueue_event(&event);
}

uint32_t minihub_event_bus_dropped_count(void)
{
    return s_dropped_events;
}

cJSON *minihub_event_bus_recent_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    size_t count = s_history_head < EVENT_HISTORY ? s_history_head : EVENT_HISTORY;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (s_history_head - count + i) % EVENT_HISTORY;
        const event_history_t *h = &s_history[idx];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "event_type", h->event_type);
        cJSON_AddNumberToObject(item, "timestamp_ms", (double)h->timestamp_ms);
        if (h->detail[0]) {
            cJSON_AddStringToObject(item, "detail", h->detail);
        }
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}
