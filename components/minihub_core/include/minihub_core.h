/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"

#define MINIHUB_CORE_API_VERSION  1

#define MINIHUB_POINT_ID_MAX  64
#define MINIHUB_TYPE_MAX      16
#define MINIHUB_VALUE_MAX     32
#define MINIHUB_ADAPTER_MAX   16
#define MINIHUB_CMD_MAX       32
#define MINIHUB_EVENT_MAX     32

#define MINIHUB_MAX_POINTS    64
#define MINIHUB_MAX_HANDLERS  16
#define MINIHUB_MAX_SUBS      32
#define MINIHUB_EVENT_QUEUE_DEPTH 32

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char value[MINIHUB_VALUE_MAX];
    cJSON *props;
    char adapter[MINIHUB_ADAPTER_MAX];
    bool online;
    int64_t updated_ms;
} minihub_point_t;

typedef struct {
    char event_type[MINIHUB_EVENT_MAX];
    cJSON *data;
    int64_t timestamp_ms;
    uint16_t point_index;
    char old_value[MINIHUB_VALUE_MAX];
    char new_value[MINIHUB_VALUE_MAX];
} minihub_event_t;

typedef void (*minihub_event_handler_t)(const minihub_event_t *event, void *user_data);

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char cmd[MINIHUB_CMD_MAX];
    cJSON *params;
} minihub_command_t;

typedef esp_err_t (*minihub_command_handler_t)(const minihub_command_t *cmd);

/* Point registry */
esp_err_t minihub_point_register(const minihub_point_t *point);
esp_err_t minihub_point_unregister(const char *point_id);
esp_err_t minihub_point_unregister_by_adapter(const char *adapter);
esp_err_t minihub_point_set_online(const char *point_id, bool online);
minihub_point_t *minihub_point_get(const char *point_id);
minihub_point_t *minihub_point_get_by_index(uint16_t index);
uint16_t minihub_point_index_of(const char *point_id);
esp_err_t minihub_point_list(minihub_point_t **out, size_t *count);
esp_err_t minihub_point_list_by_type(const char *type, minihub_point_t **out, size_t *count);
esp_err_t minihub_point_list_by_adapter(const char *adapter, minihub_point_t **out, size_t *count);

/* State store */
esp_err_t minihub_point_set(const char *point_id, const char *value, cJSON *props);
esp_err_t minihub_point_get_snapshot(const char *point_id, minihub_point_t *out);
bool minihub_point_value_changed(const char *point_id, const char *old_value, const char *new_value);

/* Per-point value-change history (RAM ring; reboot clears) */
void minihub_point_history_init(void);
void minihub_point_history_record(const char *point_id, const char *old_value, const char *new_value);
/** Caller owns returned JSON object: { "items": [ {ts_ms,old,new,source:"local"}, ... ] } */
cJSON *minihub_point_history_json(const char *point_id, size_t limit);

/* Event bus */
esp_err_t minihub_event_bus_init(void);
esp_err_t minihub_event_bus_subscribe(const char *event_type, minihub_event_handler_t handler, void *user_data);
esp_err_t minihub_event_bus_unsubscribe(const char *event_type, minihub_event_handler_t handler);
esp_err_t minihub_event_bus_publish(const char *event_type, cJSON *data);
esp_err_t minihub_event_bus_publish_point_updated(uint16_t point_index, const char *old_value, const char *new_value);
uint32_t minihub_event_bus_dropped_count(void);
cJSON *minihub_event_bus_recent_json(void);

/* Command router */
esp_err_t minihub_command_router_init(void);
esp_err_t minihub_command_register_adapter(const char *adapter, minihub_command_handler_t handler);
esp_err_t minihub_command_clear_handlers(void);
esp_err_t minihub_command_invoke(const minihub_command_t *cmd);

/* Core init / task */
esp_err_t minihub_core_init(void);
esp_err_t minihub_core_task_start(void);

/* Heap diagnostics */
void minihub_heap_log(void);
esp_err_t minihub_heap_monitor_start(void);
