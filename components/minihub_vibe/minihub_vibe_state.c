/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"
#include "minihub_vibe_priv.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "mbedtls/md.h"
#include "nvs.h"

static const char *TAG = "minihub_vibe";

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

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static esp_err_t sha256_from_hex(const char *hex, uint8_t out[MINIHUB_VIBE_SHA256_LEN])
{
    if (!hex || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    while (*hex && isspace((unsigned char)*hex)) {
        ++hex;
    }
    for (size_t i = 0; i < MINIHUB_VIBE_SHA256_LEN; ++i) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_set_expected_sha(const uint8_t sha256[MINIHUB_VIBE_SHA256_LEN])
{
    if (!sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, MINIHUB_VIBE_NVS_KEY_SHA, sha256, MINIHUB_VIBE_SHA256_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t minihub_vibe_clear_expected_sha(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_rw(&handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, MINIHUB_VIBE_NVS_KEY_SHA);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t minihub_vibe_write_sha_sidecar(const uint8_t sha256[MINIHUB_VIBE_SHA256_LEN])
{
    if (!sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(MINIHUB_VIBE_FIRMWARE_SHA_PATH, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    char hex[(MINIHUB_VIBE_SHA256_LEN * 2) + 2];
    for (size_t i = 0; i < MINIHUB_VIBE_SHA256_LEN; ++i) {
        snprintf(&hex[i * 2], 3, "%02x", sha256[i]);
    }
    hex[MINIHUB_VIBE_SHA256_LEN * 2] = '\n';
    hex[MINIHUB_VIBE_SHA256_LEN * 2 + 1] = '\0';
    size_t n = fwrite(hex, 1, MINIHUB_VIBE_SHA256_LEN * 2 + 1, file);
    int closed = fclose(file);
    if (n != MINIHUB_VIBE_SHA256_LEN * 2 + 1 || closed != 0) {
        (void)unlink(MINIHUB_VIBE_FIRMWARE_SHA_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_load_expected_sha_sidecar(uint8_t sha256[MINIHUB_VIBE_SHA256_LEN])
{
    if (!sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(MINIHUB_VIBE_FIRMWARE_SHA_PATH, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    char hex[(MINIHUB_VIBE_SHA256_LEN * 2) + 8];
    size_t n = fread(hex, 1, sizeof(hex) - 1, file);
    fclose(file);
    if (n < MINIHUB_VIBE_SHA256_LEN * 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    hex[n] = '\0';
    return sha256_from_hex(hex, sha256);
}

esp_err_t minihub_vibe_load_expected_sha(uint8_t sha256[MINIHUB_VIBE_SHA256_LEN])
{
    if (!sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MINIHUB_VIBE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = MINIHUB_VIBE_SHA256_LEN;
        err = nvs_get_blob(handle, MINIHUB_VIBE_NVS_KEY_SHA, sha256, &len);
        nvs_close(handle);
        if (err == ESP_OK && len == MINIHUB_VIBE_SHA256_LEN) {
            return ESP_OK;
        }
    }
    return minihub_vibe_load_expected_sha_sidecar(sha256);
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

esp_err_t minihub_vibe_app_desc_from_file(const char *firmware_path, esp_app_desc_t *out)
{
    if (!firmware_path || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(firmware_path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Bare OTA .bin: image header + first segment header, then esp_app_desc. */
    const long off = (long)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    esp_err_t err = ESP_OK;
    if (fseek(file, off, SEEK_SET) != 0 ||
        fread(out, 1, sizeof(*out), file) != sizeof(*out)) {
        err = ESP_FAIL;
    }
    fclose(file);
    if (err != ESP_OK) {
        return err;
    }
    if (out->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t minihub_vibe_require_project_name(const char *firmware_path,
                                            const char *expected_project)
{
    if (!firmware_path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!expected_project || expected_project[0] == '\0') {
        return ESP_OK;
    }
    esp_app_desc_t desc;
    esp_err_t err = minihub_vibe_app_desc_from_file(firmware_path, &desc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no app_desc in %s: %s", firmware_path, esp_err_to_name(err));
        return err;
    }
    if (desc.project_name[0] == '\0') {
        ESP_LOGE(TAG, "no project_name in %s", firmware_path);
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(desc.project_name, expected_project, sizeof(desc.project_name)) != 0) {
        ESP_LOGE(TAG, "project_name mismatch: staged='%.*s' expected='%s'",
                 (int)sizeof(desc.project_name), desc.project_name, expected_project);
        return ESP_ERR_INVALID_VERSION;
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

    uint8_t *buf = malloc(64 * 1024);
    if (!buf) {
        fclose(file);
        mbedtls_md_free(&ctx);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        size_t n = fread(buf, 1, 64 * 1024, file);
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
