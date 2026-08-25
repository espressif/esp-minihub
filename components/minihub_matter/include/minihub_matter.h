/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t minihub_matter_init(void);
bool minihub_matter_ready(void);

typedef enum {
    MINIHUB_MATTER_DISCOVERY_NETWORK = 0, /**< Multi-admin / already on LAN (default). */
    MINIHUB_MATTER_DISCOVERY_BLE_WIFI = 1, /**< First-time: BLE + inject device Wi-Fi. */
} minihub_matter_discovery_t;

/**
 * Commission with setup payload / manual code.
 * @param discovery  NETWORK (default multi-admin) or BLE_WIFI (first-time).
 * @param wifi_ssid  Optional; for BLE_WIFI, used before provision NVS fallback.
 * @param wifi_password Optional; same resolution order as ssid.
 */
esp_err_t minihub_matter_commission_code_ex(uint64_t node_id, const char *pairing_code,
                                            minihub_matter_discovery_t discovery,
                                            const char *wifi_ssid, const char *wifi_password);

/** Multi-admin: pair with manual code (MT:...) or QR payload — NETWORK discovery. */
esp_err_t minihub_matter_commission_code(uint64_t node_id, const char *pairing_code);

/** Multi-admin: on-network pairing with setup PIN (when window advertises PIN). */
esp_err_t minihub_matter_commission_pin(uint64_t node_id, uint32_t setup_pin);

esp_err_t minihub_matter_onoff(uint64_t node_id, uint16_t endpoint_id, bool on);
esp_err_t minihub_matter_read_onoff(uint64_t node_id, uint16_t endpoint_id, bool *out_on);

cJSON *minihub_matter_status_json(void);
const char *minihub_matter_last_error(void);

/** Next free Matter node id (>= 2). */
esp_err_t minihub_matter_next_node_id(uint64_t *out_node_id);

/** Write matter.devices[] entries for node_id + endpoints into adapters.json. */
esp_err_t minihub_matter_devices_upsert(uint64_t node_id, const uint16_t *endpoints, size_t endpoint_count,
                                        bool merge_existing, size_t *synced_count);

/** Remove a Matter point from adapters.json by point_id. */
esp_err_t minihub_matter_device_remove(const char *point_id);

#ifdef __cplusplus
}
#endif
