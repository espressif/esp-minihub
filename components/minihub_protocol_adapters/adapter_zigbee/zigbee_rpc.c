/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_rpc.h"
#include "minihub_zigbee.h"
#include "minihub_ble_mesh.h"

#include <string.h>
#include "esp_err.h"
#include "cJSON.h"

static esp_err_t rpc_permit_join(const cJSON *params, cJSON **out)
{
    int seconds = 60;
    if (params) {
        const cJSON *sec = cJSON_GetObjectItem(params, "seconds");
        if (cJSON_IsNumber(sec)) {
            seconds = sec->valueint;
        }
    }

    esp_err_t err = minihub_zigbee_permit_join(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err != ESP_OK) {
        minihub_zigbee_json_add_permit_join_error(resp, err);
    }
    cJSON_AddBoolToObject(resp, "permit_join", minihub_zigbee_permit_join_active());
    *out = resp;
    return ESP_OK;
}

static esp_err_t rpc_add_install_code(const cJSON *params, cJSON **out)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    const cJSON *ieee_j = params ? cJSON_GetObjectItem(params, "ieee") : NULL;
    const cJSON *ic_j = params ? cJSON_GetObjectItem(params, "install_code") : NULL;
    if (!cJSON_IsString(ieee_j) || !cJSON_IsString(ic_j)) {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "need ieee + install_code strings");
        *out = resp;
        return ESP_OK;
    }
    uint64_t ieee = 0;
    if (minihub_zb_parse_ieee(ieee_j->valuestring, &ieee) != 0) {
        cJSON_AddStringToObject(resp, "result", "fail");
        cJSON_AddStringToObject(resp, "error", "ieee parse failed");
        *out = resp;
        return ESP_OK;
    }
    esp_err_t err = minihub_zigbee_add_install_code(ieee, ic_j->valuestring);
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
    }
    *out = resp;
    return ESP_OK;
}

esp_err_t zigbee_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(method, "permit_join") == 0) {
        return rpc_permit_join(params, out);
    }
    if (strcmp(method, "add_install_code") == 0) {
        return rpc_add_install_code(params, out);
    }
    return ESP_ERR_NOT_FOUND;
}
