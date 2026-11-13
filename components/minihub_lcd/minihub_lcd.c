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

/* Weak stubs keep the Store's bracket calls linkable in the disabled build.
 * Same shape as the weak hooks in minihub_net.c / minihub_web.c, so a linked
 * LCD UI implementation can override them without changing the plugin ABI. */
__attribute__((weak)) void minihub_lcd_flash_write_begin(void)
{
}

__attribute__((weak)) void minihub_lcd_flash_write_end(void)
{
}
#endif
