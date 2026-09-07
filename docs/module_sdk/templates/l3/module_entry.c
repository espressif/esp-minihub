/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_module_abi.h"

extern const minihub_adapter_t PLACEHOLDER_ID_adapter;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    (void)host;
    (void)config;
    if (!out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_adapter = &PLACEHOLDER_ID_adapter;
    return ESP_OK;
}
