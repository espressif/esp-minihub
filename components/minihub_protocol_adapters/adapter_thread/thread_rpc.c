/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_THREAD_ADAPTER

#include "thread_rpc.h"
#include "minihub_thread.h"

#include <string.h>
#include "cJSON.h"
#include "esp_err.h"

esp_err_t thread_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    (void)params;
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(method, "status") == 0) {
        cJSON *status = minihub_thread_status_json();
        if (!status) {
            return ESP_ERR_NO_MEM;
        }
        *out = status;
        return ESP_OK;
    }

    if (strcmp(method, "start") == 0) {
        esp_err_t err = minihub_thread_start();
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
        if (err != ESP_OK) {
            const char *msg = minihub_thread_last_error();
            cJSON_AddStringToObject(resp, "error", (msg && msg[0]) ? msg : esp_err_to_name(err));
        }
        cJSON_AddBoolToObject(resp, "ready", minihub_thread_ready());
        *out = resp;
        return ESP_OK;
    }

    if (strcmp(method, "stop") == 0) {
        esp_err_t err = minihub_thread_stop();
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
        if (err != ESP_OK) {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        }
        *out = resp;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

#endif /* CONFIG_MINIHUB_THREAD_ADAPTER */
