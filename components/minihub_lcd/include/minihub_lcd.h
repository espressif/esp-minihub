/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Full init (display HW + UI mount).
 */
esp_err_t minihub_lcd_start(void);

/**
 * @brief RGB panel + LVGL only; call before Matter/Wi-Fi/BLE when internal RAM is tight.
 */
esp_err_t minihub_lcd_start_hw(void);

/**
 * @brief Parse/mount JSON UI; call after network stacks are up. Requires minihub_lcd_start_hw().
 */
esp_err_t minihub_lcd_start_ui(void);

/**
 * @brief Refresh dashboard data (no-op until UI mount completes). Safe from Wi-Fi event handlers.
 */
void minihub_lcd_request_refresh(void);

#ifdef __cplusplus
}
#endif
