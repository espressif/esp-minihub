/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"
#include "minihub_task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "core_task";

extern QueueHandle_t minihub_event_bus_queue(void);
extern void minihub_event_bus_dispatch_one(minihub_event_t *event);

static void core_task(void *arg)
{
    (void)arg;
    QueueHandle_t queue = minihub_event_bus_queue();
    minihub_event_t event;

    ESP_LOGI(TAG, "core task running");
    while (queue) {
        if (xQueueReceive(queue, &event, portMAX_DELAY) == pdTRUE) {
            minihub_event_bus_dispatch_one(&event);
        }
    }
    vTaskDelete(NULL);
}

esp_err_t minihub_core_task_start(void)
{
    QueueHandle_t queue = minihub_event_bus_queue();
    if (!queue) {
        ESP_LOGW(TAG, "no event queue, core task not started");
        return ESP_OK;
    }

    BaseType_t ok = minihub_task_create_psram(core_task, "core_task", 6144, NULL, configMAX_PRIORITIES - 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create core_task (internal_free=%u spiram_free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
