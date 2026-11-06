/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the on-device MQTT broker service (idempotent).
 *
 * Reads `adapters.mqtt_broker` from adapters.json and brings the listener up
 * when `enabled` is true, then keeps it in step with every `hub.reloaded`
 * event: the broker is restarted only when that subtree actually changed, so a
 * reload that touches other adapters leaves third-party clients connected.
 *
 * @return
 *     - ESP_OK: Succeed (including "configured off", which is not an error).
 *     - ESP_ERR_NO_MEM: Mutex or broker task could not be created.
 */
esp_err_t minihub_mqtt_broker_init(void);

#ifdef __cplusplus
}
#endif
