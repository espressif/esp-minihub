/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_provision.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "minihub_config.h"
#include "minihub_store.h"

#if CONFIG_MINIHUB_FACTORY_RESET_BUTTON
#include "driver/gpio.h"
#endif

static const char *TAG = "minihub_factory";

void minihub_factory_reset_execute(void)
{
    ESP_LOGW(TAG, "factory reset: clearing user data then erasing NVS");
    /* Clear user data on the filesystem (rules / scenes / adapters and any
     * Store-installed modules & packs). NVS erase below handles wifi/network
     * credentials and the store custom URL. Factory preinstall and the Web
     * console are intentionally preserved. */
    minihub_config_factory_reset();
#if CONFIG_MINIHUB_STORE
    minihub_store_factory_reset();
#endif
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void factory_reset_deferred(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    minihub_factory_reset_execute();
}

esp_err_t minihub_factory_reset_schedule(void)
{
    BaseType_t ok = xTaskCreate(factory_reset_deferred, "factory_web", 3072, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

#if CONFIG_MINIHUB_FACTORY_RESET_BUTTON

#define POLL_MS 50

static bool button_pressed(void)
{
    int level = gpio_get_level(CONFIG_MINIHUB_FACTORY_RESET_GPIO);
#if CONFIG_MINIHUB_FACTORY_RESET_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static void factory_reset_task(void *arg)
{
    (void)arg;
    int held_ms = 0;
    const int hold_ms = CONFIG_MINIHUB_FACTORY_RESET_HOLD_MS;

    ESP_LOGI(TAG, "BOOT factory-reset armed (gpio=%d active_%s hold=%dms)",
             CONFIG_MINIHUB_FACTORY_RESET_GPIO,
#if CONFIG_MINIHUB_FACTORY_RESET_ACTIVE_LOW
             "low",
#else
             "high",
#endif
             hold_ms);

    while (1) {
        if (button_pressed()) {
            held_ms += POLL_MS;
            if (held_ms == POLL_MS || (held_ms % 1000) < POLL_MS) {
                ESP_LOGI(TAG, "factory-reset hold %d/%d ms", held_ms, hold_ms);
            }
            if (held_ms >= hold_ms) {
                minihub_factory_reset_execute();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t minihub_factory_reset_button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_MINIHUB_FACTORY_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_MINIHUB_FACTORY_RESET_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreate(factory_reset_task, "factory_rst", 3072, NULL, 5, NULL);
    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

#else /* !CONFIG_MINIHUB_FACTORY_RESET_BUTTON */

esp_err_t minihub_factory_reset_button_start(void)
{
    return ESP_OK;
}

#endif
