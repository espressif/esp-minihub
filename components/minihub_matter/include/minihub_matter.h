/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t minihub_matter_init(void);
bool minihub_matter_ready(void);

#if !defined(CONFIG_MINIHUB_THREAD_ADAPTER) || (CONFIG_MINIHUB_THREAD_ADAPTER == 0)
static inline bool minihub_thread_ready(void) { return false;}
#endif

/**
 * @brief Attach CHIP ThreadStackManager to MiniHub's already-running OpenThread
 * instance (after minihub_thread_init). Required so FindOperational can use
 * OT DNS-SD / SRP instead of Wi-Fi mDNS alone.
 */
esp_err_t minihub_matter_attach_thread(void);

typedef enum {
    MINIHUB_MATTER_DISCOVERY_NETWORK = 0, /*!< Multi-admin / already on LAN (default). */
    MINIHUB_MATTER_DISCOVERY_BLE_WIFI = 1, /*!< First-time: BLE + inject device Wi-Fi. */
    MINIHUB_MATTER_DISCOVERY_BLE_THREAD = 2, /*!< First-time: BLE + inject Thread dataset. Requires */
                                             /*!< CONFIG_MINIHUB_MATTER_THREAD_VIA_BLE_COMMISSION. */
    MINIHUB_MATTER_DISCOVERY_BLE_ETHERNET = 3, /*!< First-time: BLE, device already on Ethernet (no SSID). */
} minihub_matter_discovery_t;

/**
 * @brief Commission with setup payload / manual code.
 * @param discovery  NETWORK, BLE_WIFI, BLE_THREAD, or BLE_ETHERNET.
 * @param wifi_ssid  Optional; for BLE_WIFI, used before provision NVS fallback.
 * @param wifi_password Optional; same resolution order as ssid.
 */
esp_err_t minihub_matter_commission_code_ex(uint64_t node_id, const char *pairing_code,
                                            minihub_matter_discovery_t discovery,
                                            const char *wifi_ssid, const char *wifi_password);

/**
 * @brief Answer whether commission_code_ex() could start, without starting it.
 *
 * Call this BEFORE writing adapters.json or publishing a 配网中 card: the kickoff
 * can only fail after it has consumed a node id, and a card written first stays
 * behind as commission_phase:"failed" for a device that was never attempted.
 * On failure minihub_matter_last_error() carries the reason.
 *
 * @note minihub_matter_ready() is not a substitute — the commissioner stays
 *       started while the Thread BR is off.
 */
esp_err_t minihub_matter_commission_preflight(minihub_matter_discovery_t discovery,
                                              const char *wifi_ssid);

/**
 * @brief Block until the commission started by commission_code_ex reaches
 * commissioning_success, a failure callback, or timeout.
 * Does not write adapters.json. ESP_OK only after commissioning_success.
 * timeout_ms 0 uses the default (180s). DNS-SD failures that never invoke
 * the pairing callback are reported as ESP_ERR_TIMEOUT.
 *
 * Prefer the async path: set_pending_device + poll GET /api/matter/status.
 * Do not call this from an httpd URI handler.
 */
esp_err_t minihub_matter_commission_wait(uint32_t timeout_ms);

/**
 * @brief Abort commissioning for `node_id` only. Other waiting nodes and the
 * header "cancel all" session stay. Stops CHIP pairing when this node is the
 * one in progress. ESP_ERR_NOT_FOUND when that node has neither an in-flight
 * session nor a waiting row.
 */
esp_err_t minihub_matter_commission_cancel(uint64_t node_id);

/**
 * @brief Abort every waiting Matter node and the in-flight pairing session.
 * This is the header control; a per-node cancel must not call it.
 */
esp_err_t minihub_matter_commission_cancel_all(void);

/**
 * @brief After commission_code_ex returns ESP_OK, stash the device upsert that must
 * run only on commissioning_success. Cleared on failure or 180s watchdog.
 * Relies on weak minihub_hub_reload_async when do_reload is true.
 */
esp_err_t minihub_matter_set_pending_device(uint64_t node_id, const uint16_t *endpoints,
                                            size_t endpoint_count, bool merge_existing,
                                            bool do_reload, const char *transport);

/**
 * @brief Multi-admin: pair with manual code (MT:...) or QR payload — NETWORK discovery.
 */
esp_err_t minihub_matter_commission_code(uint64_t node_id, const char *pairing_code);

/**
 * @brief Multi-admin: on-network pairing with setup PIN (when window advertises PIN).
 */
esp_err_t minihub_matter_commission_pin(uint64_t node_id, uint32_t setup_pin);

esp_err_t minihub_matter_onoff(uint64_t node_id, uint16_t endpoint_id, bool on);

/**
 * @brief Set the LevelControl CurrentLevel (0–254) on endpoint_id of node_id.
 * Sends MoveToLevelWithOnOff so the OnOff state is carried along. ESP_OK when the
 * command was accepted by CHIP; the resulting report arrives via the attr callback.
 */
esp_err_t minihub_matter_level_set(uint64_t node_id, uint16_t endpoint_id, uint8_t level);

/**
 * @brief Set the ColorControl ColorTemperatureMireds on endpoint_id of node_id.
 * Sends MoveToColorTemperature. ColorTemperature is expressed in mireds
 * (mireds = 1e6 / Kelvin); convert at the caller.
 */
esp_err_t minihub_matter_color_temp_set(uint64_t node_id, uint16_t endpoint_id, uint16_t mireds);

/**
 * @brief Set the ColorControl current hue + saturation on endpoint_id of node_id.
 * Sends MoveToHueAndSaturation. hue/saturation are 0–254.
 */
esp_err_t minihub_matter_color_xy_set(uint64_t node_id, uint16_t endpoint_id,
                                      uint8_t hue, uint8_t saturation);

/**
 * @brief Read the OnOff attribute of endpoint_id on node_id.
 *
 * ESP_OK means *out_on was written. ESP_ERR_NOT_FINISHED means the request was
 * accepted and the value will arrive through the callback set by
 * minihub_matter_set_attr_cb(); *out_on is then untouched. Any other code means
 * the read was not accepted. A read accepted this way can still fail
 * asynchronously — that shows up in minihub_matter_last_error(), not here.
 */
esp_err_t minihub_matter_read_onoff(uint64_t node_id, uint16_t endpoint_id, bool *out_on);

/** Matter attribute path for subscribe / multi-attr read. */
typedef struct {
    uint16_t cluster_id;
    uint16_t attribute_id;
} minihub_matter_attr_path_t;

/**
 * @brief Read every (endpoint × attr path) asynchronously; values arrive via
 * minihub_matter_set_attr_cb(). ESP_ERR_NOT_FINISHED means the request was accepted.
 */
esp_err_t minihub_matter_read_attrs(uint64_t node_id, const uint16_t *endpoint_ids, size_t ep_count,
                                    const minihub_matter_attr_path_t *attrs, size_t attr_count);

/**
 * @brief Subscribe to the given attribute paths on every listed endpoint.
 * One subscription per **node**. Idempotent while live.
 * @p attrs must be non-empty. Same return codes as minihub_matter_subscribe_node().
 */
esp_err_t minihub_matter_subscribe_node_attrs(uint64_t node_id, const uint16_t *endpoint_ids,
                                              size_t ep_count, const minihub_matter_attr_path_t *attrs,
                                              size_t attr_count);

/**
 * @brief Subscribe to OnOff (0x0006/0x0000) on every endpoint — thin wrapper
 * around minihub_matter_subscribe_node_attrs() for callers that still want
 * power-only. Prefer subscribe_node_attrs with the capability map paths.
 */
esp_err_t minihub_matter_subscribe_node(uint64_t node_id, const uint16_t *endpoint_ids, size_t count);

/**
 * @brief Tear down a node's subscription and free its tracked row(s).
 *
 * A re-commissioned device gets a NEW node id; rows keyed by the old id are
 * otherwise never reclaimed, so the stale endpoint list stays armed forever
 * while the status JSON still reads the node healthy. Callers use this to drop
 * nodes that are no longer commissioned, and to force a re-arm when a node's
 * endpoint set changes (subscribe_node() will not edit a live subscription).
 *
 * Performs the real teardown esp_matter::controller::send_shutdown_subscription
 * (node_id, subscription_id) when the row was live, then frees the row.
 * ESP_OK when a row existed and was dropped, ESP_ERR_NOT_FOUND when the node had
 * no tracked row, ESP_ERR_INVALID_ARG for node_id < 2.
 */
esp_err_t minihub_matter_drop_node(uint64_t node_id);

/**
 * @brief Tracked subscriptions as an array of
 * { "node_id": <number>, "endpoints": [<number>, ...], "live": <bool>,
 *   "reports": <number> }, one row per subscribed node. "reports" counts the
 * OnOff reports delivered for that node — the value does not need to change for
 * it to move, so it proves a report arrived. It is cumulative per node and resets
 * only when that node's row is dropped (minihub_matter_drop_node) or reallocated to
 * another node; re-arming a node that is not live does not clear it.
 * Caller owns the returned object.
 * NULL on allocation failure.
 */
cJSON *minihub_matter_subscriptions_json(void);

cJSON *minihub_matter_status_json(void);
const char *minihub_matter_last_error(void);

/**
 * @brief Next free Matter node id (>= 2).
 */
esp_err_t minihub_matter_next_node_id(uint64_t *out_node_id);

/**
 * @brief Write matter.devices[] entries for node_id + endpoints into adapters.json.
 * @param transport  Optional: "wifi" | "ethernet" | "thread" (stored on each device).
 */
esp_err_t minihub_matter_devices_upsert(uint64_t node_id, const uint16_t *endpoints, size_t endpoint_count,
                                        bool merge_existing, size_t *synced_count, const char *transport);

/* Compute the point_id string for a Matter node/endpoint under a transport,
 * mirroring the format used when upserting devices. Lets callers (e.g. the web
 * API) record an external name/override keyed by the same id. */
esp_err_t minihub_matter_point_id(uint64_t node_id, uint16_t endpoint_id, const char *transport,
                                  char *out, size_t out_len);

/**
 * @brief Set commission UI phase on all matter.devices[] rows for node_id.
 * @param phase  "waiting" | "failed" | NULL/"" to clear (device is commissioned).
 * @param error_msg  Optional; stored when phase is "failed".
 */
esp_err_t minihub_matter_devices_set_commission_phase(uint64_t node_id, const char *phase,
                                                      const char *error_msg);

/**
 * @brief Mark every matter.devices[] row for node_id as failed, but only when its
 * current phase is "waiting". ESP_ERR_NOT_FOUND when no such row exists.
 */
esp_err_t minihub_matter_devices_fail_if_waiting(uint64_t node_id, const char *error_msg);

/**
 * @brief Mark every matter.devices[] row whose phase is "waiting" as failed.
 */
esp_err_t minihub_matter_devices_fail_all_waiting(const char *error_msg);

/**
 * @brief Remove a Matter point from adapters.json by point_id.
 */
esp_err_t minihub_matter_device_remove(const char *point_id);

/**
 * @brief Basic Information (cluster 0x0028, typically endpoint 0).
 */
typedef struct {
    char manufacturer[64];
    char model[64];
    char sw_version[32];
    char hw_version[16];
} minihub_matter_identity_t;

typedef void (*minihub_matter_identity_cb_t)(uint64_t node_id, const minihub_matter_identity_t *id, void *ctx);

esp_err_t minihub_matter_set_identity_cb(minihub_matter_identity_cb_t cb, void *ctx);

/**
 * @brief Attribute report callback. Only boolean-valued attributes are decoded today
 * (the OnOff attribute, 0x0000 of cluster 0x0006); the cluster and attribute ids
 * are passed through so a consumer can filter on them without a new symbol being
 * added.
 *
 * Called on the CHIP event task with the stack lock held: copy what you need
 * and return. Do not call back into minihub_matter_*.
 */
typedef void (*minihub_matter_attr_bool_cb_t)(uint64_t node_id, uint16_t endpoint_id,
                                              uint32_t cluster_id, uint32_t attribute_id,
                                              bool value, void *ctx);

esp_err_t minihub_matter_set_attr_cb(minihub_matter_attr_bool_cb_t cb, void *ctx);

/**
 * @brief Typed attribute report callback (Item 3). Superset of the bool callback:
 * `value` carries the decoded attribute (or UNKNOWN when the delivery layer has
 * not yet decoded that type). The OnOff boolean case is delivered with
 * `type == MINIHUB_MATTER_VAL_BOOL` so existing consumers can migrate.
 *
 * Numeric decoding (U8/U16/I16/U32/I32/FLOAT) is enabled once
 * `matter_ctrl.cpp::on_attr_report` decodes the TLV beyond bool — until then
 * non-boolean reports arrive as MINIHUB_MATTER_VAL_UNKNOWN and a consumer should
 * fall back to the bool callback for OnOff. This API is additive; the bool
 * callback above is kept for backward compatibility.
 *
 * Called on the CHIP event task with the stack lock held: copy what you need
 * and return. Do not call back into minihub_matter_*.
 */
typedef enum {
    MINIHUB_MATTER_VAL_BOOL = 0,
    MINIHUB_MATTER_VAL_UINT,   /*!< unsigned integer attribute (any width) */
    MINIHUB_MATTER_VAL_INT,    /*!< signed integer attribute (any width) */
    MINIHUB_MATTER_VAL_DOUBLE, /*!< float / double attribute */
    MINIHUB_MATTER_VAL_UNKNOWN
} minihub_matter_attr_value_type_t;

typedef struct {
    minihub_matter_attr_value_type_t type;
    union {
        bool b;
        uint64_t u; /*!< unsigned integer attribute (any width) */
        int64_t i;  /*!< signed integer attribute (any width) */
        double d;   /*!< float / double attribute */
    } v;
} minihub_matter_attr_value_t;

typedef void (*minihub_matter_attr_typed_cb_t)(uint64_t node_id, uint16_t endpoint_id,
                                               uint32_t cluster_id, uint32_t attribute_id,
                                               const minihub_matter_attr_value_t *value,
                                               void *ctx);

esp_err_t minihub_matter_set_attr_typed_cb(minihub_matter_attr_typed_cb_t cb, void *ctx);

/**
 * @brief Best-effort read of Basic Information. Failure does not unwind commission.
 */
esp_err_t minihub_matter_interview_identity(uint64_t node_id);

/**
 * @brief Offer a Matter OTA image (CSA Provider 0x0029 + AnnounceOTAProvider 0x002A + BDX).
 *
 * `file_path` is a local Matter OTA image (CSA header) or an `http(s)` URL.
 * Local files are served on loopback for the ESP-Matter BDX HTTP downloader.
 * Sends AnnounceOTAProvider to the requestor on endpoint 0.
 */
esp_err_t minihub_matter_ota_offer(const char *point_id, const char *file_path);

#ifdef __cplusplus
}
#endif
