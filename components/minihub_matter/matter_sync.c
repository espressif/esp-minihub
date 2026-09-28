/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_matter.h"
#include "minihub_config.h"
#include "minihub_paths.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "matter_sync";

static const char *k_matter_adapter = "matter";
static const char *k_legacy_thread_adapter = "thread";

static uint64_t parse_node_id(const char *s, uint64_t fallback)
{
    if (!s || !s[0]) {
        return fallback;
    }
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0) {
        return strtoull(s, NULL, 16);
    }
    return strtoull(s, NULL, 10);
}

/** Ensure matter exists; migrate devices from legacy thread.devices once; keep RF thread. */
static bool migrate_legacy_thread(cJSON *adapters)
{
    if (!adapters) {
        return false;
    }

    cJSON *matter = cJSON_GetObjectItem(adapters, k_matter_adapter);
    cJSON *legacy = cJSON_GetObjectItem(adapters, k_legacy_thread_adapter);
    if (!legacy || !cJSON_IsObject(legacy)) {
        return false;
    }

    cJSON *legacy_devices = cJSON_GetObjectItem(legacy, "devices");
    if (!cJSON_IsArray(legacy_devices)) {
        return false;
    }

    const bool legacy_has_devices = cJSON_GetArraySize(legacy_devices) > 0;
    const cJSON *legacy_en = cJSON_GetObjectItem(legacy, "enabled");
    const bool legacy_enabled = cJSON_IsTrue(legacy_en);
    bool changed = false;

    if (!matter) {
        matter = cJSON_CreateObject();
        if (!matter) {
            return false;
        }
        cJSON_AddItemToObject(adapters, k_matter_adapter, matter);
        cJSON_AddBoolToObject(matter, "enabled", legacy_enabled);
        if (legacy_has_devices) {
            cJSON *dup = cJSON_Duplicate(legacy_devices, true);
            if (dup) {
                cJSON_AddItemToObject(matter, "devices", dup);
            }
        } else {
            cJSON_AddItemToObject(matter, "devices", cJSON_CreateArray());
        }
        changed = true;
    } else {
        cJSON *matter_devices = cJSON_GetObjectItem(matter, "devices");
        const bool matter_empty =
            !cJSON_IsArray(matter_devices) || cJSON_GetArraySize(matter_devices) == 0;
        if (matter_empty && legacy_has_devices) {
            cJSON_DeleteItemFromObject(matter, "devices");
            cJSON *dup = cJSON_Duplicate(legacy_devices, true);
            if (dup) {
                cJSON_AddItemToObject(matter, "devices", dup);
            }
            changed = true;
        }
        if (!cJSON_HasObjectItem(matter, "enabled") && legacy_enabled) {
            cJSON_AddTrueToObject(matter, "enabled");
            changed = true;
        }
        cJSON_DeleteItemFromObject(matter, "comment");
    }

    cJSON_DeleteItemFromObject(legacy, "devices");
    cJSON_DeleteItemFromObject(legacy, "enabled");
    cJSON_AddBoolToObject(legacy, "enabled", false);
    if (!cJSON_HasObjectItem(legacy, "dataset")) {
        cJSON_AddNullToObject(legacy, "dataset");
    }
    changed = true;
    return changed;
}

static cJSON *get_matter_devices(cJSON *adapters_root, bool create)
{
    cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!adapters) {
        if (!create) {
            return NULL;
        }
        adapters = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters_root, "adapters", adapters);
    }

    (void)migrate_legacy_thread(adapters);

    cJSON *matter = cJSON_GetObjectItem(adapters, k_matter_adapter);
    if (!matter) {
        if (!create) {
            return NULL;
        }
        matter = cJSON_CreateObject();
        cJSON_AddItemToObject(adapters, k_matter_adapter, matter);
    }

    cJSON_DeleteItemFromObject(matter, "enabled");
    cJSON_AddTrueToObject(matter, "enabled");

    cJSON *devices = cJSON_GetObjectItem(matter, "devices");
    if (!devices) {
        if (!create) {
            return NULL;
        }
        devices = cJSON_CreateArray();
        cJSON_AddItemToObject(matter, "devices", devices);
    }
    return devices;
}

static cJSON *find_device_entry(cJSON *devices, const char *point_id)
{
    if (!devices || !point_id) {
        return NULL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *pid = cJSON_GetObjectItem(item, "point_id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, point_id) == 0) {
            return item;
        }
    }
    return NULL;
}

static cJSON *find_device_by_endpoint(cJSON *devices, uint64_t node_id, uint16_t endpoint_id)
{
    if (!devices) {
        return NULL;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *nid = cJSON_GetObjectItem(item, "node_id");
        const cJSON *eid = cJSON_GetObjectItem(item, "endpoint_id");
        if (!cJSON_IsNumber(eid)) {
            continue;
        }
        uint64_t n = cJSON_IsString(nid) ? parse_node_id(nid->valuestring, 0) : 0;
        if (n == node_id && (uint16_t)eid->valueint == endpoint_id) {
            return item;
        }
    }
    return NULL;
}

/* point_id is "<protocol>.<device>"; for Matter the radio is per device, so the
 * commissioning transport decides the prefix (thread / wifi / ethernet). */
static void make_point_id(uint64_t node_id, uint16_t endpoint_id, const char *transport,
                          char *out, size_t out_len)
{
    /* R15: a device committed without a known transport gets the "Unknown" prefix
     * rather than masquerading as "matter" — the user裁定 wants missing transport
     * surfaced, not hidden. Devices that already have a transport keep it. */
    const char *proto = (transport && transport[0]) ? transport : "Unknown";
    snprintf(out, out_len, "%s.matter_n%" PRIu64 "_ep%u", proto, node_id, (unsigned)endpoint_id);
}

esp_err_t minihub_matter_next_node_id(uint64_t *out_node_id)
{
    if (!out_node_id) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *adapters_root = minihub_config_get_adapters();
    if (!adapters_root) {
        *out_node_id = 2;
        return ESP_OK;
    }

    uint64_t max_id = 1;
    cJSON *devices = get_matter_devices(adapters_root, false);
    if (devices) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, devices) {
            const cJSON *nid = cJSON_GetObjectItem(item, "node_id");
            if (!cJSON_IsString(nid)) {
                continue;
            }
            uint64_t n = parse_node_id(nid->valuestring, 0);
            if (n > max_id) {
                max_id = n;
            }
        }
    }

    *out_node_id = max_id < 2 ? 2 : max_id + 1;
    return ESP_OK;
}

esp_err_t minihub_matter_devices_upsert(uint64_t node_id, const uint16_t *endpoints, size_t endpoint_count,
                                        bool merge_existing, size_t *synced_count, const char *transport)
{
    if (!endpoints || endpoint_count == 0 || node_id < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (synced_count) {
        *synced_count = 0;
    }

    cJSON *adapters_root = minihub_config_get_adapters();
    if (!adapters_root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *devices = get_matter_devices(adapters_root, true);
    if (!devices) {
        return ESP_FAIL;
    }

    size_t added = 0;
    for (size_t i = 0; i < endpoint_count; i++) {
        const uint16_t ep = endpoints[i];
        if (ep == 0) {
            continue;
        }

        cJSON *existing = merge_existing ? find_device_by_endpoint(devices, node_id, ep) : NULL;
        char point_id[64];
        make_point_id(node_id, ep, transport, point_id, sizeof(point_id));
        if (existing) {
            /* R15: never rewrite an existing row's point_id. A transport-less resync
             * must not rename a row that already earned a prefix (e.g. thread.), or
             * the old id becomes a permanent orphan while the new id is created —
             * "one bulb, two cards". Keep the id the device already has; only refresh
             * the scalar fields. */
            const cJSON *pid = cJSON_GetObjectItem(existing, "point_id");
            if (cJSON_IsString(pid)) {
                strncpy(point_id, pid->valuestring, sizeof(point_id) - 1);
                point_id[sizeof(point_id) - 1] = '\0';
            }
            cJSON_DeleteItemFromObject(existing, "node_id");
            cJSON_DeleteItemFromObject(existing, "endpoint_id");
            cJSON_DeleteItemFromObject(existing, "type");
            char node_str[24];
            snprintf(node_str, sizeof(node_str), "%" PRIu64, node_id);
            cJSON_AddStringToObject(existing, "type", "light");
            cJSON_AddStringToObject(existing, "node_id", node_str);
            cJSON_AddNumberToObject(existing, "endpoint_id", ep);
            if (transport && transport[0]) {
                cJSON_DeleteItemFromObject(existing, "transport");
                cJSON_AddStringToObject(existing, "transport", transport);
            }
            added++;
            continue;
        }

        if (find_device_entry(devices, point_id)) {
            continue;
        }

        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            return ESP_ERR_NO_MEM;
        }
        char node_str[24];
        snprintf(node_str, sizeof(node_str), "%" PRIu64, node_id);
        cJSON_AddStringToObject(entry, "point_id", point_id);
        cJSON_AddStringToObject(entry, "type", "light");
        cJSON_AddStringToObject(entry, "node_id", node_str);
        cJSON_AddNumberToObject(entry, "endpoint_id", ep);
        if (transport && transport[0]) {
            cJSON_AddStringToObject(entry, "transport", transport);
        }
        cJSON_AddItemToArray(devices, entry);
        added++;
    }

    esp_err_t err = minihub_config_save_adapters(adapters_root);
    if (err != ESP_OK) {
        return err;
    }

    if (synced_count) {
        *synced_count = added;
    }
    ESP_LOGI(TAG, "upsert node %" PRIu64 ": %u endpoint(s) -> %s", node_id, (unsigned)added, MINIHUB_ADAPTERS_JSON);
    return ESP_OK;
}

esp_err_t minihub_matter_devices_set_commission_phase(uint64_t node_id, const char *phase,
                                                      const char *error_msg)
{
    if (node_id < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *adapters_root = minihub_config_get_adapters();
    if (!adapters_root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *devices = get_matter_devices(adapters_root, false);
    if (!devices) {
        return ESP_ERR_NOT_FOUND;
    }

    const bool clear = !phase || !phase[0];
    int touched = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *nid = cJSON_GetObjectItem(item, "node_id");
        uint64_t n = cJSON_IsString(nid) ? parse_node_id(nid->valuestring, 0) : 0;
        if (n != node_id) {
            continue;
        }
        cJSON_DeleteItemFromObject(item, "commission_phase");
        cJSON_DeleteItemFromObject(item, "commission_error");
        if (!clear) {
            cJSON_AddStringToObject(item, "commission_phase", phase);
            if (error_msg && error_msg[0]) {
                cJSON_AddStringToObject(item, "commission_error", error_msg);
            }
        }
        touched++;
    }
    if (touched == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = minihub_config_save_adapters(adapters_root);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "node %" PRIu64 " commission_phase=%s (%d row(s))", node_id,
                 clear ? "(cleared)" : phase, touched);
    }
    return err;
}

esp_err_t minihub_matter_device_remove(const char *point_id)
{
    if (!point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *adapters_root = minihub_config_get_adapters();
    if (!adapters_root) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *devices = get_matter_devices(adapters_root, false);
    if (!devices) {
        return ESP_ERR_NOT_FOUND;
    }

    const int count = cJSON_GetArraySize(devices);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(devices, i);
        const cJSON *pid = cJSON_GetObjectItem(item, "point_id");
        if (cJSON_IsString(pid) && strcmp(pid->valuestring, point_id) == 0) {
            const cJSON *nid = cJSON_GetObjectItem(item, "node_id");
            uint64_t node = cJSON_IsString(nid) ? parse_node_id(nid->valuestring, 0) : 0;
            if (node && minihub_matter_commission_cancel(node) == ESP_OK) {
                ESP_LOGI(TAG, "aborted in-flight commissioning for node %" PRIu64, node);
            }
            cJSON_DeleteItemFromArray(devices, i);
            esp_err_t err = minihub_config_save_adapters(adapters_root);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "removed %s from %s", point_id, MINIHUB_ADAPTERS_JSON);
            }
            return err;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
