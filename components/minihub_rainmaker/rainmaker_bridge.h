/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_rmaker_core.h"
#include "minihub_core.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rainmaker_bridge_init(esp_rmaker_node_t *node);
esp_err_t rainmaker_bridge_sync_all(void);
esp_err_t rainmaker_bridge_add_point(const minihub_point_t *point);
esp_err_t rainmaker_bridge_remove_point(const char *point_id);
void rainmaker_bridge_on_point_updated(uint16_t point_index);

/* Register the gateway itself (MiniHub) as a RainMaker device, so the App can
 * see hub-level status (network / IP / uptime / memory / connected devices /
 * firmware) and trigger a remote reboot. Call once after node_init. */
esp_err_t rainmaker_bridge_add_hub_device(esp_rmaker_node_t *node);

/* Refresh the hub self-device's telemetry params. Safe to call before MQTT
 * connect (values are queued) and periodically afterwards. */
esp_err_t rainmaker_bridge_update_hub_status(void);

#ifdef __cplusplus
}
#endif
