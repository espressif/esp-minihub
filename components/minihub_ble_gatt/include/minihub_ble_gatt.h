/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
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
/**
 * @brief Service Data (0x16) payload after UUID16 — includes UUID LE bytes + data.
 */
#define MINIHUB_BLE_GATT_SVC_DATA_MAX  31
/**
 * @brief Manufacturer Specific Data (0xFF) including company ID.
 */
#define MINIHUB_BLE_GATT_MFG_DATA_MAX  29

/**
 * @brief One BLE advertisement record from GATT client scan.
 */
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    char name[MINIHUB_BLE_GATT_ADV_NAME_MAX];
    bool has_uuid16;
    uint16_t uuid16;
    /**
     * @brief True if svc_data holds a 16-bit Service Data AD (uuid in first 2 LE bytes); false otherwise.
     */
    bool has_svc_data;
    uint16_t svc_data_uuid16;
    uint8_t svc_data_len;
    uint8_t svc_data[MINIHUB_BLE_GATT_SVC_DATA_MAX];
    bool has_mfg_data;
    uint16_t mfg_company;
    uint8_t mfg_data_len;
    uint8_t mfg_data[MINIHUB_BLE_GATT_MFG_DATA_MAX];
} minihub_ble_gatt_adv_t;

/**
 * @brief Notification / indication callback.
 *
 * @param conn_handle Connection handle.
 * @param attr_handle Characteristic value handle.
 * @param data        Payload.
 * @param len         Payload length.
 * @param ctx         User context from `minihub_ble_gatt_subscribe`.
 */
#ifdef DOXYGEN
typedef void minihub_ble_gatt_notify_cb_t(uint16_t conn_handle, uint16_t attr_handle,
                                         const uint8_t *data, size_t len, void *ctx);
#else
typedef void (*minihub_ble_gatt_notify_cb_t)(uint16_t conn_handle,
                                            uint16_t attr_handle,
                                            const uint8_t *data,
                                            size_t len,
                                            void *ctx);
#endif

/**
 * @brief Initialize NimBLE GATT **client** (not BLE Mesh radio).
 *
 * @return
 *     - ESP_OK: Succeed.
 */
esp_err_t minihub_ble_gatt_init(void);

/**
 * @brief Tear down GATT client.
 */
esp_err_t minihub_ble_gatt_deinit(void);

/**
 * @brief True if successful `minihub_ble_gatt_init`; false otherwise.
 */
bool minihub_ble_gatt_ready(void);

/**
 * @brief Start passive / active scan.
 *
 * @param duration_ms    0 → until `minihub_ble_gatt_scan_stop`.
 * @param filter_uuid16  0 → keep ads with name or UUID; else filter by UUID16.
 */
esp_err_t minihub_ble_gatt_scan_start(uint32_t duration_ms, uint16_t filter_uuid16);

/**
 * @brief Stop scan.
 */
esp_err_t minihub_ble_gatt_scan_stop(void);

/**
 * @brief Copy scanned advertisements into caller buffer.
 *
 * @param out   Output array.
 * @param max   Capacity of `out`.
 * @param count Written count.
 */
esp_err_t minihub_ble_gatt_scan_get(minihub_ble_gatt_adv_t *out, size_t max, size_t *count);

/**
 * @brief Connect to a peripheral by BD_ADDR.
 *
 * @param addr       6-byte address.
 * @param addr_type  BLE address type.
 * @param timeout_ms Connection timeout.
 */
esp_err_t minihub_ble_gatt_connect(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms);

/**
 * @brief Disconnect the current GATT connection.
 */
esp_err_t minihub_ble_gatt_disconnect(void);

/**
 * @brief True if a GATT connection is active; false otherwise.
 */
bool minihub_ble_gatt_is_connected(void);

/**
 * @brief NimBLE connection handle for the active link (0 if none).
 */
uint16_t minihub_ble_gatt_conn_handle(void);

/**
 * @brief Discover a primary service by 16-bit UUID.
 *
 * @param svc_uuid16 Service UUID.
 * @param timeout_ms Discovery timeout.
 */
esp_err_t minihub_ble_gatt_discover_svc(uint16_t svc_uuid16, uint32_t timeout_ms);

/**
 * @brief Find characteristic value / CCCD handles by 16-bit UUID.
 *
 * @param char_uuid16 Characteristic UUID.
 * @param val_handle  Out: value attribute handle (may be NULL).
 * @param cccd_handle Out: CCCD handle (may be NULL).
 */
esp_err_t minihub_ble_gatt_find_char(uint16_t char_uuid16, uint16_t *val_handle, uint16_t *cccd_handle);

/**
 * @brief Subscribe to notifications on a characteristic (write CCCD).
 */
esp_err_t minihub_ble_gatt_subscribe(uint16_t cccd_handle,
                                     minihub_ble_gatt_notify_cb_t cb,
                                     void *ctx);

/**
 * @brief Write characteristic value.
 *
 * @param val_handle Value handle.
 * @param data       Payload.
 * @param len        Length.
 * @param no_rsp     True → write without response.
 */
esp_err_t minihub_ble_gatt_write(uint16_t val_handle, const uint8_t *data, size_t len, bool no_rsp);

/**
 * @brief Wait for next notify on any subscribed characteristic.
 *
 * @param attr_handle Filter; 0 = any subscribed characteristic.
 * @param buf         Output buffer.
 * @param buf_len     Buffer size.
 * @param out_len     Bytes copied.
 * @param out_attr    Attribute handle that notified (may be NULL).
 * @param timeout_ms  Timeout.
 *
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_TIMEOUT: Timeout.
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
