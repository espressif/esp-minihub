/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "minihub_ble_mesh_types.h"
#include "minihub_ble_mesh.h"

typedef void (*ble_mesh_stack_ready_cb_t)(void *ctx);

esp_err_t ble_mesh_stack_start(void);
esp_err_t ble_mesh_stack_stop(void);
esp_err_t ble_mesh_stack_permit_provision(int seconds);
bool ble_mesh_stack_permit_provision_active(void);
bool ble_mesh_stack_running(void);
esp_err_t ble_mesh_stack_onoff(uint16_t unicast, minihub_ble_mesh_onoff_cmd_t cmd);
esp_err_t ble_mesh_stack_level(uint16_t unicast, int16_t level);
esp_err_t ble_mesh_stack_lightness(uint16_t unicast, uint16_t lightness);
esp_err_t ble_mesh_stack_ctl(uint16_t unicast, uint16_t lightness, uint16_t temperature,
                             int16_t delta_uv);

/** Best-effort: remove provisioner node by unicast (and clear local cache). */
esp_err_t ble_mesh_stack_delete_node(uint16_t unicast);

/**
 * Rehydrate control cache after reboot (NVS has the map; s_nodes is RAM-only).
 * uuid may be all-zero if unknown — unicast is enough for OnOff Set.
 */
esp_err_t ble_mesh_stack_restore_node(const uint8_t uuid[16], uint16_t unicast, uint8_t elem_num,
                                      uint8_t caps);

/** Apply before start(); stack copies the policy. */
esp_err_t ble_mesh_stack_set_provision_policy(const minihub_bm_provision_policy_t *policy);

/**
 * Release Mesh provisioner / scan so Zigbee can use the shared RF.
 * No-op while permit_provision or an active provision (PB-GATT) is running.
 */
void ble_mesh_stack_yield_rf(void);

/** True during permit window or mid-provision — Zigbee must not demote BLE coex. */
bool ble_mesh_stack_rf_critical(void);

void ble_mesh_stack_set_hooks(minihub_ble_mesh_device_cb_t on_device,
                              minihub_ble_mesh_attr_cb_t on_attr,
                              ble_mesh_stack_ready_cb_t on_ready,
                              void *ctx);
