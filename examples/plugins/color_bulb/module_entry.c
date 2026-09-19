/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * color_bulb L3 module entry. The Host calls minihub_module_entry at load
 * time; we hand back the adapter and stash the Host service table (g_host)
 * so the adapter can register points / command handlers through it.
 */
#include "minihub_module_abi.h"
#include "minihub_adapter.h"

extern const minihub_adapter_t color_bulb_adapter;
/* shared with color_bulb_adapter.c */
const minihub_module_host_api_t *g_host;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    (void)config;
    if (!host || !out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    g_host = host;
    *out_adapter = &color_bulb_adapter;
    return ESP_OK;
}
