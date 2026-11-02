/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Load scenes from `scenes.json`.
 *
 * Bumps the run token, which cancels an in-flight scene. A NULL `scenes_cfg`,
 * or an object whose `scenes` member is not an array, leaves the table empty.
 *
 * @param scenes_cfg `scenes.json` object. NULL clears the loaded scenes.
 *
 * @return ESP_OK: The scene table was replaced.
 */
esp_err_t minihub_scenes_init(const cJSON *scenes_cfg);

/**
 * @brief Replace the loaded scenes and cancel an in-flight run.
 *
 * @param scenes_cfg `scenes.json` object. NULL, or a non-array `scenes` member,
 *                   clears the table.
 *
 * @return ESP_OK: The scene table was replaced.
 */
esp_err_t minihub_scenes_reload(const cJSON *scenes_cfg);

/**
 * @brief Queue a loaded scene on the worker task.
 *
 * A new request cancels the scene that is already running.
 *
 * @param scene_id Scene `id` from the loaded table.
 *
 * @return
 *     - ESP_OK: The scene was queued.
 *     - ESP_ERR_INVALID_ARG: `scene_id` is NULL.
 *     - ESP_ERR_NOT_FOUND: No loaded scene has this id.
 *     - ESP_ERR_NO_MEM: The worker task could not be started.
 */
esp_err_t minihub_scenes_invoke(const char *scene_id);

/**
 * @brief Build a caller-owned array of the loaded scenes.
 *
 * Each item has `id`, `name`, `steps`, and `max_duration_ms`.
 *
 * @return Caller-owned array, or NULL if the array cannot be created.
 */
cJSON *minihub_scenes_list_json(void);
