/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_MATTER_ADAPTER

#include "matter_rpc.h"
#include "minihub_matter.h"

#include <string.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t matter_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    (void)params;
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(method, "status") == 0) {
        cJSON *status = minihub_matter_status_json();
        if (!status) {
            return ESP_ERR_NO_MEM;
        }
        *out = status;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

#endif /* CONFIG_MINIHUB_MATTER_ADAPTER */
