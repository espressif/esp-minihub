/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"

#include "esp_log.h"

static const char *TAG = "minihub_core";

esp_err_t minihub_point_registry_init(void);

esp_err_t minihub_core_init(void)
{
    esp_err_t err = minihub_point_registry_init();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_MINIHUB_POINT_DEBOUNCE_MS > 0
    (void)minihub_point_debounce_init();
#endif
    err = minihub_event_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    minihub_point_history_init();
    err = minihub_command_router_init();
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_core_task_start();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "core initialized (API v%d)", MINIHUB_CORE_API_VERSION);
    return ESP_OK;
}
