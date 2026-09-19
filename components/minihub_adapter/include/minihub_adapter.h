/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include "minihub_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Static or module-exported adapter interface
 *
 * Host calls init → start → poll/handle_command → stop → deinit.
 * Register static adapters with REGISTER_ADAPTER().
 */
typedef struct {
    const char *name;    /**< Adapter name (matches adapters.json key) */
    const char *version; /**< Adapter version string */
    esp_err_t (*init)(const cJSON *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    esp_err_t (*deinit)(void);
    esp_err_t (*handle_command)(const minihub_command_t *cmd);
    esp_err_t (*poll)(void);
} minihub_adapter_t;

/**
 * @brief Register a static adapter in the .minihub_adapters linker section
 *
 * @param symbol Static minihub_adapter_t instance name
 */
#define REGISTER_ADAPTER(symbol) \
    __attribute__((used)) \
    static const minihub_adapter_t *const _adapter_reg_##symbol \
    __attribute__((section(".minihub_adapters"))) = &symbol

/**
 * @brief Initialize adapter registry
 */
esp_err_t minihub_adapter_registry_init(void);

/**
 * @brief Register an adapter (static or from a Store module)
 *
 * @param adapter Pointer to adapter structure
 *
 * @return
 *     - ESP_OK: succeed
 *     - ESP_ERR_NO_MEM: registry full
 */
esp_err_t minihub_adapter_register(const minihub_adapter_t *adapter);

/**
 * @brief Unregister an adapter (e.g. before unloading its .mhub)
 *
 * @param adapter Previously registered adapter pointer
 */
esp_err_t minihub_adapter_unregister(const minihub_adapter_t *adapter);

/**
 * @brief Call init() on all enabled adapters from config
 *
 * @param config adapters.json root object
 */
esp_err_t minihub_adapter_load_all(const cJSON *config);

/**
 * @brief Call start() on all loaded adapters
 */
esp_err_t minihub_adapter_start_all(void);

/**
 * @brief Call stop() on all adapters
 */
esp_err_t minihub_adapter_stop_all(void);

/**
 * @brief Soft-restart one adapter from current config without unloading its module
 *
 * @param name           Adapter name
 * @param adapters_root  Full adapters.json root
 */
esp_err_t minihub_adapter_soft_reload(const char *name, const cJSON *adapters_root);

/**
 * @brief Stop, reload config, and restart all adapters
 *
 * @param config adapters.json root object
 */
esp_err_t minihub_adapter_reload_all(const cJSON *config);

/**
 * @brief Start background task that calls poll() on adapters
 */
esp_err_t minihub_adapter_start_poll_task(void);

/**
 * @brief Serialize adapter lifecycle with stop/load/unload
 *
 * Hub reload should hold this lock across stop_all → module_reload → load/start
 * so poll() cannot run inside a .mhub that is about to be unloaded.
 *
 * @param timeout_ms Maximum wait for the lock (ms)
 *
 * @return
 *     - ESP_OK: lock acquired
 *     - ESP_ERR_TIMEOUT: timeout
 */
esp_err_t minihub_adapter_lifecycle_lock(uint32_t timeout_ms);

/**
 * @brief Release adapter lifecycle lock
 */
void minihub_adapter_lifecycle_unlock(void);

/**
 * @brief Find adapter by name
 *
 * @return Adapter pointer, or NULL if not found
 */
const minihub_adapter_t *minihub_adapter_find(const char *name);

/**
 * @brief Get registered adapter by index
 *
 * @return Adapter pointer, or NULL if out of range
 */
const minihub_adapter_t *minihub_adapter_get(size_t index);

/**
 * @brief Number of registered adapters
 */
size_t minihub_adapter_count(void);

/**
 * @brief Check Host MINIHUB_CORE_API_VERSION against module manifest range
 *
 * @param min_v module.json core_api_min
 * @param max_v module.json core_api_max
 *
 * @return
 *     - ESP_OK: compatible
 *     - ESP_ERR_INVALID_VERSION: incompatible
 */
static inline esp_err_t minihub_adapter_check_core_api(uint32_t min_v, uint32_t max_v)
{
    if (MINIHUB_CORE_API_VERSION < min_v || MINIHUB_CORE_API_VERSION > max_v) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
