/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "cJSON.h"
#include "minihub_zigbee_types.h"

esp_err_t minihub_zigbee_init(void);
esp_err_t minihub_zigbee_start(void);
esp_err_t minihub_zigbee_stop(void);

esp_err_t minihub_zigbee_permit_join(int seconds);
bool minihub_zigbee_permit_join_active(void);

/** Add human-readable error / user_reply / hint fields to a JSON object for permit_join failures. */
void minihub_zigbee_json_add_permit_join_error(cJSON *obj, esp_err_t err);

/**
 * Add install code for a joining device (hex string, optional 0x / separators).
 * Length must be 8/10/14/18 bytes (incl. CRC). Call before permit_join.
 */
esp_err_t minihub_zigbee_add_install_code(uint64_t ieee, const char *ic_hex);

esp_err_t minihub_zigbee_onoff(uint16_t short_addr, uint8_t endpoint,
                               minihub_zigbee_onoff_cmd_t cmd);

typedef void (*minihub_zigbee_device_cb_t)(const minihub_zigbee_device_t *dev, void *ctx);
typedef void (*minihub_zigbee_attr_cb_t)(const minihub_zigbee_attr_evt_t *evt, void *ctx);

esp_err_t minihub_zigbee_set_callbacks(minihub_zigbee_device_cb_t on_device,
                                       minihub_zigbee_attr_cb_t on_attr,
                                       void *ctx);

esp_err_t minihub_zigbee_list_devices(minihub_zigbee_device_t *out, size_t cap, size_t *count);

bool minihub_zigbee_running(void);

/**
 * Block until the Zigbee coordinator has formed/restored (stack ready), or timeout.
 * Returns ESP_OK when ready, ESP_ERR_TIMEOUT on wait expiry, ESP_ERR_NOT_SUPPORTED if SDK off.
 */
esp_err_t minihub_zigbee_wait_ready(uint32_t timeout_ms);

/** Upsert device into runtime table + NVS (used after join / config bind). */
esp_err_t minihub_zigbee_upsert_device(const minihub_zigbee_device_t *dev);

/** Remove device from NVS + runtime table and unregister its point. */
esp_err_t minihub_zigbee_remove_device(const char *point_id);

/** Find device by point_id; returns NULL if missing. */
const minihub_zigbee_device_t *minihub_zigbee_find_by_point(const char *point_id);

/** Find device by short_addr + endpoint. */
const minihub_zigbee_device_t *minihub_zigbee_find_by_addr(uint16_t short_addr, uint8_t endpoint);

/** Parse IEEE hex string (optional 0x prefix). Returns 0 on success, -1 on error. */
int minihub_zb_parse_ieee(const char *s, uint64_t *out);
