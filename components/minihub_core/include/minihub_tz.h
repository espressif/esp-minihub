/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"

/**
 * Load the saved timezone and apply it with setenv("TZ") + tzset().
 * Default is UTC. Call once after nvs_flash_init().
 */
void minihub_tz_apply(void);

/** Persist and apply "UTC", "UTC+8", or "UTC-8" (whole hours, -12 through +14). */
esp_err_t minihub_tz_set(const char *id);

const char *minihub_tz_id(void);

/** Minutes east of UTC. Fixed offsets only; no daylight-saving shift. */
int minihub_tz_offset_min(void);

/** {id, offset_min, zones:[id,...]}. Caller deletes. */
cJSON *minihub_tz_status_json(void);
