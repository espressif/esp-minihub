/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"
#include "minihub_module_abi.h"
#include "minihub_adapter.h"

/**
 * @brief Init module loader (scan manifest dirs, etc.).
 */
esp_err_t minihub_module_loader_init(void);

/**
 * @brief Load all enabled dynamic modules and register adapters.
 * @param adapters_root Root of adapters.json for per-module config lookup.
 */
esp_err_t minihub_module_load_all(const cJSON *adapters_root);

/**
 * @brief Load hybrid companions whose entry is JS (deferred .so after minihub_js_reload).
 * No-op when no such modules are pending.
 */
esp_err_t minihub_module_load_deferred(const cJSON *adapters_root);

/**
 * @brief Load one module image from path.
 */
esp_err_t minihub_module_load_binary(const char *manifest_path);

/**
 * @brief Unload module and unregister its adapter.
 */
esp_err_t minihub_module_unload(const char *module_id);

/**
 * @brief Rescan manifests and load (Web hot-reload).
 */
esp_err_t minihub_module_reload(const cJSON *adapters_root);

/**
 * @brief Registered module count (includes disabled manifests).
 */
size_t minihub_module_count(void);

/**
 * @brief Loaded module count.
 */
size_t minihub_module_loaded_count(void);

/**
 * @brief Module list JSON snapshot (caller cJSON_Delete).
 */
cJSON *minihub_module_list_json(void);

/**
 * @brief Register module RPC (dlsym or static adapter init).
 */
esp_err_t minihub_module_register_rpc(const char *module_id, minihub_module_rpc_fn rpc);

/**
 * @brief Drop persisted host RPC for a module id (Store uninstall / orphan cleanup).
 * Prevents hub_reload from recreating a manifest-less listing slot.
 */
esp_err_t minihub_module_forget_host_rpc(const char *module_id);

/**
 * @brief Attach Host Web auth UI metadata (same shape as Store module ui.json).
 * Used when a module is host-static / builtin and has no on-disk ui.json.
 */
esp_err_t minihub_module_set_ui_json(const char *module_id, const char *json);

/**
 * @brief Bind module UI to an on-disk JSON file and load it now.
 * Subsequent GET /api/modules re-reads the file so edits apply without rebuild.
 */
esp_err_t minihub_module_set_ui_file(const char *module_id, const char *path);

/**
 * @brief Invoke loaded module RPC; out owned by module, caller cJSON_Delete.
 */
esp_err_t minihub_module_invoke_rpc(const char *module_id, const char *method,
                                    const cJSON *params, cJSON **out);

/**
 * @brief Whether overview / filter chips should show this adapter.
 */
bool minihub_module_overview_adapter_visible(const char *adapter_name);

/** Radio exclusion result when changing adapter enable state. */
typedef enum {
    MINIHUB_ADAPTER_RADIO_OK = 0,
    MINIHUB_ADAPTER_RADIO_CONFLICT_802154,
    MINIHUB_ADAPTER_RADIO_CONFLICT_GROUP,
} minihub_adapter_radio_status_t;

/**
 * @brief True when this adapter's stack is compiled into the current firmware.
 *        Unknown names (Store modules) are treated as supported.
 */
bool minihub_module_adapter_supported(const char *name);

/**
 * @brief True when this Hub currently has the named capability.
 *        Tokens: WiFi, WiFi_SoftAP, Zigbee, BLE, BLE_mesh, Thread_Matter,
 *        Matter, MQTT_Broker, Modbus. WiFi follows the wifi STA adapter.
 *        WiFi_SoftAP is reserved for a lasting Hub SoftAP / AP-router mode
 *        (always off until that mode ships). BLE and BLE_mesh both follow
 *        ble_mesh. A token is on only when its adapter is compiled in and
 *        enabled (or, for reserved SoftAP, when that mode is active).
 */
bool minihub_module_capability_on(const char *token);

/**
 * @brief True when every name in a module.json requires array is on.
 *        A missing or empty array matches every Hub.
 */
bool minihub_module_requires_met(const cJSON *requires_json);

/**
 * @brief Comma-separated capabilities this Hub currently has. Empty if none.
 */
void minihub_module_format_caps(char *out, size_t out_len);

/**
 * @brief Check whether enabling @p name conflicts with adapters already on in
 *        @p adapters_root. Shared by Web (/api/adapters) and LCD config UI.
 */
minihub_adapter_radio_status_t
minihub_module_adapter_radio_check(const cJSON *adapters_root, const char *name);

/**
 * @brief Validate a whole adapters document for radio conflicts.
 */
minihub_adapter_radio_status_t
minihub_module_adapters_radio_validate(const cJSON *adapters_root);

/**
 * @brief True if the module for this adapter exposes auth UI
 *        (`module.json` capabilities contains "auth", or `ui.json` `auth:true`); false otherwise.
 */
bool minihub_module_adapter_has_auth(const char *adapter_name);

/**
 * @brief First overview-visible adapter with auth capability.
 * @return True if an adapter was selected and `out` was filled; false otherwise.
 */
bool minihub_module_pick_auth_adapter(char *out, size_t out_len);

/**
 * @brief Optional: copy ui.json string label (e.g. "login_title"); false if missing.
 */
bool minihub_module_ui_label(const char *adapter_name, const char *key,
                             char *out, size_t out_len);

/**
 * @brief Per-adapter overview visibility map (caller cJSON_Delete). NULL if the lifecycle lock is busy.
 */
cJSON *minihub_module_overview_adapters_json(void);

/**
 * @brief Enable/disable a manifest module.
 */
esp_err_t minihub_module_set_enabled(const char *id, bool enabled);

/**
 * @brief Clear `/store/modules/<id>/.load_guard` so the next automatic load may retry.
 * Used after a crash left the guard stuck in "loading".
 */
esp_err_t minihub_module_clear_load_guard(const char *id);

/**
 * @brief Delete manifest file and rescan.
 */
esp_err_t minihub_module_delete(const char *id);

/**
 * @brief Fill Host API table for module entry or tests.
 */
esp_err_t minihub_module_host_api_fill(minihub_module_host_api_t *api);

/**
 * @brief Read a relative file from the Store install dir of the module owning
 * `adapter_name` (manifest parent of `/store/modules/<id>/`).
 *
 * `rel_name` must be relative: no leading `/`, no `..`. On success
 * `*out` is a malloc'd NUL-terminated buffer; the caller frees it.
 */
esp_err_t minihub_module_read_file(const char *adapter_name, const char *rel_name,
                                   char **out, size_t *out_len);

/**
 * @brief Manually attach a module-exported adapter (tests / loader internals).
 * Checks abi_version and core_api range.
 */
esp_err_t minihub_module_attach_adapter(
    const minihub_module_info_t *info,
    const minihub_adapter_t *adapter);

/**
 * @brief Register a compile-time builtin module (manifest "builtin").
 */
esp_err_t minihub_module_register_builtin(const char *id, minihub_module_entry_fn entry);

/**
 * @brief Load .mhub / .so via elf_loader and resolve entry.
 */
esp_err_t minihub_module_dl_load(const char *binary_path,
                                 minihub_module_entry_fn *out_entry,
                                 minihub_module_rpc_fn *out_rpc,
                                 void **out_handle);

/**
 * @brief Like minihub_module_dl_load, then invoke minihub_module_entry on the same
 * internal-DRAM worker (avoids calling relocated SO code from PSRAM/main stacks).
 */
esp_err_t minihub_module_dl_load_and_enter(const char *binary_path,
                                           const minihub_module_host_api_t *host,
                                           const cJSON *config,
                                           const minihub_adapter_t **out_adapter,
                                           minihub_module_rpc_fn *out_rpc,
                                           void **out_handle,
                                           const char *owner);

/**
 * @brief Unload all dynamically loaded module handles.
 */
void minihub_module_dl_unload_all(void);
