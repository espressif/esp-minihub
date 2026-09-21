/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t minihub_config_init(void);

/**
 * Writable data root after init: `/data` (Host LittleFS), `/fatfs` (claw FAT),
 * or `/nand` (Mosaic NAND DATA).
 * Safe before init: returns `/data` if that mount exists, else `/fatfs` or `/nand`
 * when present.
 */
const char *minihub_paths_data_root(void);
/** `{data_root}/tmp` for MHMD extract / scratch files (created by config init). */
const char *minihub_paths_module_tmp_dir(void);
/** `{data_root}/config/packs` (Host `/data/...` or claw `/fatfs/...`). */
const char *minihub_paths_packs_dir(void);

cJSON *minihub_config_get_adapters(void);
cJSON *minihub_config_get_rules(void);
cJSON *minihub_config_get_scenes(void);
esp_err_t minihub_config_reload(void);
/** Write adapters.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_adapters(cJSON *adapters_root);
/**
 * Ensure adapters.json has a slot for @p adapter_name so hub load_all/start
 * will activate a Store-registered adapter.
 * - Missing key → create { "enabled": true, "devices": [] } and save.
 * - Existing key + force_enable → set enabled true (leave devices alone).
 * - Existing key without force_enable → no-op.
 * Used by Store install and by module_loader for factory-preinstalled modules
 * that never go through the install API (otherwise ENABLED ≠ points).
 */
esp_err_t minihub_config_ensure_adapter_slot(const char *adapter_name, bool force_enable);
/** Remove one adapter key from adapters.json (no-op if missing). */
esp_err_t minihub_config_remove_adapter(const char *adapter_name);
/** Drop one manual devices[] entry from adapters.json (no-op if missing). */
esp_err_t minihub_config_remove_adapter_device(const char *adapter_name, const char *point_id);
/** Write rules.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_rules(cJSON *rules_root);
/** Write scenes.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_scenes(cJSON *scenes_root);
/**
 * Write a file under MINIHUB_VFS_MOUNT (e.g. rel_path "ui/screens/store.json").
 * Parent directories are created as needed. Rejects ".." in path.
 */
esp_err_t minihub_fs_write_file(const char *rel_path, const void *data, size_t len);
