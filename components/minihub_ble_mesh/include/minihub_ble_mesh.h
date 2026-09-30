/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "minihub_ble_mesh_types.h"

esp_err_t minihub_ble_mesh_init(void);
esp_err_t minihub_ble_mesh_start(void);
esp_err_t minihub_ble_mesh_stop(void);

/**
 * Set provision UUID filters + post-provision bind models (from adapters.json).
 * Call before start(); defaults remain Espressif 0xdddd + Generic OnOff if unset.
 */
esp_err_t minihub_ble_mesh_set_provision_policy(const minihub_bm_provision_policy_t *policy);

/** Open PB-ADV/PB-GATT scan window; auto-closes (scan off) when seconds elapse. */
esp_err_t minihub_ble_mesh_permit_provision(int seconds);
bool minihub_ble_mesh_permit_provision_active(void);
/** True while permit window is open or an active provision (e.g. PB-GATT) is in progress. */
bool minihub_ble_mesh_provisioning_busy(void);

/** Why a permit request was refused. The console and the MCP gateway both report
 *  it; neither should re-derive it from the stack predicates, which is how the
 *  two copies drifted into Chinese-only text in one and English in the other. */
typedef enum {
    MINIHUB_BLE_MESH_PERMIT_OK = 0,
    MINIHUB_BLE_MESH_PERMIT_BUSY,           /**< a window or provision is already running */
    MINIHUB_BLE_MESH_PERMIT_STACK_OFF,      /**< stack failed to start or is still booting */
    MINIHUB_BLE_MESH_PERMIT_ZIGBEE_ACTIVE,  /**< Zigbee joining holds the radio */
    MINIHUB_BLE_MESH_PERMIT_BAD_ARG,        /**< seconds out of range */
    MINIHUB_BLE_MESH_PERMIT_FAILED,         /**< anything else */
} minihub_ble_mesh_permit_t;

/** Classify a minihub_ble_mesh_permit_provision() result. `zigbee_joining` is an
 *  argument rather than a call so the mesh stack never depends on Zigbee. */
minihub_ble_mesh_permit_t minihub_ble_mesh_permit_classify(esp_err_t err, bool zigbee_joining);

/** Text for a classification, in Chinese when `zh`. Returns NULL for
 *  MINIHUB_BLE_MESH_PERMIT_FAILED so the caller can show esp_err_to_name(). */
const char *minihub_ble_mesh_permit_text(minihub_ble_mesh_permit_t reason, bool zh);

esp_err_t minihub_ble_mesh_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd);

/** Generic Level (−32768…32767). */
esp_err_t minihub_ble_mesh_level(uint16_t unicast, int16_t level);
/** Light Lightness actual (0…65535). */
esp_err_t minihub_ble_mesh_lightness(uint16_t unicast, uint16_t lightness);
/** Light CTL: lightness + temperature (800–20000) + delta UV. */
esp_err_t minihub_ble_mesh_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                               int16_t delta_uv);

typedef void (*minihub_ble_mesh_device_cb_t)(const minihub_ble_mesh_device_t *dev, void *ctx);
typedef void (*minihub_ble_mesh_attr_cb_t)(const minihub_ble_mesh_attr_evt_t *evt, void *ctx);

esp_err_t minihub_ble_mesh_set_callbacks(minihub_ble_mesh_device_cb_t on_device,
                                         minihub_ble_mesh_attr_cb_t on_attr,
                                         void *ctx);

esp_err_t minihub_ble_mesh_list_devices(minihub_ble_mesh_device_t *out, size_t cap, size_t *count);

bool minihub_ble_mesh_running(void);

esp_err_t minihub_ble_mesh_upsert_device(const minihub_ble_mesh_device_t *dev);

/** Remove from NVS + runtime, unregister point, best-effort stack node delete. */
esp_err_t minihub_ble_mesh_remove_device(const char *point_id);

const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_point(const char *point_id);
const minihub_ble_mesh_device_t *minihub_ble_mesh_find_by_unicast(uint16_t unicast);

/**
 * Offer an end-device OTA image for a BLE Mesh point.
 *
 * Plugins publish ``props.ota`` on the same Host point. Mesh Firmware Update
 * / BLOB transport is not wired; this returns ``ESP_ERR_NOT_SUPPORTED``.
 */
esp_err_t minihub_ble_mesh_ota_offer(const char *point_id, const char *file_path);
