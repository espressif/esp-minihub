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

/** Host ↔ module binary ABI version; bump on incompatible changes */
#define MINIHUB_MODULE_ABI_VERSION  1

/** Module image magic "MHMD" */
#define MINIHUB_MODULE_MAGIC  0x4D484D44U

/** Exported module entry symbol (resolved by elf_loader) */
#define MINIHUB_MODULE_ENTRY_SYMBOL  "minihub_module_entry"

/** Optional module RPC dispatch (Host /api/modules/{id}/rpc) */
#define MINIHUB_MODULE_RPC_SYMBOL    "minihub_module_rpc"

/**
 * Module RPC: method + params(JSON) → out(JSON; caller cJSON_Delete).
 * ESP_OK means out is filled; ESP_ERR_NOT_FOUND means unknown method.
 */
typedef esp_err_t (*minihub_module_rpc_fn)(const char *method, const cJSON *params, cJSON **out);

/**
 * Core services Host exposes to dynamic modules.
 * Modules must not link minihub_core; call only through this vtable.
 */
typedef struct {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t core_api_version;

    esp_err_t (*point_register)(const minihub_point_t *point);
    esp_err_t (*point_unregister)(const char *point_id);
    minihub_point_t *(*point_get)(const char *point_id);
    esp_err_t (*point_set)(const char *point_id, const char *value, cJSON *props);

    esp_err_t (*command_register_adapter)(const char *adapter_name, minihub_command_handler_t handler);
    esp_err_t (*event_publish)(const char *event_type, cJSON *data);

    /** Fields below require host struct_size >= sizeof(this) (ABI extend) */
    cJSON *(*config_get_adapters)(void);
    esp_err_t (*config_save_adapters)(cJSON *root);
    esp_err_t (*hub_reload)(void);
    /** Soft-reload one adapter (no module unload). Prefer after device sync. */
    esp_err_t (*adapter_soft_reload)(const char *adapter_name);
} minihub_module_host_api_t;

/** Module metadata (manifest / runtime registry) */
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
 * Required export for dynamic modules.
 * @param host        Host service table (read-only)
 * @param config      Adapter config from adapters.json
 * @param out_adapter Output minihub_adapter_t* (owned by the module)
 */
typedef esp_err_t (*minihub_module_entry_fn)(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter);

/** .mhub image header (little-endian) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t image_size;
    uint32_t entry_offset;
    uint32_t core_api_min;
    uint32_t core_api_max;
    uint8_t reserved[12];
} minihub_module_image_header_t;
