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

/**
 * Attach CHIP ThreadStackManager to MiniHub's already-running OpenThread
 * instance (after minihub_thread_init). Required so FindOperational can use
 * OT DNS-SD / SRP instead of Wi-Fi mDNS alone.
 */
esp_err_t minihub_matter_attach_thread(void);

typedef enum {
    MINIHUB_MATTER_DISCOVERY_NETWORK = 0, /**< Multi-admin / already on LAN (default). */
    MINIHUB_MATTER_DISCOVERY_BLE_WIFI = 1, /**< First-time: BLE + inject device Wi-Fi. */
    MINIHUB_MATTER_DISCOVERY_BLE_THREAD = 2, /**< First-time: BLE + inject Thread dataset from Hub BR. */
    MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET = 3, /**< First-time: BLE, device already on Ethernet (no SSID). */
} minihub_matter_discovery_t;

/**
 * Commission with setup payload / manual code.
 * @param discovery  NETWORK, BLE_WIFI, BLE_THREAD, or BLE_ETHERNET.
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

/**
 * Write matter.devices[] entries for node_id + endpoints into adapters.json.
 * @param transport  Optional: "wifi" | "ethernet" | "thread" (stored on each device).
 */
esp_err_t minihub_matter_devices_upsert(uint64_t node_id, const uint16_t *endpoints, size_t endpoint_count,
                                        bool merge_existing, size_t *synced_count, const char *transport);

/** Remove a Matter point from adapters.json by point_id. */
esp_err_t minihub_matter_device_remove(const char *point_id);

/** Basic Information (cluster 0x0028, typically endpoint 0). */
typedef struct {
    char manufacturer[64];
    char model[64];
    char sw_version[32];
    char hw_version[16];
} minihub_matter_identity_t;

typedef void (*minihub_matter_identity_cb_t)(uint64_t node_id, const minihub_matter_identity_t *id, void *ctx);

esp_err_t minihub_matter_set_identity_cb(minihub_matter_identity_cb_t cb, void *ctx);

/** Best-effort read of Basic Information. Failure does not unwind commission. */
esp_err_t minihub_matter_interview_identity(uint64_t node_id);

/**
 * Offer a Matter OTA image (CSA Provider 0x0029 + AnnounceOTAProvider 0x002A + BDX).
 *
 * ``file_path`` is a local Matter OTA image (CSA header) or an ``http(s)`` URL.
 * Local files are served on loopback for the ESP-Matter BDX HTTP downloader.
 * Sends AnnounceOTAProvider to the requestor on endpoint 0.
 */
esp_err_t minihub_matter_ota_offer(const char *point_id, const char *file_path);

#ifdef __cplusplus
}
#endif
