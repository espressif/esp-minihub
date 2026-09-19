/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * meter_plug L3 module entry. The Host calls minihub_module_entry at load
 * time; we hand back the adapter and stash the Host service table so the
 * adapter can register points / command handlers through it.
 */
#include "minihub_module_abi.h"
#include "minihub_adapter.h"

extern const minihub_adapter_t meter_plug_adapter;
/* shared with meter_plug_adapter.c */
const minihub_module_host_api_t *g_meter_plug_host;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    (void)config;
    if (!host || !out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    g_meter_plug_host = host;
    *out_adapter = &meter_plug_adapter;
    return ESP_OK;
}
