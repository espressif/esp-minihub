/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "minihub_plugin_sdk/esp_idf.h"
#include "minihub_plugin_sdk/cjson.h"


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file minihub.h
 * @brief Plugin SDK — MiniHub Host exports (points, ABI, config, HTTP worker, net, BLE GATT).
 *
 * Prefer the Host vtable (``minihub_module_host_api_t``) for Core services.
 * Each API carries an ``@note`` with its MiniHub component header origin.
 * Programming guide documents the same contracts via component headers
 * (``minihub_core``, ``module_abi``, ``minihub_ble_gatt``, …).
 */

/** @brief Host ↔ module binary ABI version; must match ``module.json`` ``abi_version``.
 *  @note MiniHub: ``components/minihub_module/include/minihub_module_abi.h`` */
#define MINIHUB_MODULE_ABI_VERSION  1
/** @brief Core API version; ``module.json`` ``core_api_min`` / ``core_api_max`` must cover Host.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_CORE_API_VERSION    1
/** @brief Maximum ``point_id`` string length (including NUL).
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_POINT_ID_MAX        64
/** @brief Maximum point type string length.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_TYPE_MAX            16
/** @brief Maximum point value string length.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_VALUE_MAX           32
/** @brief Maximum adapter name string length.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_ADAPTER_MAX         16
/** @brief Maximum command name string length.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_CMD_MAX             32
/** @brief Maximum event type string length.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
#define MINIHUB_EVENT_MAX           32

/**
 * @brief Device / state record exposed on REST ``/api/points``.
 *
 * ``props`` is owned by Core after register (Host copies). Do not free Host
 * pointers from ``point_get``.
 * @note MiniHub: ``components/minihub_core/include/minihub_core.h``
 */
typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX]; /**< Stable id */
    char type[MINIHUB_TYPE_MAX];         /**< e.g. sensor, switch */
    char value[MINIHUB_VALUE_MAX];       /**< String snapshot */
    cJSON *props;                        /**< Optional properties */
    char adapter[MINIHUB_ADAPTER_MAX];   /**< Owning adapter name */
    bool online;
    int64_t updated_ms;
} minihub_point_t;

/** @brief Event bus payload (also mirrored on WebSocket ``/ws/events``).
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
typedef struct {
    char event_type[MINIHUB_EVENT_MAX];
    cJSON *data;
    int64_t timestamp_ms;
    uint16_t point_index;
    char old_value[MINIHUB_VALUE_MAX];
    char new_value[MINIHUB_VALUE_MAX];
} minihub_event_t;

/** @brief Event bus subscriber callback.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
typedef void (*minihub_event_handler_t)(const minihub_event_t *event, void *user_data);

/** @brief Command routed to an adapter ``handle_command``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char cmd[MINIHUB_CMD_MAX];
    cJSON *params;
} minihub_command_t;

/** @brief Adapter command handler type.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
typedef esp_err_t (*minihub_command_handler_t)(const minihub_command_t *cmd);

/**
 * @brief Adapter vtable returned from ``minihub_module_entry``.
 *
 * Host calls init → start → poll / handle_command → stop → deinit.
 * @note MiniHub: ``components/minihub_adapter/include/minihub_adapter.h``
 */
typedef struct {
    const char *name;    /**< Matches ``adapters.json`` key / ``module.json`` adapter */
    const char *version;
    esp_err_t (*init)(const cJSON *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    esp_err_t (*deinit)(void);
    esp_err_t (*handle_command)(const minihub_command_t *cmd);
    esp_err_t (*poll)(void);
} minihub_adapter_t;

/**
 * @brief Host services passed into ``minihub_module_entry``.
 *
 * Prefer these over linking Core. Check ``struct_size`` before calling fields
 * appended in newer Host versions.
 * @note MiniHub: ``components/minihub_module/include/minihub_module_abi.h``
 */
typedef struct {
    uint32_t struct_size;      /**< Bytes filled by Host */
    uint32_t abi_version;      /**< Must equal MINIHUB_MODULE_ABI_VERSION */
    uint32_t core_api_version; /**< Host MINIHUB_CORE_API_VERSION */
    esp_err_t (*point_register)(const minihub_point_t *point);
    esp_err_t (*point_unregister)(const char *point_id);
    minihub_point_t *(*point_get)(const char *point_id);
    esp_err_t (*point_set)(const char *point_id, const char *value, cJSON *props);
    esp_err_t (*command_register_adapter)(const char *adapter_name, minihub_command_handler_t handler);
    esp_err_t (*event_publish)(const char *event_type, cJSON *data);
    cJSON *(*config_get_adapters)(void);
    esp_err_t (*config_save_adapters)(cJSON *root);
    esp_err_t (*hub_reload)(void);
    esp_err_t (*adapter_soft_reload)(const char *adapter_name);
    /**
     * Read a relative file from this module's Store install directory
     * (adapter_name → registry → manifest parent). Rejects absolute / ``..``.
     * On success ``*out`` is malloc'd (caller frees); ``out_len`` may be NULL.
     */
    esp_err_t (*module_read_file)(const char *adapter_name, const char *rel_name,
                                  char **out, size_t *out_len);
    /**
     * Subscribe to Core event bus (e.g. ``point.updated`` / ``point.registered``).
     * Appended at tail — check ``struct_size`` before calling on older Hosts.
     */
    esp_err_t (*event_subscribe)(const char *event_type, minihub_event_handler_t handler, void *user_data);

    /**
     * Merge ``props.params`` only (keeps identity / ota top-level keys).
     * Appended at tail — check ``struct_size`` before calling on older Hosts.
     */
    esp_err_t (*point_merge_params)(const char *point_id, const char *value, cJSON *params);

    /**
     * Shallow-merge top-level props keys (identity / ``ota`` / …).
     * Appended at tail — check ``struct_size`` before calling on older Hosts.
     */
    esp_err_t (*point_merge_props)(const char *point_id, const char *value, cJSON *props_patch);
} minihub_module_host_api_t;

/**
 * @brief Optional module RPC (``POST /api/modules/{id}/rpc``).
 *
 * Export symbol ``minihub_module_rpc`` or register via ``minihub_module_register_rpc``.
 * @note MiniHub: ``components/minihub_module/include/minihub_module_abi.h``
 */
typedef esp_err_t (*minihub_module_rpc_fn)(const char *method, const cJSON *params, cJSON **out);

/**
 * @brief Required plugin entry (ELF symbol ``minihub_module_entry``).
 *
 * @param host   Host vtable (retain for later calls)
 * @param config Per-adapter object from ``adapters.json`` (may be NULL)
 * @param[out] out_adapter Adapter vtable to register; must remain valid while loaded
 * @return
 *     - ESP_OK: succeed
 *     - ESP_ERR_INVALID_ARG: invalid argument
 * @note MiniHub: ``components/minihub_module/include/minihub_module_abi.h``
 */
esp_err_t minihub_module_entry(const minihub_module_host_api_t *host, const cJSON *config,
                               const minihub_adapter_t **out_adapter);

/** @brief Register or update a point (Host copies ``props`` if non-NULL).
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_register(const minihub_point_t *point);
/** @brief Look up point; returns Core-owned storage (do not free).
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
minihub_point_t *minihub_point_get(const char *point_id);
/** @brief Copy point into caller buffer.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_get_snapshot(const char *point_id, minihub_point_t *out);
/** @brief Update value and publish change events.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_set(const char *point_id, const char *value, cJSON *props);
/**
 * @brief Replace ``props.params`` while keeping other top-level props keys.
 * @note MiniHub: ``components/minihub_core/include/minihub_core.h``
 */
esp_err_t minihub_point_merge_params(const char *point_id, const char *value, cJSON *params);
/**
 * @brief Shallow-merge top-level keys from ``props_patch`` into existing props.
 * @note MiniHub: ``components/minihub_core/include/minihub_core.h``
 */
esp_err_t minihub_point_merge_props(const char *point_id, const char *value, cJSON *props_patch);
/** @brief Set online flag.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_set_online(const char *point_id, bool online);
/** @brief Remove all points owned by ``adapter``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_unregister_by_adapter(const char *adapter);
/** @brief Heap-allocated list of points for ``adapter``; caller ``minihub_point_list_free``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_point_list_by_adapter(const char *adapter, minihub_point_t **out, size_t *count);
/** @brief Free a list returned by ``minihub_point_list*``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
void minihub_point_list_free(minihub_point_t *list, size_t count);
/** @brief Dispatch a command to the adapter that owns ``cmd->point_id``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_command_invoke(const minihub_command_t *cmd);

/** @brief Subscribe to an event type (e.g. ``point.updated`` / ``point.registered``).
 *  Prefer vtable ``event_subscribe``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_core.h`` */
esp_err_t minihub_event_bus_subscribe(const char *event_type, minihub_event_handler_t handler,
                                      void *user_data);

/** @brief Heap snapshot of ``adapters.json`` root; caller ``cJSON_Delete``. Prefer vtable.
 *  @note MiniHub: ``components/minihub_config/include/minihub_config.h`` */
cJSON *minihub_config_get_adapters(void);
/** @brief Persist ``adapters.json``. Prefer vtable ``config_save_adapters``.
 *  @note MiniHub: ``components/minihub_config/include/minihub_config.h`` */
esp_err_t minihub_config_save_adapters(cJSON *adapters_root);
/** @brief Full hub reload (config, packs, modules). Prefer vtable ``hub_reload``.
 *  @note MiniHub: ``components/minihub_hub/include/minihub_hub.h`` */
esp_err_t minihub_hub_reload(void);
/** @brief True if any uplink has IPv4.
 *  @note MiniHub: ``components/minihub_net/include/minihub_net.h`` */
bool minihub_net_has_ip(void);
/** @brief Log DNS / optionally resolve ``hostname`` (NULL = dump only).
 *  @note MiniHub: ``components/minihub_net/include/minihub_net.h`` */
void minihub_net_dns_probe(const char *hostname);

/** @brief Work callback for ``minihub_http_run_worker``.
 *  @note MiniHub: ``components/minihub_core/include/minihub_http_worker.h`` */
typedef esp_err_t (*minihub_http_worker_fn_t)(void *ctx);
/** @brief Completion callback for async HTTP worker.
 *  @note MiniHub: ``components/minihub_core/include/minihub_http_worker.h`` */
typedef void (*minihub_http_worker_done_cb_t)(esp_err_t result, void *user_ctx);

/**
 * @brief Run TLS/HTTP work on a dedicated task (avoids httpd stack overflow).
 *
 * Blocks until ``fn`` returns.
 * @note MiniHub: ``components/minihub_core/include/minihub_http_worker.h``
 */
esp_err_t minihub_http_run_worker(minihub_http_worker_fn_t fn, void *ctx);
/** @brief Queue HTTPS work; ``cb`` runs on the worker task when finished.
 *  @note MiniHub: ``components/minihub_core/include/minihub_http_worker.h`` */
esp_err_t minihub_http_run_worker_async(minihub_http_worker_fn_t fn, void *ctx,
                                        minihub_http_worker_done_cb_t cb, void *user_ctx);
/** @brief True when called from the HTTP worker task (safe to run HTTPS inline).
 *  @note MiniHub: ``components/minihub_core/include/minihub_http_worker.h`` */
bool minihub_http_is_worker_task(void);

/** @brief Fill a host API table (tests / static attach). Prefer the pointer from entry.
 *  @note MiniHub: ``components/minihub_module/include/minihub_module.h`` */
esp_err_t minihub_module_host_api_fill(minihub_module_host_api_t *api);
/**
 * @brief Read a relative file from the Store install dir of ``adapter_name``.
 *
 * Rejects absolute paths and ``..``. On success ``*out`` is malloc'd (caller frees).
 * @note MiniHub: ``components/minihub_module/include/minihub_module.h``
 */
esp_err_t minihub_module_read_file(const char *adapter_name, const char *rel_name,
                                   char **out, size_t *out_len);
/** @brief Register RPC for ``module_id`` (Store / Web).
 *  @note MiniHub: ``components/minihub_module/include/minihub_module.h`` */
esp_err_t minihub_module_register_rpc(const char *module_id, minihub_module_rpc_fn rpc);
/** @brief Bind module UI to an on-disk JSON file and load it.
 *  @note MiniHub: ``components/minihub_module/include/minihub_module.h`` */
esp_err_t minihub_module_set_ui_file(const char *module_id, const char *path);

/** @brief Maximum BLE advertised device name length.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
#define MINIHUB_BLE_GATT_ADV_NAME_MAX  32
/** @brief Maximum scan result records returned at once.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
#define MINIHUB_BLE_GATT_ADV_MAX       24
/** @brief Maximum service-data payload bytes in advertisement.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
#define MINIHUB_BLE_GATT_SVC_DATA_MAX  31
/** @brief Maximum manufacturer-data payload bytes in advertisement.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
#define MINIHUB_BLE_GATT_MFG_DATA_MAX  29

/** @brief One BLE advertisement record from GATT client scan.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[MINIHUB_BLE_GATT_ADV_NAME_MAX];
    bool has_uuid16;
    uint16_t uuid16;
    bool has_svc_data;
    uint16_t svc_data_uuid16;
    uint8_t svc_data_len;
    uint8_t svc_data[MINIHUB_BLE_GATT_SVC_DATA_MAX];
    bool has_mfg_data;
    uint16_t mfg_company;
    uint8_t mfg_data_len;
    uint8_t mfg_data[MINIHUB_BLE_GATT_MFG_DATA_MAX];
} minihub_ble_gatt_adv_t;

/** @brief Notify callback for ``minihub_ble_gatt_subscribe``.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
typedef void (*minihub_ble_gatt_notify_cb_t)(uint16_t conn_handle, uint16_t attr_handle,
                                             const uint8_t *data, size_t len, void *ctx);

/** @brief Init NimBLE GATT client (not BLE Mesh radio).
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_init(void);
/** @brief Tear down GATT client.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_deinit(void);
/** @brief True after successful ``minihub_ble_gatt_init``.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
bool minihub_ble_gatt_ready(void);
/** @brief Start scan; ``duration_ms`` 0 = until stop; ``filter_uuid16`` 0 = no UUID filter.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_scan_start(uint32_t duration_ms, uint16_t filter_uuid16);
/** @brief Stop BLE scan.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_scan_stop(void);
/** @brief Copy scanned advertisements into caller buffer.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_scan_get(minihub_ble_gatt_adv_t *out, size_t max, size_t *count);
/** @brief Connect to a peripheral by BD_ADDR.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_connect(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms);
/** @brief Disconnect the current GATT connection.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_disconnect(void);
/** @brief True when a GATT connection is active.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
bool minihub_ble_gatt_is_connected(void);
/** @brief NimBLE connection handle for the active link (0 if none).
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
uint16_t minihub_ble_gatt_conn_handle(void);
/** @brief Discover a primary service by 16-bit UUID.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_discover_svc(uint16_t svc_uuid16, uint32_t timeout_ms);
/** @brief Find characteristic value / CCCD handles by 16-bit UUID.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_find_char(uint16_t char_uuid16, uint16_t *val_handle, uint16_t *cccd_handle);
/** @brief Subscribe to notifications on a characteristic (write CCCD).
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_subscribe(uint16_t cccd_handle, minihub_ble_gatt_notify_cb_t cb, void *ctx);
/** @brief Write characteristic value.
 *  @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h`` */
esp_err_t minihub_ble_gatt_write(uint16_t val_handle, const uint8_t *data, size_t len, bool no_rsp);
/**
 * @brief Wait for next notify (optional ``attr_handle`` filter; 0 = any).
 * @return ESP_OK or ESP_ERR_TIMEOUT
 * @note MiniHub: ``components/minihub_ble_gatt/include/minihub_ble_gatt.h``
 */
esp_err_t minihub_ble_gatt_wait_notify(uint16_t attr_handle, uint8_t *buf, size_t buf_len,
                                       size_t *out_len, uint16_t *out_attr, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
