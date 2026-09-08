/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "minihub_adapter.h"

static const char *TAG = "PLACEHOLDER_ID";

static esp_err_t hello_init(const cJSON *config)
{
    (void)config;
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

static esp_err_t hello_start(void)
{
    ESP_LOGI(TAG, "start");
    return ESP_OK;
}

const minihub_adapter_t PLACEHOLDER_ID_adapter = {
    .name = "PLACEHOLDER_ID",
    .version = "0.1.0",
    .init = hello_init,
    .start = hello_start,
};

#if !CONFIG_MINIHUB_DYNAMIC_MODULES
REGISTER_ADAPTER(PLACEHOLDER_ID_adapter);
#endif
