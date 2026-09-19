/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ble_mesh_ota L3 module entry. Does not register device points — Host
 * ble_mesh adapter owns light/bm_* ; this plugin only merges props.owner / ota.
 */
#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "cJSON.h"
#include <string.h>

extern const minihub_adapter_t ble_mesh_ota_adapter;
const minihub_module_host_api_t *g_ble_mesh_ota_host;
cJSON *g_ble_mesh_ota_config;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    if (!host || !out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    g_ble_mesh_ota_host = host;
    if (g_ble_mesh_ota_config) {
        cJSON_Delete(g_ble_mesh_ota_config);
        g_ble_mesh_ota_config = NULL;
    }
    if (config) {
        g_ble_mesh_ota_config = cJSON_Duplicate(config, true);
    }
    *out_adapter = &ble_mesh_ota_adapter;
    return ESP_OK;
}
