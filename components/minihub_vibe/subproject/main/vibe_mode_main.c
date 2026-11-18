/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Retained maintenance application (app,factory). It only stages nothing and
 * copies a firmware image from external LittleFS storage into ota_0, then
 * rewrites otadata so the next boot returns to the normal application. */

#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minihub_vibe.h"
#include "nvs_flash.h"

static const char *TAG = "vibe_mode";

#define MAX_FAILURES 3

void app_main(void)
{
    /* Never erase NVS here: it holds Wi-Fi credentials, RainMaker identity and
     * the Host's OTA configuration. A failed init only disables the stored
     * device preference, which falls back to probing both devices. */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init: %s (device preference ignored)", esp_err_to_name(err));
    }

    const bool pending = minihub_vibe_take_pending();
    ESP_LOGI(TAG, "Vibe Mode start (pending=%d)", pending ? 1 : 0);

    const char *result = "no media";
    bool copied = false;
    bool ok = false;

    minihub_vibe_device_t device = MINIHUB_VIBE_DEVICE_NONE;
    err = minihub_vibe_media_open(&device);
    if (err == ESP_OK) {
        struct stat st;
        if (stat(MINIHUB_VIBE_FIRMWARE_PATH, &st) == 0 && S_ISREG(st.st_mode)) {
            err = minihub_vibe_apply(MINIHUB_VIBE_FIRMWARE_PATH, &copied);
            ok = (err == ESP_OK);
            if (!ok) {
                ESP_LOGE(TAG, "copy failed: %s", esp_err_to_name(err));
            }
            result = ok ? (copied ? "copied" : "unchanged") : "copy failed";
        } else {
            ESP_LOGI(TAG, "no %s", MINIHUB_VIBE_FIRMWARE_PATH);
            result = "no firmware";
            /* An absent image is not a failure: the pending flag may simply be
             * stale, and the running ota_0 image is still valid. */
            ok = true;
        }
    } else {
        ESP_LOGI(TAG, "no external LittleFS volume: %s", esp_err_to_name(err));
        result = "no media";
        ok = true;
    }

    (void)minihub_vibe_media_close();

    if (ok) {
        minihub_vibe_clear_failures();
        (void)minihub_vibe_set_result(result);
        err = minihub_vibe_return_to_app();
        if (err != ESP_OK) {
            /* ota_0 is not a valid image (for example a blank factory flash).
             * Do not reboot: the bootloader would select this app again forever. */
            ESP_LOGE(TAG, "cannot select ota_0: %s; staying in Vibe Mode", esp_err_to_name(err));
            (void)minihub_vibe_set_result("ota_0 invalid");
            return;
        }
        ESP_LOGI(TAG, "back to ota_0 (%s)", result);
        esp_restart();
        return;
    }

    uint8_t failures = minihub_vibe_bump_failure();
    (void)minihub_vibe_set_result(result);
    if (failures >= MAX_FAILURES) {
        ESP_LOGE(TAG, "copy failed %u times; staying in Vibe Mode. Replace the media and reboot.",
                 (unsigned)failures);
        return;
    }

    /* otadata is still erased, so the bootloader selects this factory app again
     * and the copy is retried with whatever media is present after the reboot. */
    ESP_LOGW(TAG, "retry %u/%d in 3 s", (unsigned)failures, MAX_FAILURES);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}
