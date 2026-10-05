/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "minihub_module_abi_check.h"

static const char *TAG = "minihub_abi";

bool minihub_module_abi_acceptable(const char *name, const char *hash)
{
    if (!name || !hash) {
        return false;
    }
    for (size_t i = 0; i < MINIHUB_ABI_SYMBOL_COUNT; i++) {
        const minihub_abi_symbol_t *s = kMinihubAbiSymbols[i];
        if (strcmp(s->name, name) != 0) {
            continue;
        }
        for (size_t j = 0; j < s->n_hashes; j++) {
            if (strcasecmp(s->hashes[j], hash) == 0) {
                return true;
            }
        }
        /* Symbol is known to the firmware but the plugin's hash is not in the
         * acceptable set -> incompatible (renamed/retyped/removed). */
        return false;
    }
    /* Unknown symbol: the plugin references a host API this firmware does not
     * export. Reject rather than risk a bad call. */
    return false;
}

esp_err_t minihub_module_abi_check_symbols(const cJSON *abi)
{
    if (!abi || !cJSON_IsObject(abi)) {
        return ESP_OK;
    }
    const cJSON *syms = cJSON_GetObjectItem(abi, "symbols");
    if (!cJSON_IsObject(syms)) {
        return ESP_OK;
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, syms) {
        if (!cJSON_IsString(entry) || !entry->valuestring) {
            return ESP_ERR_INVALID_VERSION;
        }
        if (!minihub_module_abi_acceptable(entry->string, entry->valuestring)) {
            ESP_LOGW(TAG, "plugin symbol '%s' hash not accepted by firmware", entry->string);
            return ESP_ERR_INVALID_VERSION;
        }
    }
    return ESP_OK;
}
