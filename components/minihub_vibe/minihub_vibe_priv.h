/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_app_desc.h"
#include "esp_err.h"
#include "minihub_vibe.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read esp_app_desc from a bare OTA .bin (first DROM segment). Internal to
 *  minihub_vibe — kept out of the public header so cycle-avoiding include-only
 *  consumers (e.g. minihub_lcd) do not need esp_app_format on their compile
 *  line. */
esp_err_t minihub_vibe_app_desc_from_file(const char *firmware_path, esp_app_desc_t *out);

#ifdef __cplusplus
}
#endif
