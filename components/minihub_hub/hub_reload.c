/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_hub.h"

#include "minihub_adapter.h"
#include "minihub_config.h"
#include "minihub_js.h"
#include "minihub_module.h"
#include "minihub_pack.h"
#include "minihub_rainmaker.h"
#include "minihub_rules.h"
#include "minihub_scenes.h"
#include "minihub_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "minihub_hub";

/* MCP latches hub-ready=false around async reload. Strong version lives in
 * minihub_mcp_server; keep a weak stub so host builds without MCP still link. */
void __attribute__((weak)) minihub_hub_reload_done(esp_err_t err)
{
    (void)err;
}

static SemaphoreHandle_t s_reload_mu;
static TaskHandle_t s_reload_task;
static volatile bool s_reload_pending;
static volatile bool s_reload_busy;
static volatile bool s_soft_pending;
#define SOFT_RELOAD_QUEUE_MAX 4
static char s_soft_queue[SOFT_RELOAD_QUEUE_MAX][32];
static volatile size_t s_soft_queue_len;
static portMUX_TYPE s_soft_mux = portMUX_INITIALIZER_UNLOCKED;

static void hub_reload_task(void *arg);

static esp_err_t hub_soft_reload_locked(const char *adapter_name)
{
    if (!adapter_name || !adapter_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * Use the in-memory adapters tree. Full minihub_config_reload() deletes
     * s_adapters_cfg and races concurrent get_adapters() walkers (module sync /
     * RPC / RainMaker) → cJSON UAF. Saves already update the live tree.
     */
    cJSON *adapters = minihub_config_get_adapters();
    if (!adapters) {
        return ESP_FAIL;
    }
    esp_err_t err = minihub_adapter_soft_reload(adapter_name, adapters);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "soft reload '%s' failed: %s", adapter_name, esp_err_to_name(err));
        return err;
    }
    /* Point events may still be queued; force RainMaker prune + config report. */
    esp_err_t sync_err = minihub_rainmaker_sync_points();
    if (sync_err != ESP_OK && sync_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "rainmaker sync after soft reload: %s", esp_err_to_name(sync_err));
    }
    ESP_LOGI(TAG, "soft reload complete (%s)", adapter_name);
    return ESP_OK;
}

static esp_err_t hub_reload_locked(void)
{
    esp_err_t err = minihub_config_reload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config reload failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *adapters = minihub_config_get_adapters();
    if (!adapters) {
        return ESP_FAIL;
    }

    err = minihub_pack_reload(adapters);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pack reload failed: %s", esp_err_to_name(err));
    }

    /* Hold lifecycle lock across stop → dlclose → load/start so adapter_poll
     * cannot execute inside a dynamic module that is being freed. */
    minihub_js_unload_all();
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        ESP_LOGE(TAG, "adapter lifecycle lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    err = minihub_adapter_stop_all();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adapter stop failed: %s", esp_err_to_name(err));
    }

    /* module_reload unregisters DL adapters then dlclose — do not unload first. */
    err = minihub_module_reload(adapters);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "module reload failed: %s", esp_err_to_name(err));
    }

    err = minihub_adapter_load_all(adapters);
    if (err != ESP_OK) {
        minihub_adapter_lifecycle_unlock();
        ESP_LOGE(TAG, "adapter load failed: %s", esp_err_to_name(err));
        return err;
    }

    err = minihub_adapter_start_all();
    if (err != ESP_OK) {
        minihub_adapter_lifecycle_unlock();
        ESP_LOGE(TAG, "adapter start failed: %s", esp_err_to_name(err));
        return err;
    }

    minihub_adapter_lifecycle_unlock();

    /* L2 after built-in + L3 + pack merge (adapters already started). */
    minihub_js_unload_all();
    err = minihub_js_reload();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JS plugin reload failed: %s", esp_err_to_name(err));
    }

    err = minihub_module_load_deferred(adapters);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deferred module load failed: %s", esp_err_to_name(err));
    }

    err = minihub_rules_reload(minihub_config_get_rules());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rules reload failed: %s", esp_err_to_name(err));
    }

    err = minihub_scenes_reload(minihub_config_get_scenes());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scenes reload failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "hub reload complete");
    return ESP_OK;
}

esp_err_t minihub_hub_reload(void)
{
    if (!s_reload_mu) {
        s_reload_mu = xSemaphoreCreateMutex();
        if (!s_reload_mu) {
            minihub_hub_reload_done(ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_reload_mu, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "hub reload busy — timed out waiting");
        minihub_hub_reload_done(ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    s_reload_busy = true;
    esp_err_t err = hub_reload_locked();
    s_reload_busy = false;
    xSemaphoreGive(s_reload_mu);
    minihub_hub_reload_done(err);
    return err;
}

static esp_err_t ensure_reload_task(void)
{
    if (s_reload_task) {
        return ESP_OK;
    }
    BaseType_t ok = minihub_task_create_psram(hub_reload_task, "hub_reload", 8192, NULL, 4, &s_reload_task);
    if (ok != pdPASS) {
        s_reload_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void hub_reload_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_reload_pending || s_soft_pending) {
            if (s_reload_pending) {
                s_reload_pending = false;
                esp_err_t err = minihub_hub_reload();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "async hub reload failed: %s", esp_err_to_name(err));
                }
                continue;
            }
            if (s_soft_pending) {
                char name[32];
                name[0] = '\0';
                portENTER_CRITICAL(&s_soft_mux);
                if (s_soft_queue_len > 0) {
                    memcpy(name, s_soft_queue[0], sizeof(name));
                    memmove(s_soft_queue[0], s_soft_queue[1],
                            (s_soft_queue_len - 1) * sizeof(s_soft_queue[0]));
                    s_soft_queue_len--;
                    s_soft_pending = (s_soft_queue_len > 0);
                } else {
                    s_soft_pending = false;
                }
                portEXIT_CRITICAL(&s_soft_mux);
                if (name[0]) {
                    esp_err_t err = minihub_hub_soft_reload_adapter(name);
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "async soft reload '%s' failed: %s", name,
                                 esp_err_to_name(err));
                    }
                }
            }
        }
    }
}

esp_err_t minihub_hub_soft_reload_adapter(const char *adapter_name)
{
    if (!s_reload_mu) {
        s_reload_mu = xSemaphoreCreateMutex();
        if (!s_reload_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_reload_mu, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "hub soft reload busy — timed out waiting");
        return ESP_ERR_TIMEOUT;
    }
    s_reload_busy = true;
    esp_err_t err = hub_soft_reload_locked(adapter_name);
    s_reload_busy = false;
    xSemaphoreGive(s_reload_mu);
    return err;
}

esp_err_t minihub_hub_soft_reload_adapter_async(const char *adapter_name)
{
    if (!adapter_name || !adapter_name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_reload_task();
    if (err != ESP_OK) {
        return err;
    }

    bool queued = false;
    bool duplicate = false;
    portENTER_CRITICAL(&s_soft_mux);
    for (size_t i = 0; i < s_soft_queue_len; i++) {
        if (strncmp(s_soft_queue[i], adapter_name, sizeof(s_soft_queue[i])) == 0) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate && s_soft_queue_len < SOFT_RELOAD_QUEUE_MAX) {
        strncpy(s_soft_queue[s_soft_queue_len], adapter_name, sizeof(s_soft_queue[0]) - 1);
        s_soft_queue[s_soft_queue_len][sizeof(s_soft_queue[0]) - 1] = '\0';
        s_soft_queue_len++;
        s_soft_pending = true;
        queued = true;
    }
    portEXIT_CRITICAL(&s_soft_mux);

    if (duplicate) {
        ESP_LOGI(TAG, "soft reload already queued (%s)", adapter_name);
        return ESP_OK;
    }
    if (!queued) {
        ESP_LOGW(TAG, "soft reload queue full — dropped (%s)", adapter_name);
        return ESP_ERR_NO_MEM;
    }
    xTaskNotifyGive(s_reload_task);
    ESP_LOGI(TAG, "soft reload scheduled (%s)", adapter_name);
    return ESP_OK;
}

esp_err_t minihub_hub_reload_async(void)
{
    esp_err_t err = ensure_reload_task();
    if (err != ESP_OK) {
        return err;
    }
    s_reload_pending = true;
    xTaskNotifyGive(s_reload_task);
    ESP_LOGI(TAG, "hub reload scheduled");
    return ESP_OK;
}
