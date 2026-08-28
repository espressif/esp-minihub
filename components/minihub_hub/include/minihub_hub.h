/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/** Reload config, packs, modules, adapters (Web /api/hub/reload) */
esp_err_t minihub_hub_reload(void);

/** Run hub_reload on a worker task (coalesce duplicates; avoid blocking UI/RPC) */
esp_err_t minihub_hub_reload_async(void);

/**
 * Soft-reload one adapter from adapters.json (no module dlclose / Zigbee restart).
 * Used after module device sync.
 */
esp_err_t minihub_hub_soft_reload_adapter(const char *adapter_name);
esp_err_t minihub_hub_soft_reload_adapter_async(const char *adapter_name);

/**
 * Remove Hub-side device mapping(s) from JSON request.
 * Request: point_id (single), or delete_all + adapter (batch).
 * Legacy: delete + point_id (Web per-adapter devices POST).
 * Optional reload (default true): hub reload after Matter removal.
 * Sets *resp to a JSON object (caller owns); returns ESP_ERR_NO_MEM on OOM.
 */
esp_err_t minihub_hub_points_remove(const cJSON *req, cJSON **resp);
