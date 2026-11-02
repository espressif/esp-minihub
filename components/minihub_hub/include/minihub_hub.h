/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Reload config, packs, modules, adapters (Web /api/hub/reload).
 */
esp_err_t minihub_hub_reload(void);

/**
 * @brief Run hub_reload on a worker task (coalesce duplicates; avoid blocking UI/RPC).
 */
esp_err_t minihub_hub_reload_async(void);

/**
 * @brief Soft-reload one adapter from adapters.json (no module dlclose / Zigbee restart).
 * Used after module device sync.
 */
esp_err_t minihub_hub_soft_reload_adapter(const char *adapter_name);
esp_err_t minihub_hub_soft_reload_adapter_async(const char *adapter_name);

/**
 * @brief Republish one running adapter's points from the live adapters tree without
 * stopping it. Use for config-only changes (a device row appeared, or its
 * status changed) — a full reload unregisters every adapter's points and they
 * only return when start_all() finishes, which blanks the console.
 *
 * @return ESP_ERR_NOT_SUPPORTED if the adapter has no resync().
 */
esp_err_t minihub_hub_resync_adapter(const char *adapter_name);

/**
 * @brief Queue a resync on the hub_reload worker. Use from httpd / RPC handlers: the
 * resync takes the adapter lifecycle lock, which adapter_poll can hold for
 * seconds, so calling the synchronous form from a URI handler stalls the console.
 * If the adapter turns out not to support resync, the worker falls back to a
 * full reload.
 */
esp_err_t minihub_hub_resync_adapter_async(const char *adapter_name);

/**
 * @brief Remove Hub-side device mapping(s) from JSON request.
 * Request: point_id (single), or delete_all + adapter (batch).
 * Legacy: delete + point_id (Web per-adapter devices POST).
 * Optional reload (default true): hub reload after Matter removal.
 * Sets *resp to a JSON object (caller owns); returns ESP_ERR_NO_MEM on OOM.
 */
esp_err_t minihub_hub_points_remove(const cJSON *req, cJSON **resp);
