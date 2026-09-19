/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "minihub_core.h"
#include "minihub_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Store L3 native module binary ABI
 *
 * Modules export minihub_module_entry and use minihub_module_host_api_t to call Core
 * services without linking minihub_core.
 */

/** @brief Host ↔ module ABI version; bump on incompatible vtable changes */
#define MINIHUB_MODULE_ABI_VERSION  1

/*
 * Runtime compatibility (Store L3 .so):
 * - Same IDF target as Host (e.g. esp32s31); chip mismatch fails at dlopen/relocate.
 * - module.json abi_version must match Host; core_api_min gates the lower
 *   bound (Host core API must be >= core_api_min). core_api_max is optional:
 *   omit it (or 0) to mean "no upper bound" so older modules keep loading on
 *   newer Hosts.
 * - Host resolves undefined symbols from a shared allowlist (host_export_allowlist.json);
 *   plugins must not link the Host firmware or depend on Host Kconfig macros.
 * - PT_LOAD segment layout may vary (link flags / toolchain); Host syncs PSRAM I/D
 *   cache after dlopen and before module entry on S31-class targets.
 * - Extend minihub_module_host_api_t only at the tail; modules must honor struct_size.
 */

/** @brief Module image magic ("MHMD") */
#define MINIHUB_MODULE_MAGIC  0x4D484D44U

/** @brief Exported module entry symbol name (resolved by elf_loader) */
#define MINIHUB_MODULE_ENTRY_SYMBOL  "minihub_module_entry"

/** @brief Optional module RPC symbol (POST /api/modules/{id}/rpc) */
#define MINIHUB_MODULE_RPC_SYMBOL    "minihub_module_rpc"

/** @brief Module RPC dispatch callback (optional symbol MINIHUB_MODULE_RPC_SYMBOL) */
typedef esp_err_t (*minihub_module_rpc_fn)(const char *method, const cJSON *params, cJSON **out);

/**
 * @brief Read a file from the calling module's Store install directory.
 *
 * Resolves the install dir via the module registry (adapter_name to the
 * manifest parent dir). Does not accept absolute paths from the plugin.
 *
 * @param adapter_name Module adapter name (usually equals module id)
 * @param rel_name     Relative name under that dir (e.g. "devices.json").
 *                     Rejects absolute paths and ".." segments.
 * @param[out] out     Malloc'd NUL-terminated buffer; caller must free().
 * @param[out] out_len Optional length excluding NUL (may be NULL).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND, ESP_ERR_NO_MEM, ESP_FAIL
 */
typedef esp_err_t (*minihub_module_read_file_fn)(const char *adapter_name, const char *rel_name,
                                                 char **out, size_t *out_len);

/**
 * @brief Subscribe to the Core event bus (for example point.updated).
 *
 * Appended at the vtable tail. Check struct_size before calling on older Hosts.
 *
 * @param event_type Event name
 * @param handler    Callback
 * @param user_data  Passed through to handler
 * @return ESP_OK on success
 */
typedef esp_err_t (*minihub_module_event_subscribe_fn)(const char *event_type,
                                                       minihub_event_handler_t handler,
                                                       void *user_data);

/**
 * @brief Merge props.params only (keeps identity / ota top-level keys).
 *
 * Appended at the vtable tail. Check struct_size before calling on older Hosts.
 *
 * @param point_id Device point id
 * @param value    Optional compact summary; may be NULL
 * @param params   Params object to merge
 * @return ESP_OK on success
 */
typedef esp_err_t (*minihub_module_point_merge_params_fn)(const char *point_id, const char *value,
                                                          cJSON *params);

/**
 * @brief Shallow-merge top-level props keys (identity / ota / ...).
 *
 * Appended at the vtable tail. Check struct_size before calling on older Hosts.
 *
 * @param point_id     Device point id
 * @param value        Optional compact summary; may be NULL
 * @param props_patch  Top-level props keys to merge
 * @return ESP_OK on success
 */
typedef esp_err_t (*minihub_module_point_merge_props_fn)(const char *point_id, const char *value,
                                                         cJSON *props_patch);

/**
 * @brief Core services Host exposes to dynamic modules
 *
 * Modules must not link minihub_core; call only through this vtable.
 * Check host->struct_size before calling pointers appended in newer Host versions.
 */
typedef struct {
    uint32_t struct_size;     /**< Size of vtable filled by Host */
    uint32_t abi_version;     /**< Must equal MINIHUB_MODULE_ABI_VERSION */
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

    minihub_module_read_file_fn module_read_file;           /**< See minihub_module_read_file_fn */
    minihub_module_event_subscribe_fn event_subscribe;     /**< See minihub_module_event_subscribe_fn */
    minihub_module_point_merge_params_fn point_merge_params; /**< See minihub_module_point_merge_params_fn */
    minihub_module_point_merge_props_fn point_merge_props; /**< See minihub_module_point_merge_props_fn */
} minihub_module_host_api_t;

/** @brief Module metadata (manifest / runtime registry) */
typedef struct {
    char id[32];
    char name[64];
    char version[16];
    char adapter_name[16];
    uint32_t abi_version;
    uint32_t core_api_min;
    uint32_t core_api_max;
    bool enabled;
    bool loaded;
} minihub_module_info_t;

/**
 * @brief Required module entry export (symbol MINIHUB_MODULE_ENTRY_SYMBOL)
 */
typedef esp_err_t (*minihub_module_entry_fn)(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter);

/** @brief .mhub image header (little-endian, prepended to ELF) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t image_size;
    uint32_t entry_offset;
    uint32_t core_api_min;
    uint32_t core_api_max;
    uint8_t reserved[12];
} minihub_module_image_header_t;

#ifdef __cplusplus
}
#endif
