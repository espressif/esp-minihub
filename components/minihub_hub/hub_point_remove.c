/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_hub.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_ZIGBEE_SDK
#include "minihub_zigbee.h"
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
#include "minihub_ble_mesh.h"
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
#include "minihub_matter.h"
#endif

static const char *TAG = "hub_remove";

static cJSON *remove_err(const char *code, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", code ? code : "fail");
    if (detail && detail[0]) {
        cJSON_AddStringToObject(root, "message", detail);
    }
    return root;
}

static cJSON *remove_ok(cJSON *removed_arr, size_t removed, const char *single_id, bool reloaded)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(removed_arr);
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "result", "ok");
    cJSON_AddNumberToObject(root, "removed", (double)removed);
    if (single_id && single_id[0]) {
        cJSON_AddStringToObject(root, "point_id", single_id);
    }
    if (removed_arr) {
        cJSON_AddItemToObject(root, "point_ids", removed_arr);
    }
    if (reloaded) {
        cJSON_AddBoolToObject(root, "reloaded", true);
    }
    return root;
}

static esp_err_t remove_one(const char *adapter, const char *point_id)
{
    if (!adapter || !adapter[0] || !point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (strcmp(adapter, "zigbee") == 0) {
        esp_err_t err = minihub_zigbee_remove_device(point_id);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            /* Drop the live point even if NVS had no entry (mirrors matter). */
            (void)minihub_point_unregister(point_id);
            (void)minihub_config_remove_adapter_device("zigbee", point_id);
            return ESP_OK;
        }
        return err;
    }
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (strcmp(adapter, "ble_mesh") == 0) {
        esp_err_t err = minihub_ble_mesh_remove_device(point_id);
        if (err == ESP_OK) {
            (void)minihub_config_remove_adapter_device("ble_mesh", point_id);
        }
        return err;
    }
#endif
#if CONFIG_MINIHUB_MATTER_ADAPTER
    if (strcmp(adapter, "matter") == 0) {
        esp_err_t err = minihub_matter_device_remove(point_id);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            /* Drop the live point now so the card disappears even if reload is
             * async / commissioner is broken. */
            (void)minihub_point_unregister(point_id);
            return ESP_OK;
        }
        return err;
    }
#endif

    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = minihub_point_unregister(point_id);
    if (err == ESP_OK) {
        (void)minihub_config_remove_adapter_device(snap.adapter, point_id);
    }
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    return err;
}

static esp_err_t resolve_adapter(const char *point_id, const cJSON *adapter_j, char *adapter_out,
                                 size_t adapter_len)
{
    if (cJSON_IsString(adapter_j) && adapter_j->valuestring[0]) {
        strlcpy(adapter_out, adapter_j->valuestring, adapter_len);
        return ESP_OK;
    }
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) == ESP_OK) {
        strlcpy(adapter_out, snap.adapter, adapter_len);
        if (snap.props) {
            cJSON_Delete(snap.props);
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t collect_ids_for_adapter(const char *adapter, char ids[][MINIHUB_POINT_ID_MAX],
                                         size_t cap, size_t *count)
{
    minihub_point_t *points = NULL;
    size_t n = 0;

    *count = 0;
    if (minihub_point_list_by_adapter(adapter, &points, &n) != ESP_OK || !points) {
        return ESP_OK;
    }
    for (size_t i = 0; i < n && *count < cap; i++) {
        strlcpy(ids[*count], points[i].point_id, MINIHUB_POINT_ID_MAX);
        (*count)++;
    }
    minihub_point_list_free(points, n);
    return ESP_OK;
}

esp_err_t minihub_hub_points_remove(const cJSON *req, cJSON **resp)
{
    const cJSON *delete_all = NULL;
    const cJSON *point_id_j = NULL;
    const cJSON *adapter_j = NULL;
    const cJSON *reload_j = NULL;
    char adapter_buf[MINIHUB_ADAPTER_MAX];
    bool matter_removed = false;
    bool do_reload = true;

    if (!req || !resp) {
        return ESP_ERR_INVALID_ARG;
    }
    *resp = NULL;

    delete_all = cJSON_GetObjectItem(req, "delete_all");
    point_id_j = cJSON_GetObjectItem(req, "point_id");
    adapter_j = cJSON_GetObjectItem(req, "adapter");
    reload_j = cJSON_GetObjectItem(req, "reload");
    do_reload = !cJSON_IsBool(reload_j) || cJSON_IsTrue(reload_j);

    if (cJSON_IsTrue(delete_all)) {
        if (!cJSON_IsString(adapter_j) || !adapter_j->valuestring[0]) {
            *resp = remove_err("INVALID_ARG", "delete_all requires adapter");
            return ESP_OK;
        }

        char ids[MINIHUB_MAX_POINTS][MINIHUB_POINT_ID_MAX];
        size_t count = 0;
        cJSON *removed_arr = cJSON_CreateArray();
        size_t removed = 0;

        if (!removed_arr) {
            return ESP_ERR_NO_MEM;
        }

        (void)collect_ids_for_adapter(adapter_j->valuestring, ids, MINIHUB_MAX_POINTS, &count);
        for (size_t i = 0; i < count; i++) {
            if (remove_one(adapter_j->valuestring, ids[i]) != ESP_OK) {
                continue;
            }
            cJSON_AddItemToArray(removed_arr, cJSON_CreateString(ids[i]));
            removed++;
            if (strcmp(adapter_j->valuestring, "matter") == 0) {
                matter_removed = true;
            }
        }

        if (removed == 0) {
            cJSON_Delete(removed_arr);
            *resp = remove_err("NOT_FOUND", "no matching devices");
            return ESP_OK;
        }

        if (matter_removed && do_reload) {
            /* The deleted point is already gone (dropped above); republish the rest
             * in place instead of a full reload, which would blank every other
             * adapter's devices for the ~1.7 s restart takes. */
            (void)minihub_hub_resync_adapter_async("matter");
        }

        *resp = remove_ok(removed_arr, removed, NULL, matter_removed && do_reload);
        return ESP_OK;
    }

    if (!cJSON_IsString(point_id_j) || !point_id_j->valuestring[0]) {
        const cJSON *del_legacy = cJSON_GetObjectItem(req, "delete");
        if (!cJSON_IsTrue(del_legacy)) {
            *resp = remove_err("INVALID_ARG", "need point_id or delete_all+adapter");
            return ESP_OK;
        }
        *resp = remove_err("INVALID_ARG", "missing point_id");
        return ESP_OK;
    }

    if (resolve_adapter(point_id_j->valuestring, adapter_j, adapter_buf, sizeof(adapter_buf)) != ESP_OK) {
        *resp = remove_err("NOT_FOUND", point_id_j->valuestring);
        return ESP_OK;
    }

    esp_err_t err = remove_one(adapter_buf, point_id_j->valuestring);
    if (err != ESP_OK) {
        *resp = remove_err(esp_err_to_name(err), point_id_j->valuestring);
        return ESP_OK;
    }

    if (strcmp(adapter_buf, "matter") == 0) {
        matter_removed = true;
    }
    if (matter_removed && do_reload) {
        /* Never sync-reload on the httpd task — Matter re-init blocks UI. */
        (void)minihub_hub_reload_async();
    }

    ESP_LOGI(TAG, "removed %s (%s)", point_id_j->valuestring, adapter_buf);
    *resp = remove_ok(NULL, 1, point_id_j->valuestring, matter_removed && do_reload);
    return ESP_OK;
}
