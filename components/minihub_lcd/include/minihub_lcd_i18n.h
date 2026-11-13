/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LCD UI language. The dictionary lives in ui/i18n/strings.json; both the
 * Brookesia JSON constants and the firmware-side labels are generated from it.
 * Switching language re-parses root.json with a new Environment.language. */

void lcd_i18n_init(void);
const char *lcd_language(void);
const char *lcd_tr(const char *key);
uint32_t lcd_i18n_generation(void);

/* Returns ESP_OK when the language changed, ESP_ERR_INVALID_ARG for an
 * unsupported language, ESP_ERR_INVALID_STATE when it is already active. */
esp_err_t lcd_i18n_set(const char *lang);

/* Public entry point used by the system screen / Web API. */
esp_err_t minihub_lcd_set_language(const char *lang);

#ifdef __cplusplus
}
#endif
