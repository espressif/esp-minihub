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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MiniHub Core — point registry, state store, event bus, and command router.
 *
 * Static Host adapters link this component directly. Store L3 modules must use
 * minihub_module_host_api_t instead (see minihub_module_abi.h).
 */

/**
 * @brief Core API version; modules declare compatible range in module.json.
 */
#define MINIHUB_CORE_API_VERSION  1

/**
 * @brief Maximum length of point_id string (including NUL).
 */
#define MINIHUB_POINT_ID_MAX  64
/**
 * @brief Maximum length of point type string.
 */
#define MINIHUB_TYPE_MAX      32
/**
 * @brief Maximum length of point value string snapshot.
 */
#define MINIHUB_VALUE_MAX     32
/**
 * @brief Maximum length of adapter name string.
 */
#define MINIHUB_ADAPTER_MAX   16
/**
 * @brief Maximum length of command name string.
 */
#define MINIHUB_CMD_MAX       32
/**
 * @brief Maximum length of event type string.
 */
#define MINIHUB_EVENT_MAX     32

/**
 * @brief Maximum number of registered points.
 */
#define MINIHUB_MAX_POINTS    64
/**
 * @brief Maximum number of command handlers.
 */
#define MINIHUB_MAX_HANDLERS  16
/**
 * @brief Maximum number of event bus subscriptions.
 */
#define MINIHUB_MAX_SUBS      32
/**
 * @brief Event queue depth before drops occur.
 */
#define MINIHUB_EVENT_QUEUE_DEPTH 32

/**
 * @brief Unified device/state record exposed on REST /api/points.
 */
typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX]; /*!< Stable point identifier. */
    char type[MINIHUB_TYPE_MAX];         /*!< E.g. sensor, actuator, switch. */
    char value[MINIHUB_VALUE_MAX];     /*!< String value snapshot. */
    cJSON *props;                      /*!< Optional properties (Core copies on register). */
    char adapter[MINIHUB_ADAPTER_MAX]; /*!< Owning adapter name. */
    bool online;                       /*!< Online flag. */
    int64_t updated_ms;                /*!< Last update timestamp. Measurement unit: ms. */
} minihub_point_t;

/**
 * @brief Event bus payload (mirrored on WebSocket /ws/events).
 */
typedef struct {
    char event_type[MINIHUB_EVENT_MAX]; /*!< Event name, e.g. point.updated. */
    cJSON *data;                        /*!< Optional JSON payload. */
    int64_t timestamp_ms;
    uint16_t point_index;
    char old_value[MINIHUB_VALUE_MAX];
    char new_value[MINIHUB_VALUE_MAX];
} minihub_event_t;

#ifdef DOXYGEN
/**
 * @brief Event bus subscriber callback.
 */
typedef void minihub_event_handler_t(const minihub_event_t *event, void *user_data);
#else
typedef void (*minihub_event_handler_t)(const minihub_event_t *event, void *user_data);
#endif

/**
 * @brief Command routed to an adapter handle_command handler.
 */
typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX]; /*!< Target point. */
    char cmd[MINIHUB_CMD_MAX];           /*!< Command name (adapter-specific). */
    cJSON *params;                       /*!< Optional JSON parameters. */
} minihub_command_t;

#ifdef DOXYGEN
/**
 * @brief Adapter command handler registered with minihub_command_register_adapter.
 */
typedef esp_err_t minihub_command_handler_t(const minihub_command_t *cmd);
#else
typedef esp_err_t (*minihub_command_handler_t)(const minihub_command_t *cmd);
#endif

/**
 * @brief Add or update a point in the registry.
 *
 * @param point Point record. If props is non-NULL, Core copies the JSON object.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_INVALID_ARG: Invalid argument.
 *     - ESP_ERR_NO_MEM: Registry full or allocation failed.
 */
esp_err_t minihub_point_register(const minihub_point_t *point);

/**
 * @brief Remove a single point from the registry.
 *
 * @param point_id Target point id.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point does not exist.
 */
esp_err_t minihub_point_unregister(const char *point_id);

/**
 * @brief Remove all points owned by an adapter.
 *
 * @param adapter Adapter name.
 *
 * @return
 *     - ESP_OK: Succeed.
 */
esp_err_t minihub_point_unregister_by_adapter(const char *adapter);

/**
 * @brief Update the online flag for a point.
 *
 * @param point_id Target point id.
 * @param online   New online state.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point does not exist.
 */
esp_err_t minihub_point_set_online(const char *point_id, bool online);

/**
 * @brief Look up a point by id.
 *
 * @param point_id Target point id.
 *
 * @return Pointer to Core-owned storage, or NULL if not found. Do not free.
 */
minihub_point_t *minihub_point_get(const char *point_id);

/**
 * @brief Look up a point by registry index.
 *
 * @param index Zero-based index.
 *
 * @return Pointer to Core-owned storage, or NULL if out of range.
 */
minihub_point_t *minihub_point_get_by_index(uint16_t index);

/**
 * @brief Return the registry index of a point.
 *
 * @param point_id Target point id.
 *
 * @return Index, or UINT16_MAX if not found.
 */
uint16_t minihub_point_index_of(const char *point_id);

/**
 * @brief Return a heap snapshot of all registered points.
 *
 * @param out   Output pointer; caller must free(*out).
 * @param count Output count.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Allocation failed.
 */
esp_err_t minihub_point_list(minihub_point_t **out, size_t *count);

/**
 * @brief Return points filtered by type.
 *
 * @param type  Type string filter.
 * @param out   Output pointer; caller must free(*out).
 * @param count Output count.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Allocation failed.
 */
esp_err_t minihub_point_list_by_type(const char *type, minihub_point_t **out, size_t *count);

/**
 * @brief Return points filtered by adapter name.
 *
 * @param adapter Adapter name filter.
 * @param out     Output pointer; caller must free(*out).
 * @param count   Output count.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Allocation failed.
 */
esp_err_t minihub_point_list_by_adapter(const char *adapter, minihub_point_t **out, size_t *count);
/* Free a list returned by minihub_point_list*() (frees each point's props too). */
void minihub_point_list_free(minihub_point_t *list, size_t count);

/* Internal: keep s_mutex held across an index -> pointer -> write sequence.
 * Must bracket minihub_point_index_of_locked / minihub_point_get_by_index_locked. */
void minihub_point_registry_lock(void);
void minihub_point_registry_unlock(void);
int16_t minihub_point_index_of_locked(const char *point_id);
minihub_point_t *minihub_point_get_by_index_locked(uint16_t index);

/* Control-target resolution: (data-model type, parameter) -> registered device
 * point_id, e.g. ("light", "power") -> "sim.js_kitchen" (or "thread.bulb").
 * The transport prefix is deliberately NOT stored in the device type catalog
 * (device display is protocol-independent); it comes from the device
 * REGISTRATION: point_id is <protocol>.<device>, and live param metadata lives
 * in props.params.<parameter>. Legacy per-param ids
 * (<protocol>.<device>.<parameter>) are still accepted when present.
 * Contract: the registered point MUST carry the data-model `type` (adapters set
 * point.type from their device descriptor), matching the catalog `device`.
 * Returns ESP_OK (out filled), ESP_ERR_NOT_FOUND (nothing registered for that
 * type/parameter), ESP_ERR_INVALID_ARG or ESP_ERR_INVALID_SIZE.
 * Note: when several registered points share the same type (multiple instances
 * of one device kind) this returns the FIRST match only. POST /api/devices/control
 * prefers an optional body ``point_id`` when present; this helper remains the
 * fallback. Callers keep the parameter name separate in cmd.params. */
esp_err_t minihub_point_resolve_by_type_param(const char *type, const char *parameter,
                                              char *out, size_t size);

/**
 * @brief Check whether a registered point carries a given parameter.
 *
 * True when `props.params` contains `parameter`. Used by the control
 * endpoint to authorize a command by the live device point rather than the
 * static device_widgets catalog — the latter may omit plugin-exposed
 * parameters (e.g. miot `indicator`) and must not 404 legitimate controls.
 *
 * @param point_id    Target point id.
 * @param parameter   Parameter name (data-model).
 * @return True if the point exists and carries the parameter; false otherwise.
 */
bool minihub_point_has_param(const char *point_id, const char *parameter);

/**
 * @brief Update point value and publish change events.
 *
 * @param point_id Target point id.
 * @param value    New string value.
 * @param props    Optional properties JSON; may be NULL.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point does not exist.
 */
esp_err_t minihub_point_set(const char *point_id, const char *value, cJSON *props);

/**
 * @brief Replace `props.params` while keeping other top-level props keys.
 *
 * Used by adapters that publish telemetry so plugin extras (`ota`, `ieee`,
 * `manufacturer`, …) on the same `point_id` are not wiped.
 * `params` is duplicated; caller retains ownership.
 *
 * @param point_id Target point id.
 * @param value    New summary value (NULL = keep existing).
 * @param params   Object to store under `props.params` (required).
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point does not exist.
 *     - ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM: Bad args / allocation.
 */
esp_err_t minihub_point_merge_params(const char *point_id, const char *value, cJSON *params);

/**
 * @brief Shallow-merge top-level keys from `props_patch` into existing props.
 *
 * Each key in `props_patch` replaces the same key on the live point
 * (`params` is replaced as a whole object). Other existing keys are kept.
 * Useful for plugins writing `props.ota` / identity without R-M-W races
 * against telemetry. `props_patch` is not consumed; caller retains ownership.
 *
 * @param point_id    Target point id.
 * @param value       New summary value (NULL = keep existing).
 * @param props_patch Object whose top-level keys are merged (required).
 */
esp_err_t minihub_point_merge_props(const char *point_id, const char *value, cJSON *props_patch);

/**
 * @brief Remember a display name for a point.
 *
 * Stored in /data/config/device_names.json and reapplied when the point is
 * registered or its props are replaced. 1 to 48 bytes, no control characters.
 * Does not itself update the live point.
 */
esp_err_t minihub_device_name_set(const char *point_id, const char *name);

/**
 * @brief Copy current point snapshot into caller buffer.
 *
 * @param point_id Target point id.
 * @param out      Output buffer.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point does not exist.
 */
esp_err_t minihub_point_get_snapshot(const char *point_id, minihub_point_t *out);

/**
 * @brief Compare old and new string values (dedup helper).
 *
 * @return True if values differ; false otherwise.
 */
bool minihub_point_value_changed(const char *point_id, const char *old_value, const char *new_value);

/**
 * @brief Initialize RAM ring history (cleared on reboot).
 */
void minihub_point_history_init(void);

/**
 * @brief Record one value change in RAM history.
 */
void minihub_point_history_record(const char *point_id, const char *old_value, const char *new_value);

/**
 * @brief Return value-change history as JSON.
 *
 * Caller owns returned object: { "items": [ {ts_ms, old, new, source}, ... ] }
 *
 * @param point_id Target point id.
 * @param limit    Maximum number of entries.
 *
 * @return JSON object; caller must cJSON_Delete.
 */
cJSON *minihub_point_history_json(const char *point_id, size_t limit);

/**
 * @brief Initialize the event bus queue and dispatch task.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Allocation failed.
 */
esp_err_t minihub_event_bus_init(void);

/**
 * @brief Subscribe to an event type.
 *
 * @param event_type Event name, e.g. point.updated.
 * @param handler    Callback invoked from event task.
 * @param user_data  Opaque pointer passed to handler.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Subscription table full.
 */
esp_err_t minihub_event_bus_subscribe(const char *event_type, minihub_event_handler_t handler, void *user_data);

/**
 * @brief Unsubscribe a handler from an event type.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Subscription not found.
 */
esp_err_t minihub_event_bus_unsubscribe(const char *event_type, minihub_event_handler_t handler);

/**
 * @brief Drop every subscription owned by a module (used before dlclose to avoid
 *        invoking callbacks that live in the freed .so).
 *
 * @param owner Module id (adapter name); matches the tag set via.
 *              minihub_event_bus_set_current_owner during the module's entry/start.
 */
esp_err_t minihub_event_bus_unsubscribe_owner(const char *owner);

/**
 * @brief Tag the next subscription(s) with an owning module id.
 *
 * @note Set this while a module's entry/start runs so its event callbacks are
 *       reaped on unload. Pass NULL/"" to clear (core/JS subscribers).
 */
void minihub_event_bus_set_current_owner(const char *owner);

/**
 * @brief Current owner tag applied to new subscriptions ("" if none).
 */
const char *minihub_event_bus_current_owner(void);

/**
 * @brief Publish an event on the Hub event bus.
 *
 * @note
 *      - The payload is duplicated into the queued event, so the caller keeps
 *        ownership of @p data and must free it.
 *      - Do not run lengthy work in event handlers; defer to a lower-priority task.
 *
 * @return
 *     - ESP_OK: Queued.
 *     - ESP_ERR_INVALID_ARG: `event_type` is NULL.
 *     - ESP_ERR_NO_MEM: Queue full (this event dropped).
 */
esp_err_t minihub_event_bus_publish(const char *event_type, cJSON *data);

/**
 * @brief Publish standard point.updated event.
 */
esp_err_t minihub_event_bus_publish_point_updated(uint16_t point_index, const char *old_value, const char *new_value);

/**
 * @brief Count of events dropped when the queue is full.
 */
uint32_t minihub_event_bus_dropped_count(void);

/**
 * @brief Recent events as JSON snapshot.
 *
 * @return JSON object; caller must cJSON_Delete.
 */
cJSON *minihub_event_bus_recent_json(void);

/**
 * @brief Initialize command routing table.
 */
esp_err_t minihub_command_router_init(void);

/**
 * @brief Register a command handler for an adapter name.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Handler table full.
 */
esp_err_t minihub_command_register_adapter(const char *adapter, minihub_command_handler_t handler);

/**
 * @brief Clear all command handlers (typically before reload).
 */
esp_err_t minihub_command_clear_handlers(void);

/**
 * @brief Dispatch command to the adapter that owns point_id.
 *
 * Equivalent to REST POST /api/command.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NOT_FOUND: Point or handler missing.
 */
esp_err_t minihub_command_invoke(const minihub_command_t *cmd);

/**
 * @brief Inject the adapter lifecycle lock used to serialize command dispatch
 *        with the module reload path (prevents use-after-free into a dlclosed
 *        module). Provided by the adapter component at init so core does not
 *        depend on adapter headers; core only stores the callbacks.
 *
 * @param take  acquires the (recursive) lifecycle lock.
 * @param give  releases it.
 */
void minihub_command_set_lifecycle_lock(void (*take)(void), void (*give)(void));

/**
 * @brief Pin a command handler so reload waits for it, without holding the
 *        lifecycle mutex across the handler. enter() runs before the mutex
 *        is taken to look up the handler; leave() runs after the handler
 *        returns. Either may be NULL (then the mutex stays held across the
 *        call, which is the previous behaviour).
 */
void minihub_command_set_lifecycle_call(void (*enter)(void), void (*leave)(void));

/**
 * @brief Initialize Core subsystems.
 */
esp_err_t minihub_core_init(void);

/**
 * @brief Start the point-update debounce sweep timer (trailing-edge publish).
 *
 * Only meaningful when CONFIG_MINIHUB_POINT_DEBOUNCE_MS > 0. Must be called once
 * during core init. The sweep re-publishes the final point value once the
 * debounce window elapses, so subscribers still receive eventual state.
 * No-op when debounce is disabled.
 */
esp_err_t minihub_point_debounce_init(void);

/**
 * @brief Start Core background task.
 */
esp_err_t minihub_core_task_start(void);

/**
 * @brief Log current heap usage.
 */
void minihub_heap_log(void);

/**
 * @brief Start periodic heap monitor task.
 */
esp_err_t minihub_heap_monitor_start(void);

#ifdef __cplusplus
}
#endif
