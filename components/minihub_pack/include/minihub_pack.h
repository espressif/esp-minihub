/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Scan and load packs from /data/config/packs/.
 */
esp_err_t minihub_pack_init(void);

/**
 * @brief Merge enabled pack devices/config into adapters.json (in-place).
 */
esp_err_t minihub_pack_merge_into_adapters(cJSON *adapters_root);

/**
 * @brief Rescan and merge (Web hot-reload).
 */
esp_err_t minihub_pack_reload(cJSON *adapters_root);

/**
 * @brief Number of loaded packs.
 */
size_t minihub_pack_count(void);

/**
 * @brief Snapshot of loaded packs (JSON array; caller cJSON_Delete).
 */
cJSON *minihub_pack_list_json(void);

/**
 * @brief Enable/disable a pack (writes json under packs/).
 */
esp_err_t minihub_pack_set_enabled(const char *id, bool enabled);

/**
 * @brief Delete pack file and rescan.
 */
esp_err_t minihub_pack_delete(const char *id);
