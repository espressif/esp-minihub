/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t minihub_config_init(void);
cJSON *minihub_config_get_adapters(void);
cJSON *minihub_config_get_rules(void);
cJSON *minihub_config_get_scenes(void);
esp_err_t minihub_config_reload(void);
/** Write adapters.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_adapters(cJSON *adapters_root);
/** Write rules.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_rules(cJSON *rules_root);
/** Write scenes.json to LittleFS and refresh cache */
esp_err_t minihub_config_save_scenes(cJSON *scenes_root);
/**
 * Write a file under MINIHUB_VFS_MOUNT (e.g. rel_path "ui/screens/store.json").
 * Parent directories are created as needed. Rejects ".." in path.
 */
esp_err_t minihub_fs_write_file(const char *rel_path, const void *data, size_t len);
