/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"
#include "minihub_vibe_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "minihub_vibe_apply";

/* Vibe Mode has PSRAM malloc; 64 KiB cuts SD/USB fread and flash write
 * round-trips vs the old 4 KiB buffer (~6 MiB image was ~3 min). */
#define COPY_BUFFER_SIZE (64 * 1024)
#define PROGRESS_EVERY_BYTES (512 * 1024)

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

esp_err_t minihub_vibe_apply(const char *firmware_path, bool *copied, bool force_recovery)
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

    /* Refuse an unexpected firmware.bin before writing ota_0.
     * Pending path: NVS (Host stage) with sidecar fallback. Force-GPIO recovery:
     * sidecar only — NVS has no vibe_sha when the Host could not arm pending. */
    uint8_t expected[MINIHUB_VIBE_SHA256_LEN];
    esp_err_t err = force_recovery ? minihub_vibe_load_expected_sha_sidecar(expected)
                                   : minihub_vibe_load_expected_sha(expected);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no expected SHA (%s): %s",
                 force_recovery ? MINIHUB_VIBE_FIRMWARE_SHA_PATH : "NVS or sidecar",
                 esp_err_to_name(err));
        return err;
    }

    esp_app_desc_t staged;
    err = minihub_vibe_app_desc_from_file(firmware_path, &staged);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_desc %s: %s", firmware_path, esp_err_to_name(err));
        return err;
    }

    /* Product and identity checks use ota_0's Host descriptor, not this
     * factory image (project_name "vibe_mode"). */
    esp_app_desc_t installed;
    bool have_installed = (esp_ota_get_partition_description(part, &installed) == ESP_OK &&
                           installed.magic_word == ESP_APP_DESC_MAGIC_WORD);
    if (have_installed && installed.project_name[0] != '\0') {
        if (strncmp(staged.project_name, installed.project_name,
                    sizeof(staged.project_name)) != 0) {
            ESP_LOGE(TAG, "project_name mismatch: staged='%.*s' ota_0='%.*s'",
                     (int)sizeof(staged.project_name), staged.project_name,
                     (int)sizeof(installed.project_name), installed.project_name);
            return ESP_ERR_INVALID_VERSION;
        }
        if (memcmp(staged.app_elf_sha256, installed.app_elf_sha256,
                   sizeof(staged.app_elf_sha256)) == 0) {
            ESP_LOGI(TAG,
                     "ota_0 app_elf_sha256 matches staged image (%02x%02x%02x%02x…); skip copy",
                     staged.app_elf_sha256[0], staged.app_elf_sha256[1],
                     staged.app_elf_sha256[2], staged.app_elf_sha256[3]);
            (void)minihub_vibe_clear_expected_sha();
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "ota_0 has no app_desc; copy required");
    }

    uint8_t sha256[MINIHUB_VIBE_SHA256_LEN];
    size_t image_size = 0;
    err = minihub_vibe_sha256_file(firmware_path, sha256, &image_size);
    if (err != ESP_OK || image_size == 0) {
        ESP_LOGE(TAG, "hash %s: %s", firmware_path, esp_err_to_name(err));
        return (err == ESP_OK) ? ESP_ERR_INVALID_SIZE : err;
    }
    if (image_size > part->size) {
        ESP_LOGE(TAG, "image size %u exceeds %s (%u)",
                 (unsigned)image_size, part->label, (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(expected, sha256, MINIHUB_VIBE_SHA256_LEN) != 0) {
        ESP_LOGE(TAG, "staged SHA mismatch; refusing copy");
        return ESP_ERR_INVALID_CRC;
    }

    esp_ota_handle_t handle;
    err = esp_ota_begin(part, image_size, &handle);
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

    ESP_LOGI(TAG, "copying %u bytes to %s (sha256=%02x%02x%02x%02x…)",
             (unsigned)image_size, part->label, sha256[0], sha256[1], sha256[2], sha256[3]);

    size_t written = 0;
    size_t next_log = PROGRESS_EVERY_BYTES;
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
        if (written >= next_log || written == image_size) {
            ESP_LOGI(TAG, "progress %u / %u bytes", (unsigned)written, (unsigned)image_size);
            next_log += PROGRESS_EVERY_BYTES;
        }
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

    *copied = true;
    (void)minihub_vibe_clear_expected_sha();
    ESP_LOGI(TAG, "copied %u bytes to %s", (unsigned)image_size, part->label);
    return ESP_OK;
}
