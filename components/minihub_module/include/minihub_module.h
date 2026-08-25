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

/** Init module loader (scan manifest dirs, etc.) */
esp_err_t minihub_module_loader_init(void);

/**
 * Load all enabled dynamic modules and register adapters.
 * @param adapters_root Root of adapters.json for per-module config lookup
 */
esp_err_t minihub_module_load_all(const cJSON *adapters_root);

/**
 * Load hybrid companions whose entry is JS (deferred .so after minihub_js_reload).
 * No-op when no such modules are pending.
 */
esp_err_t minihub_module_load_deferred(const cJSON *adapters_root);

/** Load one module image from path */
esp_err_t minihub_module_load_binary(const char *manifest_path);

/** Unload module and unregister its adapter */
esp_err_t minihub_module_unload(const char *module_id);

/** Rescan manifests and load (Web hot-reload) */
esp_err_t minihub_module_reload(const cJSON *adapters_root);

/** Registered module count (includes disabled manifests) */
size_t minihub_module_count(void);

/** Loaded module count */
size_t minihub_module_loaded_count(void);

/** Module list JSON snapshot (caller cJSON_Delete) */
cJSON *minihub_module_list_json(void);

/** Register module RPC (dlsym or static adapter init) */
esp_err_t minihub_module_register_rpc(const char *module_id, minihub_module_rpc_fn rpc);

/**
 * Attach Host Web auth UI metadata (same shape as Store module ui.json).
 * Used when a module is host-static / builtin and has no on-disk ui.json.
 */
esp_err_t minihub_module_set_ui_json(const char *module_id, const char *json);

/**
 * Bind module UI to an on-disk JSON file and load it now.
 * Subsequent GET /api/modules re-reads the file so edits apply without rebuild.
 */
esp_err_t minihub_module_set_ui_file(const char *module_id, const char *path);

/** Invoke loaded module RPC; out owned by module, caller cJSON_Delete */
esp_err_t minihub_module_invoke_rpc(const char *module_id, const char *method,
                                    const cJSON *params, cJSON **out);

/** Whether overview / filter chips should show this adapter */
bool minihub_module_overview_adapter_visible(const char *adapter_name);

/**
 * True when the module for this adapter exposes auth UI
 * (module.json capabilities contains "auth", or ui.json auth:true).
 */
bool minihub_module_adapter_has_auth(const char *adapter_name);

/**
 * First overview-visible adapter with auth capability.
 * @return true and fills out on success
 */
bool minihub_module_pick_auth_adapter(char *out, size_t out_len);

/** Optional: copy ui.json string label (e.g. "login_title"); false if missing */
bool minihub_module_ui_label(const char *adapter_name, const char *key,
                             char *out, size_t out_len);

/** Per-adapter overview visibility map (caller cJSON_Delete) */
cJSON *minihub_module_overview_adapters_json(void);

/** Enable/disable a manifest module */
esp_err_t minihub_module_set_enabled(const char *id, bool enabled);

/** Delete manifest file and rescan */
esp_err_t minihub_module_delete(const char *id);

/** Fill Host API table for module entry or tests */
esp_err_t minihub_module_host_api_fill(minihub_module_host_api_t *api);

/**
 * Manually attach a module-exported adapter (tests / loader internals).
 * Checks abi_version and core_api range.
 */
esp_err_t minihub_module_attach_adapter(
    const minihub_module_info_t *info,
    const minihub_adapter_t *adapter);

/** Register a compile-time builtin module (manifest "builtin") */
esp_err_t minihub_module_register_builtin(const char *id, minihub_module_entry_fn entry);

/** Load .mhub / .so via elf_loader and resolve entry */
esp_err_t minihub_module_dl_load(const char *binary_path,
                                 minihub_module_entry_fn *out_entry,
                                 minihub_module_rpc_fn *out_rpc,
                                 void **out_handle);

/** Unload all dynamically loaded module handles */
void minihub_module_dl_unload_all(void);
