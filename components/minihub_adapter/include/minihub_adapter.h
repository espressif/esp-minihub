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
 * @brief Static or module-exported adapter interface.
 *
 * Host calls init → start → poll/handle_command → stop → deinit.
 * Register static adapters with REGISTER_ADAPTER().
 */
typedef struct {
    const char *name;    /*!< Adapter name (matches adapters.json key). */
    const char *version; /*!< Adapter version string. */
    esp_err_t (*init)(const cJSON *config);
    esp_err_t (*start)(void);
    esp_err_t (*stop)(void);
    esp_err_t (*deinit)(void);
    esp_err_t (*handle_command)(const minihub_command_t *cmd);
    esp_err_t (*poll)(void);
    /**
     * @brief Optional: re-read this adapter's config subtree and republish its points in
     * place, without stop()/start(). Use for config-only changes (a device row
     * appeared, or its status changed) — a full reload unregisters every adapter's
     * points and they only return when start_all() finishes, which blanks the
     * console for ~1.7 s. NULL means not supported; minihub_adapter_resync() then
     * returns ESP_ERR_NOT_SUPPORTED and the caller falls back to a reload.
     *
     * @note Mirrored in docs/module_sdk/include/minihub_plugin_sdk/minihub.h —
     *       keep the two in step (tools/check_plugin_sdk.py asserts sizeof).
     *       MINIHUB_MODULE_ABI_VERSION stays 1 by deliberate choice: no product is
     *       deployed yet, so L3 modules are rebuilt rather than version-gated.
     *       WARNING (R2): inserting/removing/reordering a field in the MIDDLE of this
     *       struct is an ABI break even if sizeof() is unchanged — a rebuilt L3 module
     *       compiled against the old layout will read/write garbage. Any such
     *       incompatibility MUST bump MINIHUB_MODULE_ABI_VERSION.
     */
    esp_err_t (*resync)(const cJSON *config);
} minihub_adapter_t;

/**
 * @brief Register a static adapter in the .minihub_adapters linker section.
 *
 * @param symbol Static minihub_adapter_t instance name.
 */
#define REGISTER_ADAPTER(symbol) \
    __attribute__((used)) \
    static const minihub_adapter_t *const _adapter_reg_##symbol \
    __attribute__((section(".minihub_adapters"))) = &symbol

/**
 * @brief Initialize adapter registry.
 */
esp_err_t minihub_adapter_registry_init(void);

/**
 * @brief Register an adapter (static or from a Store module).
 *
 * @param adapter Pointer to adapter structure.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_NO_MEM: Registry full.
 */
esp_err_t minihub_adapter_register(const minihub_adapter_t *adapter);

/**
 * @brief Unregister an adapter (e.g. before unloading its .mhub).
 *
 * @param adapter Previously registered adapter pointer.
 */
esp_err_t minihub_adapter_unregister(const minihub_adapter_t *adapter);

/**
 * @brief Call init() on all enabled adapters from config.
 *
 * @param config adapters.json root object.
 */
esp_err_t minihub_adapter_load_all(const cJSON *config);

/**
 * @brief Call start() on all loaded adapters.
 */
esp_err_t minihub_adapter_start_all(void);

/**
 * @brief Call stop() on all adapters.
 */
esp_err_t minihub_adapter_stop_all(void);

/**
 * @brief Soft-restart one adapter from current config without unloading its module.
 *
 * @param name           Adapter name.
 * @param adapters_root  Full adapters.json root.
 */
esp_err_t minihub_adapter_soft_reload(const char *name, const cJSON *adapters_root);

/**
 * @brief Republish one running adapter's points from current config, no restart.
 *
 * Unlike soft_reload this never calls stop()/deinit(), so the adapter's points
 * stay in the registry the whole time — a client polling the point list cannot
 * see the device disappear.
 *
 * @param name           Adapter name.
 * @param adapters_root  Full adapters.json root.
 *
 * @return
 *     - ESP_OK: Republished.
 *     - ESP_ERR_NOT_SUPPORTED: Adapter is running but has no resync().
 *     - ESP_ERR_NOT_FOUND: Adapter not running or has no config entry.
 */
esp_err_t minihub_adapter_resync(const char *name, const cJSON *adapters_root);

/**
 * @brief Stop, reload config, and restart all adapters.
 *
 * @param config adapters.json root object.
 */
esp_err_t minihub_adapter_reload_all(const cJSON *config);

/**
 * @brief Start background task that calls poll() on adapters.
 */
esp_err_t minihub_adapter_start_poll_task(void);

/**
 * @brief Framework poll hook type.
 *
 * Hooks are invoked once per adapter_poll_task tick (~5 s), AFTER the lifecycle
 * lock is released, so they must be non-blocking and must not take the lock
 * (e.g. only notify another task). A blocking hook would delay adapter polling
 * and hub reloads.
 */
typedef void (*minihub_adapter_poll_hook_t)(void);

/**
 * @brief Register a lightweight poll hook invoked every framework tick.
 *
 * @param hook Non-blocking callback.
 *
 * @return
 *     - ESP_OK: Registered / already registered.
 *     - ESP_ERR_INVALID_ARG: Hook is NULL.
 *     - ESP_ERR_NO_MEM: Hook table full.
 */
esp_err_t minihub_adapter_register_poll_hook(minihub_adapter_poll_hook_t hook);

/**
 * @brief Unregister a previously registered poll hook.
 *
 * @param hook Hook previously passed to minihub_adapter_register_poll_hook().
 */
esp_err_t minihub_adapter_unregister_poll_hook(minihub_adapter_poll_hook_t hook);

/**
 * @brief Serialize adapter lifecycle with stop/load/unload.
 *
 * Hub reload should hold this lock across stop_all → module_reload → load/start
 * so poll() cannot run inside a .mhub that is about to be unloaded.
 *
 * @param timeout_ms Maximum wait for the lock. Measurement unit: ms.
 *
 * @return
 *     - ESP_OK: Lock acquired.
 *     - ESP_ERR_TIMEOUT: Timeout.
 */
esp_err_t minihub_adapter_lifecycle_lock(uint32_t timeout_ms);

/**
 * @brief Release adapter lifecycle lock.
 */
void minihub_adapter_lifecycle_unlock(void);

/**
 * @brief Lock, then wait until in-flight poll/command calls finish.
 *
 * Returns with the lock held and new poll/command calls blocked. Use this
 * before stop/dlclose. Pair with minihub_adapter_lifecycle_unlock_quiesced().
 */
esp_err_t minihub_adapter_lifecycle_lock_quiesced(uint32_t timeout_ms);

/**
 * @brief Clear the quiesce flag and release the lock from lock_quiesced().
 */
void minihub_adapter_lifecycle_unlock_quiesced(void);

/**
 * @brief Find adapter by name.
 *
 * @return Adapter pointer, or NULL if not found.
 */
const minihub_adapter_t *minihub_adapter_find(const char *name);

/**
 * @brief Get registered adapter by index.
 *
 * @return Adapter pointer, or NULL if out of range.
 */
const minihub_adapter_t *minihub_adapter_get(size_t index);

/**
 * @brief Number of registered adapters.
 */
size_t minihub_adapter_count(void);

/**
 * @brief Check Host MINIHUB_CORE_API_VERSION against module manifest range.
 *
 * @param min_v module.json core_api_min.
 * @param max_v module.json core_api_max.
 *
 * @return
 *     - ESP_OK: Compatible.
 *     - ESP_ERR_INVALID_VERSION: Incompatible.
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
