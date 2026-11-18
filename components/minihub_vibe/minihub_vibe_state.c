/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "minihub_vibe";

static uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    return ~crc;
}

static esp_err_t nvs_open_rw(nvs_handle_t *out)
{
    return nvs_open(MINIHUB_VIBE_NVS_NAMESPACE, NVS_READWRITE, out);
}

esp_err_t minihub_vibe_device_from_nvs(minihub_vibe_device_t *out_device)
{
    if (!out_device) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_device = MINIHUB_VIBE_DEVICE_NONE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MINIHUB_VIBE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    char value[16] = {0};
    size_t len = sizeof(value);
    err = nvs_get_str(handle, MINIHUB_VIBE_NVS_KEY_DEVICE, value, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(value, MINIHUB_VIBE_DEVICE_SDMMC) == 0) {
        *out_device = MINIHUB_VIBE_DEVICE_SDMMC_ID;
    } else if (strcmp(value, MINIHUB_VIBE_DEVICE_USB) == 0) {
        *out_device = MINIHUB_VIBE_DEVICE_USB_ID;
    } else {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_device_to_nvs(minihub_vibe_device_t device)
{
    const char *value = NULL;
    if (device == MINIHUB_VIBE_DEVICE_SDMMC_ID) {
        value = MINIHUB_VIBE_DEVICE_SDMMC;
    } else if (device == MINIHUB_VIBE_DEVICE_USB_ID) {
        value = MINIHUB_VIBE_DEVICE_USB;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, MINIHUB_VIBE_NVS_KEY_DEVICE, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t minihub_vibe_mask_from_nvs(uint8_t *out_mask)
{
    if (!out_mask) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_mask = MINIHUB_VIBE_DEV_MASK_ALL;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(MINIHUB_VIBE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* No namespace yet: keep the default ALL mask set above. */
        return ESP_OK;
    }
    uint8_t mask = 0;
    err = nvs_get_u8(handle, MINIHUB_VIBE_NVS_KEY_DEVICE_MASK, &mask);
    nvs_close(handle);
    if (err == ESP_OK) {
        *out_mask = mask ? mask : MINIHUB_VIBE_DEV_MASK_ALL;
        return ESP_OK;
    }
    /* No mask stored: honour a legacy single-device choice if present. */
    minihub_vibe_device_t dev = MINIHUB_VIBE_DEVICE_NONE;
    if (minihub_vibe_device_from_nvs(&dev) == ESP_OK && dev != MINIHUB_VIBE_DEVICE_NONE) {
        *out_mask = (dev == MINIHUB_VIBE_DEVICE_SDMMC_ID)
                        ? MINIHUB_VIBE_DEV_SDMMC_BIT
                        : MINIHUB_VIBE_DEV_USB_BIT;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_mask_to_nvs(uint8_t mask)
{
    if (mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, MINIHUB_VIBE_NVS_KEY_DEVICE_MASK, mask);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

uint8_t minihub_vibe_mask_effective(void)
{
    uint8_t mask = 0;
#if CONFIG_MINIHUB_VIBE_SDMMC
    mask |= MINIHUB_VIBE_DEV_SDMMC_BIT;
#endif
#if CONFIG_MINIHUB_VIBE_USB
    mask |= MINIHUB_VIBE_DEV_USB_BIT;
#endif
    return mask ? mask : MINIHUB_VIBE_DEV_MASK_ALL;
}

esp_err_t minihub_vibe_set_pending(bool pending)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, MINIHUB_VIBE_NVS_KEY_PENDING, pending ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool minihub_vibe_take_pending(void)
{
    nvs_handle_t handle;
    if (nvs_open_rw(&handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    bool pending = (nvs_get_u8(handle, MINIHUB_VIBE_NVS_KEY_PENDING, &value) == ESP_OK && value != 0);
    if (pending) {
        (void)nvs_set_u8(handle, MINIHUB_VIBE_NVS_KEY_PENDING, 0);
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
    return pending;
}

esp_err_t minihub_vibe_set_result(const char *message)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, MINIHUB_VIBE_NVS_KEY_RESULT, message ? message : "");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t minihub_vibe_get_result(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MINIHUB_VIBE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = buf_len;
    err = nvs_get_str(handle, MINIHUB_VIBE_NVS_KEY_RESULT, buf, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t sha256_begin(mbedtls_md_context_t *ctx)
{
    /* ESP-IDF 6.2 ships mbedTLS 4 (TF-PSA-Crypto): mbedtls/sha256.h is gone.
     * The md API is what the rest of this repo uses (minihub_update/update_fetch.c). */
    mbedtls_md_init(ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(ctx, info, 0) != 0 || mbedtls_md_starts(ctx) != 0) {
        mbedtls_md_free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_sha256_file(const char *path, uint8_t sha256[MINIHUB_VIBE_SHA256_LEN],
                                   size_t *image_size_out)
{
    if (!path || !sha256 || !image_size_out) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    mbedtls_md_context_t ctx;
    esp_err_t err = sha256_begin(&ctx);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    uint8_t *buf = malloc(4096);
    if (!buf) {
        fclose(file);
        mbedtls_md_free(&ctx);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        size_t n = fread(buf, 1, 4096, file);
        if (n == 0) {
            if (ferror(file)) {
                err = ESP_FAIL;
            }
            break;
        }
        if (mbedtls_md_update(&ctx, buf, n) != 0) {
            err = ESP_FAIL;
            break;
        }
        total += n;
    }
    if (err == ESP_OK && mbedtls_md_finish(&ctx, sha256) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        *image_size_out = total;
    }

    free(buf);
    fclose(file);
    mbedtls_md_free(&ctx);
    return err;
}

size_t minihub_vibe_trailer_offset(const esp_partition_t *part)
{
    if (!part || part->erase_size == 0 || part->size <= part->erase_size) {
        return 0;
    }
    return (part->size / part->erase_size - 1) * part->erase_size;
}

esp_err_t minihub_vibe_trailer_read(const esp_partition_t *part, minihub_vibe_trailer_t *out)
{
    if (!part || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = minihub_vibe_trailer_offset(part);
    if (off == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_partition_read(part, off, out, sizeof(*out));
    if (err != ESP_OK) {
        return err;
    }
    if (out->magic != MINIHUB_VIBE_TRAILER_MAGIC) {
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t expected = out->crc32;
    if (crc32_le(0, (const uint8_t *)out, offsetof(minihub_vibe_trailer_t, crc32)) != expected) {
        ESP_LOGW(TAG, "trailer CRC mismatch on %s", part->label);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_trailer_erase(const esp_partition_t *part)
{
    if (!part) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = minihub_vibe_trailer_offset(part);
    if (off == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_partition_erase_range(part, off, part->erase_size);
}

esp_err_t minihub_vibe_trailer_write(const esp_partition_t *part, size_t image_size,
                                     const uint8_t sha256[MINIHUB_VIBE_SHA256_LEN])
{
    if (!part || !sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = minihub_vibe_trailer_erase(part);
    if (err != ESP_OK) {
        return err;
    }
    minihub_vibe_trailer_t trailer = {
        .magic = MINIHUB_VIBE_TRAILER_MAGIC,
        .image_size = (uint32_t)image_size,
    };
    memcpy(trailer.sha256, sha256, MINIHUB_VIBE_SHA256_LEN);
    trailer.crc32 = crc32_le(0, (const uint8_t *)&trailer, offsetof(minihub_vibe_trailer_t, crc32));
    return esp_partition_write(part, minihub_vibe_trailer_offset(part), &trailer, sizeof(trailer));
}

#define MINIHUB_VIBE_NVS_KEY_FAILURES "vibe_fail"

uint8_t minihub_vibe_bump_failure(void)
{
    nvs_handle_t handle;
    if (nvs_open_rw(&handle) != ESP_OK) {
        return 0xff;
    }
    uint8_t count = 0;
    (void)nvs_get_u8(handle, MINIHUB_VIBE_NVS_KEY_FAILURES, &count);
    if (count < 0xff) {
        ++count;
    }
    (void)nvs_set_u8(handle, MINIHUB_VIBE_NVS_KEY_FAILURES, count);
    (void)nvs_commit(handle);
    nvs_close(handle);
    return count;
}

void minihub_vibe_clear_failures(void)
{
    nvs_handle_t handle;
    if (nvs_open_rw(&handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(handle, MINIHUB_VIBE_NVS_KEY_FAILURES, 0);
    (void)nvs_commit(handle);
    nvs_close(handle);
}
