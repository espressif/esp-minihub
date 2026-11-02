/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Load rules and start the time and executor tasks.
 *
 * Subscribes to `point.updated`. A NULL `rules_cfg`, or an object whose
 * `rules` member is not an array, clears the table and still starts the tasks.
 *
 * @param rules_cfg `rules.json` object. NULL clears the loaded rules.
 *
 * @return
 *     - ESP_OK: The rules are loaded and both tasks are running.
 *     - ESP_ERR_NO_MEM: The executor queue, a step timer, a task, or the
 *       `point.updated` subscription could not be created.
 */
esp_err_t minihub_rules_init(const cJSON *rules_cfg);

/**
 * @brief Replace the loaded rules.
 *
 * Clears each rule's last-fired time and once-only flag, stops pending step
 * timers, and drops a delayed step that still carries the previous generation.
 *
 * @param rules_cfg `rules.json` object. NULL, or a non-array `rules` member,
 *                  clears the table.
 *
 * @return ESP_OK: The rule table was replaced.
 */
esp_err_t minihub_rules_reload(const cJSON *rules_cfg);

/**
 * @brief Stop rule timers and ask the time and executor tasks to exit.
 *
 * @return ESP_OK: Pending step timers are stopped and both tasks are flagged to exit.
 */
esp_err_t minihub_rules_stop(void);
