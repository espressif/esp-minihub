/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "minihub_miot_auth.h"
#include "module_bridge.h"

extern const minihub_adapter_t miot_adapter;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    if (!out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    minihub_miot_module_set_host(host);
    esp_err_t err = minihub_miot_auth_init();
    if (err != ESP_OK) {
        return err;
    }
    (void)config;
    *out_adapter = &miot_adapter;
    return ESP_OK;
}
