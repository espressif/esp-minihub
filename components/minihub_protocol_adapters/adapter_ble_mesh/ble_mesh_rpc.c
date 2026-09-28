/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_rpc.h"
#include "minihub_ble_mesh.h"
#include "minihub_zigbee.h"

#include <string.h>
#include "esp_err.h"
#include "cJSON.h"

static esp_err_t rpc_permit_provision(const cJSON *params, cJSON **out)
{
    int seconds = 60;
    if (params) {
        const cJSON *sec = cJSON_GetObjectItem(params, "seconds");
        if (cJSON_IsNumber(sec)) {
            seconds = sec->valueint;
        }
    }

    esp_err_t err = minihub_ble_mesh_permit_provision(seconds);
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(resp, "result", err == ESP_OK ? "ok" : "fail");
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE && minihub_ble_mesh_provisioning_busy()) {
            cJSON_AddStringToObject(resp, "error", "BLE Mesh 正在配网，请勿重复开启");
        } else if (err == ESP_ERR_INVALID_STATE && minihub_zigbee_permit_join_active()) {
            cJSON_AddStringToObject(resp, "error", "Zigbee 正在入网，无法同时开启 BLE Mesh 配网");
        } else if (err == ESP_ERR_INVALID_ARG) {
            cJSON_AddStringToObject(resp, "error", "seconds 无效（须 >= 0；0 关闭窗口）");
        } else {
            cJSON_AddStringToObject(resp, "error", esp_err_to_name(err));
        }
    }
    cJSON_AddBoolToObject(resp, "permit_provision", minihub_ble_mesh_permit_provision_active());
    *out = resp;
    return ESP_OK;
}

esp_err_t ble_mesh_module_rpc(const char *method, const cJSON *params, cJSON **out)
{
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(method, "permit_provision") == 0) {
        return rpc_permit_provision(params, out);
    }
    return ESP_ERR_NOT_FOUND;
}
