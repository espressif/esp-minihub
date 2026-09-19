/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * matter_ota L3 module entry. Does not register device points — Host
 * matter adapter owns light/matter_* ; this plugin only merges props.owner / ota.
 */
#ifdef MINIHUB_PLUGIN_STANDALONE
#include "minihub_plugin_sdk.h"
#else
#include "minihub_module_abi.h"
#include "minihub_adapter.h"
#include "cJSON.h"
#endif
#include <string.h>

extern const minihub_adapter_t matter_ota_adapter;
const minihub_module_host_api_t *g_matter_ota_host;
cJSON *g_matter_ota_config;

esp_err_t minihub_module_entry(
    const minihub_module_host_api_t *host,
    const cJSON *config,
    const minihub_adapter_t **out_adapter)
{
    if (!host || !out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    g_matter_ota_host = host;
    if (g_matter_ota_config) {
        cJSON_Delete(g_matter_ota_config);
        g_matter_ota_config = NULL;
    }
    if (config) {
        g_matter_ota_config = cJSON_Duplicate(config, true);
    }
    *out_adapter = &matter_ota_adapter;
    return ESP_OK;
}
