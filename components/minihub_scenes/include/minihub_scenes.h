/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t minihub_scenes_init(const cJSON *scenes_cfg);
esp_err_t minihub_scenes_reload(const cJSON *scenes_cfg);
esp_err_t minihub_scenes_invoke(const char *scene_id);
cJSON *minihub_scenes_list_json(void);
