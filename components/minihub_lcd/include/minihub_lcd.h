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

/**
 * @brief Mark a window in which flash writes disable the cache.
 *
 * The RGB panel scans a PSRAM framebuffer by DMA. While a flash operation holds
 * the cache disabled the scan cannot fetch pixels, which shows up as random
 * tearing rather than a stall. Callers bracket the whole write window so the
 * panel can freeze itself instead. No-op when the LCD UI is off.
 */
void minihub_lcd_flash_write_begin(void);

/**
 * @brief Clear the window set by minihub_lcd_flash_write_begin().
 *
 * @note Must run exactly once per begin; the flag is not a refcount, so an
 *       unbalanced call either leaves the panel frozen or unfreezes it early.
 */
void minihub_lcd_flash_write_end(void);

#ifdef __cplusplus
}
#endif
