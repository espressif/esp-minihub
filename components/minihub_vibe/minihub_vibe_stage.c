/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_vibe_stage";

static FILE *s_file;
static bool s_stage_begun;

esp_err_t minihub_vibe_stage_begin(size_t expected_size)
{
    if (s_stage_begun) {
        return ESP_ERR_INVALID_STATE;
    }

    minihub_vibe_device_t device = MINIHUB_VIBE_DEVICE_NONE;
    esp_err_t err = minihub_vibe_media_open(&device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no external LittleFS volume: %s", esp_err_to_name(err));
        return err;
    }

    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%s", MINIHUB_VIBE_MOUNT_PATH, MINIHUB_VIBE_FIRMWARE_DIR);
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }

    /* A truncated or interrupted previous download must never be handed to
     * Vibe Mode as if it were a complete image. */
    (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
    (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
    (void)minihub_vibe_clear_expected_sha();
    s_file = fopen(MINIHUB_VIBE_FIRMWARE_PATH, "wb");
    if (!s_file) {
        return ESP_FAIL;
    }
    s_stage_begun = true;
    ESP_LOGI(TAG, "staging %s (device=%d, expected=%u bytes)",
             MINIHUB_VIBE_FIRMWARE_PATH, device, (unsigned)expected_size);
    return ESP_OK;
}

esp_err_t minihub_vibe_stage_write(const void *data, size_t len)
{
    if (!s_stage_begun || !s_file) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    return (fwrite(data, 1, len, s_file) == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t minihub_vibe_stage_finish(void)
{
    if (!s_stage_begun || !s_file) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)fflush(s_file);
    esp_err_t err = (fclose(s_file) == 0) ? ESP_OK : ESP_FAIL;
    s_file = NULL;
    s_stage_begun = false;
    if (err != ESP_OK) {
        (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
        (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
        return err;
    }

    /* Catch cross-product images before reboot into Vibe Mode. */
    const esp_app_desc_t *running = esp_app_get_description();
    err = minihub_vibe_require_project_name(
        MINIHUB_VIBE_FIRMWARE_PATH,
        (running && running->project_name[0]) ? running->project_name : NULL);
    if (err != ESP_OK) {
        (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
        (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
        return err;
    }

    uint8_t sha256[MINIHUB_VIBE_SHA256_LEN];
    size_t image_size = 0;
    err = minihub_vibe_sha256_file(MINIHUB_VIBE_FIRMWARE_PATH, sha256, &image_size);
    if (err != ESP_OK || image_size == 0) {
        ESP_LOGE(TAG, "hash staged image: %s", esp_err_to_name(err));
        (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }
    err = minihub_vibe_write_sha_sidecar(sha256);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write SHA sidecar: %s", esp_err_to_name(err));
        (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
        return err;
    }
    err = minihub_vibe_set_expected_sha(sha256);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store expected SHA: %s", esp_err_to_name(err));
        (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
        (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
        return err;
    }

    err = minihub_vibe_set_pending(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not arm Vibe Mode: %s", esp_err_to_name(err));
    }
    return err;
}

void minihub_vibe_stage_abort(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    s_stage_begun = false;
    (void)unlink(MINIHUB_VIBE_FIRMWARE_PATH);
    (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
    (void)minihub_vibe_clear_expected_sha();
    (void)minihub_vibe_set_pending(false);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_MINIHUB_VIBE_ENTER_REBOOT_DELAY_MS));
    esp_restart();
}

esp_err_t minihub_vibe_enter(void)
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, MINIHUB_VIBE_FACTORY_LABEL);
    if (!factory) {
        ESP_LOGE(TAG, "no %s factory partition", MINIHUB_VIBE_FACTORY_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    /* For an app,factory target ESP-IDF erases otadata instead of writing a
     * slot sequence. The bootloader then defaults to the factory image. */
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "select Vibe Mode: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(reboot_task, "vibe_enter", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "reboot task failed; restarting inline");
        esp_restart();
    }
    return ESP_OK;
}
