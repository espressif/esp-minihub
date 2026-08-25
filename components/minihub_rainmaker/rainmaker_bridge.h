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

#ifdef __cplusplus
}
#endif
