/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_lcd.h"
#include "esp_err.h"

#if CONFIG_MINIHUB_LCD_UI
esp_err_t minihub_lcd_start_impl(void);
esp_err_t minihub_lcd_start_hw_impl(void);
esp_err_t minihub_lcd_start_ui_impl(void);
#endif

esp_err_t minihub_lcd_start(void)
{
#if CONFIG_MINIHUB_LCD_UI
    return minihub_lcd_start_impl();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_lcd_start_hw(void)
{
#if CONFIG_MINIHUB_LCD_UI
    return minihub_lcd_start_hw_impl();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_lcd_start_ui(void)
{
#if CONFIG_MINIHUB_LCD_UI
    return minihub_lcd_start_ui_impl();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if !CONFIG_MINIHUB_LCD_UI
void minihub_lcd_request_refresh(void)
{
}
#endif
