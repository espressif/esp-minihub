/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_client.h"
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "esp_log.h"
#include "mbedtls/md.h"
#include "update_internal.h"

static const char *TAG = "minihub_update";

static void bytes_to_hex(const uint8_t in[32], char hex[65])
{
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = digits[(in[i] >> 4) & 0xf];
        hex[i * 2 + 1] = digits[in[i] & 0xf];
    }
    hex[64] = '\0';
}

static int hex_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    return strcasecmp(a, b) == 0;
}

static esp_err_t md_begin(mbedtls_md_context_t *ctx)
{
    mbedtls_md_init(ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(ctx, info, 0) != 0 || mbedtls_md_starts(ctx) != 0) {
        mbedtls_md_free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t mh_update_sha256_file(const char *path, char hex[65], size_t *size_out)
{
    if (!path || !hex) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    mbedtls_md_context_t ctx;
    esp_err_t err = md_begin(&ctx);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }
    uint8_t buf[1024];
    size_t total = 0;
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), file);
        if (n == 0) {
            break;
        }
        if (mbedtls_md_update(&ctx, buf, n) != 0) {
            err = ESP_FAIL;
            break;
        }
        total += n;
    }
    int ferr = ferror(file);
    fclose(file);
    uint8_t raw[32];
    if (err == ESP_OK && mbedtls_md_finish(&ctx, raw) != 0) {
        err = ESP_FAIL;
    }
    mbedtls_md_free(&ctx);
    if (ferr) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        return err;
    }
    bytes_to_hex(raw, hex);
    if (size_out) {
        *size_out = total;
    }
    return ESP_OK;
}

static esp_err_t mkdir_parents(const char *file_path)
{
    char dir[MH_UPDATE_URL_MAX];
    size_t n = strlen(file_path);
    if (n == 0 || n >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dir, file_path, n + 1);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        return ESP_OK;
    }
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return ESP_FAIL;
            }
        }
        *p = '/';
    }
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t mh_update_http_get(const char *url, char **body, size_t *len, size_t max_len)
{
    if (!url || !body || max_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    *body = NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s HTTP %d", url, status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    char *buf = malloc(max_len);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    while (total + 1 < max_len) {
        int n = esp_http_client_read(client, buf + total, (int)(max_len - total - 1));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        total += (size_t)n;
    }
    if (err == ESP_OK && total + 1 >= max_len) {
        err = ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[total] = '\0';
    *body = buf;
    if (len) {
        *len = total;
    }
    return ESP_OK;
}

esp_err_t mh_update_download_verified(const char *url, const char *dest,
                                      const char *expect_sha, size_t expect_size)
{
    if (!url || !dest) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mkdir_parents(dest);
    if (err != ESP_OK) {
        return err;
    }
    char tmp[MH_UPDATE_URL_MAX];
    int wrote = snprintf(tmp, sizeof(tmp), "%s.part", dest);
    if (wrote < 0 || (size_t)wrote >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 1024,
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    FILE *out = fopen(tmp, "wb");
    if (!out) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    mbedtls_md_context_t ctx;
    err = md_begin(&ctx);
    if (err != ESP_OK) {
        fclose(out);
        unlink(tmp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }
    char *chunk = malloc(1024);
    if (!chunk) {
        mbedtls_md_free(&ctx);
        fclose(out);
        unlink(tmp);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    while (1) {
        int n = esp_http_client_read(client, chunk, 1024);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        if (fwrite(chunk, 1, (size_t)n, out) != (size_t)n ||
            mbedtls_md_update(&ctx, (const unsigned char *)chunk, (size_t)n) != 0) {
            err = ESP_FAIL;
            break;
        }
        total += (size_t)n;
    }
    free(chunk);
    fclose(out);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    uint8_t raw[32];
    char hex[65];
    if (err == ESP_OK && mbedtls_md_finish(&ctx, raw) != 0) {
        err = ESP_FAIL;
    }
    mbedtls_md_free(&ctx);
    if (err != ESP_OK) {
        unlink(tmp);
        return err;
    }
    bytes_to_hex(raw, hex);
    if (expect_size != 0 && total != expect_size) {
        ESP_LOGW(TAG, "size mismatch %s (%u != %u)", dest, (unsigned)total, (unsigned)expect_size);
        unlink(tmp);
        return ESP_ERR_INVALID_SIZE;
    }
    if (expect_sha && expect_sha[0] && !hex_eq(hex, expect_sha)) {
        ESP_LOGW(TAG, "sha256 mismatch %s", dest);
        unlink(tmp);
        return ESP_ERR_INVALID_CRC;
    }
    if (rename(tmp, dest) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}
