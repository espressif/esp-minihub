/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_task.h"

#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "adapter_registry";

static const minihub_adapter_t *s_registered[16];
static size_t s_registered_count;

static const minihub_adapter_t *s_active[16];
static size_t s_active_count;

/** Radio-host adapters left running by stop_all — start_all must not call start() on these. */
static const minihub_adapter_t *s_kept_radio[16];
static size_t s_kept_radio_count;

/** Recursive: hub_reload may lock then call stop_all/soft_reload which also lock. */
static SemaphoreHandle_t s_lifecycle_mu;

static esp_err_t ensure_lifecycle_mu(void)
{
    if (s_lifecycle_mu) {
        return ESP_OK;
    }
    s_lifecycle_mu = xSemaphoreCreateRecursiveMutex();
    return s_lifecycle_mu ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t minihub_adapter_lifecycle_lock(uint32_t timeout_ms)
{
    if (ensure_lifecycle_mu() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lifecycle_mu, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void minihub_adapter_lifecycle_unlock(void)
{
    if (s_lifecycle_mu) {
        xSemaphoreGiveRecursive(s_lifecycle_mu);
    }
}

static bool adapter_is_radio_host(const minihub_adapter_t *adapter)
{
    if (!adapter || !adapter->name) {
        return false;
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (strcmp(adapter->name, "zigbee") == 0) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (strcmp(adapter->name, "ble_mesh") == 0) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    if (strcmp(adapter->name, "thread") == 0) {
        return true;
    }
#endif
    return false;
}

esp_err_t minihub_adapter_registry_init(void)
{
    s_registered_count = 0;
    s_active_count = 0;
    s_kept_radio_count = 0;
    return ensure_lifecycle_mu();
}

esp_err_t minihub_adapter_register(const minihub_adapter_t *adapter)
{
    if (!adapter || !adapter->name) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_registered_count; i++) {
        const minihub_adapter_t *existing = s_registered[i];
        if (existing == adapter) {
            return ESP_OK;
        }
        if (existing && existing->name && strcmp(existing->name, adapter->name) == 0) {
            ESP_LOGW(TAG, "adapter '%s' already registered (v%s); skip duplicate v%s",
                     adapter->name,
                     existing->version ? existing->version : "?",
                     adapter->version ? adapter->version : "?");
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_registered_count >= 16) {
        return ESP_ERR_NO_MEM;
    }
    s_registered[s_registered_count++] = adapter;
    return ESP_OK;
}

esp_err_t minihub_adapter_unregister(const minihub_adapter_t *adapter)
{
    if (!adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    size_t write = 0;
    bool found = false;
    for (size_t i = 0; i < s_registered_count; i++) {
        if (s_registered[i] == adapter) {
            found = true;
            continue;
        }
        s_registered[write++] = s_registered[i];
    }
    s_registered_count = write;

    write = 0;
    for (size_t i = 0; i < s_active_count; i++) {
        if (s_active[i] == adapter) {
            continue;
        }
        s_active[write++] = s_active[i];
    }
    s_active_count = write;

    minihub_adapter_lifecycle_unlock();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static const minihub_adapter_t **adapter_ptr_begin(void)
{
    return (const minihub_adapter_t **)s_registered;
}

static const minihub_adapter_t **adapter_ptr_end(void)
{
    return (const minihub_adapter_t **)&s_registered[s_registered_count];
}

const minihub_adapter_t *minihub_adapter_get(size_t index)
{
    if (index >= s_registered_count) {
        return NULL;
    }
    return s_registered[index];
}

const minihub_adapter_t *minihub_adapter_find(const char *name)
{
    size_t count = minihub_adapter_count();
    for (size_t i = 0; i < count; i++) {
        const minihub_adapter_t *adapter = minihub_adapter_get(i);
        if (adapter && adapter->name && strcmp(adapter->name, name) == 0) {
            return adapter;
        }
    }
    return NULL;
}

size_t minihub_adapter_count(void)
{
    return (size_t)(adapter_ptr_end() - adapter_ptr_begin());
}

esp_err_t minihub_adapter_load_all(const cJSON *config)
{
    if (!config || !cJSON_IsObject(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *adapters = cJSON_GetObjectItem(config, "adapters");
    if (!adapters || !cJSON_IsObject(adapters)) {
        ESP_LOGW(TAG, "no adapters section in config");
        return ESP_OK;
    }

    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    /* Preserve radio-host adapters left active by stop_all (Zigbee / BLE Mesh).
     * Drop them if adapters.json now has enabled=false; otherwise refresh init
     * (packs / provision policy) without tearing the radio stack down. */
    const minihub_adapter_t *survivors[16];
    size_t survivor_in = s_active_count < 16 ? s_active_count : 16;
    size_t survivor_count = 0;
    for (size_t i = 0; i < survivor_in; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (!adapter || !adapter->name) {
            continue;
        }
        const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter->name);
        bool enabled = true;
        if (cJSON_IsObject(adapter_cfg)) {
            const cJSON *enabled_j = cJSON_GetObjectItem(adapter_cfg, "enabled");
            if (cJSON_IsBool(enabled_j) && !cJSON_IsTrue(enabled_j)) {
                enabled = false;
            }
        } else if (!adapter_cfg) {
            enabled = false;
        }
        if (!enabled) {
            ESP_LOGI(TAG, "radio-host adapter '%s' disabled in adapters.json — stopping",
                     adapter->name);
            if (adapter->stop) {
                adapter->stop();
            }
            /* Skip deinit: radio stacks keep controller/mesh state for a later
             * re-enable; freeing while NimBLE/Zigbee still runs caused crashes. */
            minihub_point_unregister_by_adapter(adapter->name);
            continue;
        }
        if (adapter->init && adapter_cfg) {
            esp_err_t err = adapter->init(adapter_cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "radio-host '%s' config refresh failed: %s",
                         adapter->name, esp_err_to_name(err));
            }
        }
        survivors[survivor_count++] = adapter;
    }

    s_active_count = 0;
    for (size_t i = 0; i < survivor_count && s_active_count < 16; i++) {
        s_active[s_active_count++] = survivors[i];
    }

    size_t count = minihub_adapter_count();
    for (size_t idx = 0; idx < count && s_active_count < 16; idx++) {
        const minihub_adapter_t *adapter = minihub_adapter_get(idx);
        if (!adapter) {
            continue;
        }
        bool already = false;
        for (size_t s = 0; s < s_active_count; s++) {
            if (s_active[s] == adapter) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }

        const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter->name);
        if (!adapter_cfg) {
            continue;
        }

        if (cJSON_IsObject(adapter_cfg)) {
            const cJSON *enabled = cJSON_GetObjectItem(adapter_cfg, "enabled");
            if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) {
                ESP_LOGI(TAG, "adapter '%s' disabled", adapter->name);
                continue;
            }
        }

        esp_err_t err = ESP_OK;
        if (adapter->init) {
            err = adapter->init(adapter_cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "adapter '%s' init failed: %s", adapter->name, esp_err_to_name(err));
                continue;
            }
        }

        s_active[s_active_count++] = adapter;
        ESP_LOGI(TAG, "loaded adapter '%s' v%s", adapter->name, adapter->version ? adapter->version : "?");
    }

    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_start_all(void)
{
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < s_active_count; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (!adapter->start) {
            continue;
        }
        /* Only skip start for radio hosts that were actually kept running.
         * Newly enabled radio (e.g. zigbee→ble_mesh) must still call start(). */
        bool was_kept = false;
        for (size_t k = 0; k < s_kept_radio_count; k++) {
            if (s_kept_radio[k] == adapter) {
                was_kept = true;
                break;
            }
        }
        if (was_kept) {
            if (adapter->handle_command) {
                minihub_command_register_adapter(adapter->name, adapter->handle_command);
            }
            continue;
        }
        esp_err_t err = adapter->start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adapter '%s' start failed: %s", adapter->name, esp_err_to_name(err));
            continue;
        }
        if (adapter->handle_command) {
            minihub_command_register_adapter(adapter->name, adapter->handle_command);
        }
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "adapter", adapter->name);
        minihub_event_bus_publish("adapter.loaded", data);
        cJSON_Delete(data);
    }
    s_kept_radio_count = 0;
    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_stop_all(void)
{
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    const minihub_adapter_t *kept[16];
    size_t kept_count = 0;
    s_kept_radio_count = 0;

    for (size_t i = 0; i < s_active_count; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (adapter_is_radio_host(adapter)) {
            if (kept_count < 16) {
                kept[kept_count++] = adapter;
            }
            if (s_kept_radio_count < 16) {
                s_kept_radio[s_kept_radio_count++] = adapter;
            }
            ESP_LOGI(TAG, "keeping radio-host adapter '%s' across reload", adapter->name);
            continue;
        }
        if (adapter->stop) {
            adapter->stop();
        }
        if (adapter->deinit) {
            adapter->deinit();
        }
        if (adapter->name) {
            minihub_point_unregister_by_adapter(adapter->name);
        }
    }
    minihub_command_clear_handlers();
    s_active_count = 0;
    for (size_t i = 0; i < kept_count; i++) {
        s_active[s_active_count++] = kept[i];
        if (kept[i]->handle_command) {
            minihub_command_register_adapter(kept[i]->name, kept[i]->handle_command);
        }
    }
    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_soft_reload(const char *name, const cJSON *adapters_root)
{
    if (!name || !adapters_root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    const minihub_adapter_t *adapter = minihub_adapter_find(name);
    if (!adapter) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (adapter_is_radio_host(adapter)) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    const cJSON *adapter_cfg = adapters ? cJSON_GetObjectItem(adapters, name) : NULL;
    if (!adapter_cfg) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (adapter->stop) {
        adapter->stop();
    }
    if (adapter->deinit) {
        adapter->deinit();
    }
    minihub_point_unregister_by_adapter(name);

    esp_err_t err = ESP_OK;
    if (adapter->init) {
        err = adapter->init(adapter_cfg);
        if (err != ESP_OK) {
            minihub_adapter_lifecycle_unlock();
            return err;
        }
    }
    if (adapter->start) {
        err = adapter->start();
        if (err != ESP_OK) {
            minihub_adapter_lifecycle_unlock();
            return err;
        }
    }
    if (adapter->handle_command) {
        minihub_command_register_adapter(name, adapter->handle_command);
    }
    ESP_LOGI(TAG, "soft-reloaded adapter '%s'", name);
    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_reload_all(const cJSON *config)
{
    esp_err_t err = minihub_adapter_stop_all();
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_adapter_load_all(config);
    if (err != ESP_OK) {
        return err;
    }
    return minihub_adapter_start_all();
}

static void adapter_poll_task(void *arg)
{
    (void)arg;
    while (true) {
        if (minihub_adapter_lifecycle_lock(60000) == ESP_OK) {
            for (size_t i = 0; i < s_active_count; i++) {
                const minihub_adapter_t *adapter = s_active[i];
                if (adapter && adapter->poll) {
                    adapter->poll();
                }
            }
            minihub_adapter_lifecycle_unlock();
        } else {
            ESP_LOGW(TAG, "adapter poll skipped — lifecycle lock busy");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t minihub_adapter_start_poll_task(void)
{
    const uint32_t stack_words = 8192;
    BaseType_t ok = minihub_task_create_psram(adapter_poll_task, "adapter_poll", stack_words, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
