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
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_mem";

void minihub_heap_log(void)
{
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t dma_free = heap_caps_get_free_size(dma_caps);
    const size_t dma_min = heap_caps_get_minimum_free_size(dma_caps);
    const size_t dma_largest = heap_caps_get_largest_free_block(dma_caps);

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    if (psram_total > 0) {
        ESP_LOGI(TAG,
                 "internal: free=%uB min=%uB largest=%uB total=%uB | "
                 "dma_int: free=%uB min=%uB largest=%uB | "
                 "psram: free=%uB min=%uB largest=%uB total=%uB",
                 (unsigned)int_free, (unsigned)int_min, (unsigned)int_largest, (unsigned)int_total,
                 (unsigned)dma_free, (unsigned)dma_min, (unsigned)dma_largest,
                 (unsigned)psram_free, (unsigned)psram_min, (unsigned)psram_largest, (unsigned)psram_total);
    } else {
        ESP_LOGI(TAG,
                 "internal: free=%uB min=%uB largest=%uB total=%uB | "
                 "dma_int: free=%uB min=%uB largest=%uB | psram: n/a",
                 (unsigned)int_free, (unsigned)int_min, (unsigned)int_largest, (unsigned)int_total,
                 (unsigned)dma_free, (unsigned)dma_min, (unsigned)dma_largest);
    }
}

#if CONFIG_MINIHUB_HEAP_LOG_INTERVAL_SEC > 0

static void heap_monitor_task(void *arg)
{
    const uint32_t interval_ms = (uint32_t)(uintptr_t)arg;

    minihub_heap_log();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        minihub_heap_log();
    }
}

esp_err_t minihub_heap_monitor_start(void)
{
    const uint32_t interval_ms = (uint32_t)CONFIG_MINIHUB_HEAP_LOG_INTERVAL_SEC * 1000U;
    BaseType_t ok = minihub_task_create_psram(heap_monitor_task, "heap_mon", 2048, (void *)(uintptr_t)interval_ms, 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create heap monitor task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "heap monitor started (interval=%ds)", CONFIG_MINIHUB_HEAP_LOG_INTERVAL_SEC);
    return ESP_OK;
}

#else

esp_err_t minihub_heap_monitor_start(void)
{
    return ESP_OK;
}

#endif
