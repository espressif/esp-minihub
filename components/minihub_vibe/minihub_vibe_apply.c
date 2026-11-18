/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "minihub_vibe_apply";

#define COPY_BUFFER_SIZE 4096

esp_err_t minihub_vibe_return_to_app(void)
{
    const esp_partition_t *app = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, MINIHUB_VIBE_APP_LABEL);
    if (!app) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Rewrites otadata with a valid sequence for ota_0. Without this call the
     * bootloader keeps selecting the factory image on every boot. */
    return esp_ota_set_boot_partition(app);
}

esp_err_t minihub_vibe_apply(const char *firmware_path, bool *copied)
{
    if (!firmware_path || !copied) {
        return ESP_ERR_INVALID_ARG;
    }
    *copied = false;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, MINIHUB_VIBE_APP_LABEL);
    if (!part) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t sha256[MINIHUB_VIBE_SHA256_LEN];
    size_t image_size = 0;
    esp_err_t err = minihub_vibe_sha256_file(firmware_path, sha256, &image_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hash %s: %s", firmware_path, esp_err_to_name(err));
        return err;
    }

    size_t trailer_off = minihub_vibe_trailer_offset(part);
    if (image_size == 0 || trailer_off == 0 || image_size > trailer_off) {
        ESP_LOGE(TAG, "image size %u does not fit before the trailer at 0x%x",
                 (unsigned)image_size, (unsigned)trailer_off);
        return ESP_ERR_INVALID_SIZE;
    }

    minihub_vibe_trailer_t stored;
    if (minihub_vibe_trailer_read(part, &stored) == ESP_OK &&
        stored.image_size == image_size &&
        memcmp(stored.sha256, sha256, MINIHUB_VIBE_SHA256_LEN) == 0) {
        ESP_LOGI(TAG, "ota_0 already holds this image, skipping copy");
        return ESP_OK;
    }

    /* OTA_WITH_SEQUENTIAL_WRITES erases only the sectors the write touches, so
     * a stale trailer at the tail would survive and cause a false "unchanged"
     * result after a different image was copied. Clear it first. */
    err = minihub_vibe_trailer_erase(part);
    if (err != ESP_OK) {
        return err;
    }

    esp_ota_handle_t handle;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    FILE *file = fopen(firmware_path, "rb");
    uint8_t *buf = malloc(COPY_BUFFER_SIZE);
    if (!file || !buf) {
        if (file) {
            fclose(file);
        }
        free(buf);
        (void)esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    while (1) {
        size_t n = fread(buf, 1, COPY_BUFFER_SIZE, file);
        if (n == 0) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            break;
        }
        written += n;
    }
    free(buf);
    fclose(file);

    if (err != ESP_OK || written != image_size) {
        ESP_LOGE(TAG, "short/failed copy: %u of %u bytes", (unsigned)written, (unsigned)image_size);
        (void)esp_ota_abort(handle);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        return err;
    }
    /* The trailer is the commit marker: if power fails before this write the
     * next Vibe Mode run sees a blank trailer and copies the image again. */
    err = minihub_vibe_trailer_write(part, image_size, sha256);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trailer write: %s", esp_err_to_name(err));
        return err;
    }

    *copied = true;
    ESP_LOGI(TAG, "copied %u bytes to %s", (unsigned)image_size, part->label);
    return ESP_OK;
}
