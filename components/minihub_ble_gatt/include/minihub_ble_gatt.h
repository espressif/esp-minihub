/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic NimBLE GATT client for dynamic modules (no vendor protocol).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINIHUB_BLE_GATT_ADV_NAME_MAX  32
#define MINIHUB_BLE_GATT_ADV_MAX       24
/** Service Data (0x16) payload after UUID16 — includes UUID LE bytes + data. */
#define MINIHUB_BLE_GATT_SVC_DATA_MAX  31
/** Manufacturer Specific Data (0xFF) including company ID. */
#define MINIHUB_BLE_GATT_MFG_DATA_MAX  29

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[MINIHUB_BLE_GATT_ADV_NAME_MAX];
    bool has_uuid16;
    uint16_t uuid16;
    /** True if svc_data holds a 16-bit Service Data AD (uuid in first 2 LE bytes). */
    bool has_svc_data;
    uint16_t svc_data_uuid16;
    uint8_t svc_data_len;
    uint8_t svc_data[MINIHUB_BLE_GATT_SVC_DATA_MAX];
    bool has_mfg_data;
    uint16_t mfg_company;
    uint8_t mfg_data_len;
    uint8_t mfg_data[MINIHUB_BLE_GATT_MFG_DATA_MAX];
} minihub_ble_gatt_adv_t;

typedef void (*minihub_ble_gatt_notify_cb_t)(uint16_t conn_handle,
                                            uint16_t attr_handle,
                                            const uint8_t *data,
                                            size_t len,
                                            void *ctx);

esp_err_t minihub_ble_gatt_init(void);
esp_err_t minihub_ble_gatt_deinit(void);
bool minihub_ble_gatt_ready(void);

/** duration_ms=0 → until scan_stop. filter_uuid16=0 → keep all ads with name or UUID. */
esp_err_t minihub_ble_gatt_scan_start(uint32_t duration_ms, uint16_t filter_uuid16);
esp_err_t minihub_ble_gatt_scan_stop(void);
esp_err_t minihub_ble_gatt_scan_get(minihub_ble_gatt_adv_t *out, size_t max, size_t *count);

esp_err_t minihub_ble_gatt_connect(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t minihub_ble_gatt_disconnect(void);
bool minihub_ble_gatt_is_connected(void);
uint16_t minihub_ble_gatt_conn_handle(void);

esp_err_t minihub_ble_gatt_discover_svc(uint16_t svc_uuid16, uint32_t timeout_ms);
esp_err_t minihub_ble_gatt_find_char(uint16_t char_uuid16, uint16_t *val_handle, uint16_t *cccd_handle);

esp_err_t minihub_ble_gatt_subscribe(uint16_t cccd_handle,
                                     minihub_ble_gatt_notify_cb_t cb,
                                     void *ctx);
esp_err_t minihub_ble_gatt_write(uint16_t val_handle, const uint8_t *data, size_t len, bool no_rsp);

/**
 * Wait for next notify on any subscribed characteristic (or filter by attr_handle!=0).
 * Copies payload into buf; returns ESP_ERR_TIMEOUT on timeout.
 */
esp_err_t minihub_ble_gatt_wait_notify(uint16_t attr_handle,
                                       uint8_t *buf,
                                       size_t buf_len,
                                       size_t *out_len,
                                       uint16_t *out_attr,
                                       uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
