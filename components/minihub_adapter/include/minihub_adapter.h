/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "minihub_core.h"

typedef struct {
    const char *name;
    const char *version;
    esp_err_t (*init)(const cJSON *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    esp_err_t (*deinit)(void);
    esp_err_t (*handle_command)(const minihub_command_t *cmd);
    esp_err_t (*poll)(void);
} minihub_adapter_t;

#define REGISTER_ADAPTER(symbol) \
    __attribute__((used)) \
    static const minihub_adapter_t *const _adapter_reg_##symbol \
    __attribute__((section(".minihub_adapters"))) = &symbol

esp_err_t minihub_adapter_registry_init(void);
esp_err_t minihub_adapter_register(const minihub_adapter_t *adapter);
/** Remove a previously registered adapter (e.g. before unloading its .mhub). */
esp_err_t minihub_adapter_unregister(const minihub_adapter_t *adapter);
esp_err_t minihub_adapter_load_all(const cJSON *config);
esp_err_t minihub_adapter_start_all(void);
esp_err_t minihub_adapter_stop_all(void);
/** Soft-restart a single active adapter from current config (no module unload). */
esp_err_t minihub_adapter_soft_reload(const char *name, const cJSON *adapters_root);
esp_err_t minihub_adapter_reload_all(const cJSON *config);
esp_err_t minihub_adapter_start_poll_task(void);

/**
 * Serialize adapter poll with stop/load/start and module dlclose.
 * Hub reload should hold this across stop_all → module_reload → load/start
 * so adapter_poll cannot run inside a .mhub that is about to be freed.
 */
esp_err_t minihub_adapter_lifecycle_lock(uint32_t timeout_ms);
void minihub_adapter_lifecycle_unlock(void);
const minihub_adapter_t *minihub_adapter_find(const char *name);
/** Registered adapter at index, or NULL if out of range. */
const minihub_adapter_t *minihub_adapter_get(size_t index);
size_t minihub_adapter_count(void);

static inline esp_err_t minihub_adapter_check_core_api(uint32_t min_v, uint32_t max_v)
{
    if (MINIHUB_CORE_API_VERSION < min_v || MINIHUB_CORE_API_VERSION > max_v) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}
