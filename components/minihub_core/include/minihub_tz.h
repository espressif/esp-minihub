/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"

/**
 * @brief Load the saved timezone and apply it with setenv("TZ") + tzset().
 * Default is UTC. Call once after nvs_flash_init().
 */
void minihub_tz_apply(void);

/**
 * @brief Persist and apply a fixed offset: "UTC", "UTC+8", "UTC-8", "UTC+5:30".
 *
 * Whole hours from -12 to +14, plus the half and quarter hours in use; the
 * minute field is 00, 15, 30 or 45. `ESP_ERR_INVALID_ARG` for anything else.
 */
esp_err_t minihub_tz_set(const char *id);

const char *minihub_tz_id(void);

/**
 * @brief Minutes east of UTC. Fixed offsets only; no daylight-saving shift.
 */
int minihub_tz_offset_min(void);

/**
 * @brief {Id, offset_min, zones:[id,...]}. Caller deletes.
 */
cJSON *minihub_tz_status_json(void);
