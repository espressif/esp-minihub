/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal ZIP reader for .hub packages (store + raw deflate via ROM tinfl).
 */

#include "hub_zip.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MINIHUB_HUB_ZIP_HOST_TEST
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
/* Keep in sync with minihub_paths.h MINIHUB_PATH_MAX */
#ifndef MINIHUB_PATH_MAX
#define MINIHUB_PATH_MAX 256
#endif
#else
#include "esp_log.h"
#include "minihub_paths.h"
#endif

static const char *TAG = "hub_zip";

#define ZIP_EOCD_SIG          0x06054b50u
#define ZIP_CD_SIG            0x02014b50u
#define ZIP_LFH_SIG           0x04034b50u
#define ZIP_METHOD_STORE      0
#define ZIP_METHOD_DEFLATE    8
#define HUB_ZIP_MAX_PATH      MINIHUB_PATH_MAX
#define HUB_ZIP_MAX_FILES     64
#define HUB_ZIP_MAX_UNCOMP    (2u * 1024u * 1024u)

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0755) == 0) {
        return ESP_OK;
    }
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t mkdir_p(const char *path)
{
    char tmp[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            esp_err_t err = ensure_dir(tmp);
            *p = '/';
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ensure_dir(tmp);
}

static esp_err_t ensure_parent_dirs(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return ESP_OK;
    }
    *slash = '\0';
    esp_err_t err = mkdir_p(path);
    *slash = '/';
    return err;
}

bool minihub_hub_zip_path_ok(const char *name)
{
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\') {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0 || n >= HUB_ZIP_MAX_PATH) {
        return false;
    }
    const char *p = name;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
            return false;
        }
        if (p[0] == '\\') {
            return false;
        }
        const char *slash = strchr(p, '/');
        size_t seg = slash ? (size_t)(slash - p) : strlen(p);
        if (seg == 0) {
            return false;
        }
        if (seg == 1 && p[0] == '.') {
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

esp_err_t minihub_hub_remove_tree(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    DIR *d = opendir(path);
    if (!d) {
        return (errno == ENOENT) ? ESP_OK : ESP_FAIL;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[256];
        if (snprintf(child, sizeof(child), "%s/%s", path, ent->d_name) >= (int)sizeof(child)) {
            closedir(d);
            return ESP_ERR_INVALID_SIZE;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            esp_err_t err = minihub_hub_remove_tree(child);
            if (err != ESP_OK) {
                closedir(d);
                return err;
            }
        } else {
            unlink(child);
        }
    }
    closedir(d);
    if (rmdir(path) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_file_bytes(const char *path, const uint8_t *data, size_t len)
{
    char mutable[256];
    if (strlen(path) >= sizeof(mutable)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strncpy(mutable, path, sizeof(mutable) - 1);
    mutable[sizeof(mutable) - 1] = '\0';
    esp_err_t err = ensure_parent_dirs(mutable);
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

#ifdef MINIHUB_HUB_ZIP_HOST_TEST
#include <zlib.h>

static esp_err_t inflate_raw(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_len, size_t *out_written)
{
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        return ESP_FAIL;
    }
    strm.next_in = (Bytef *)(uintptr_t)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)dst_len;
    int z = inflate(&strm, Z_FINISH);
    size_t written = (size_t)strm.total_out;
    inflateEnd(&strm);
    if (z != Z_STREAM_END) {
        return ESP_FAIL;
    }
    if (out_written) {
        *out_written = written;
    }
    return ESP_OK;
}
#else
#include "esp_heap_caps.h"
#include "miniz.h"

/* ROM tinfl_decompress_mem_to_mem() puts tinfl_decompressor (~11KB) on the
 * caller's stack. Claw httpd is 12KB, so Store install of a deflated .mhub
 * trips stack protection. Keep the ROM coroutine, allocate state on heap. */
static esp_err_t inflate_raw(const uint8_t *src, size_t src_len,
                             uint8_t *dst, size_t dst_len, size_t *out_written)
{
    tinfl_decompressor *decomp = heap_caps_malloc(sizeof(*decomp),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!decomp) {
        decomp = heap_caps_malloc(sizeof(*decomp), MALLOC_CAP_8BIT);
    }
    if (!decomp) {
        return ESP_ERR_NO_MEM;
    }

    tinfl_init(decomp);
    size_t in_sz = src_len;
    size_t out_sz = dst_len;
    tinfl_status st = tinfl_decompress(
        decomp, src, &in_sz, dst, dst, &out_sz,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(decomp);

    if (st != TINFL_STATUS_DONE) {
        return ESP_FAIL;
    }
    if (out_written) {
        *out_written = out_sz;
    }
    return ESP_OK;
}
#endif

static const uint8_t *find_eocd(const uint8_t *zip, size_t zip_len, size_t *eocd_off)
{
    if (zip_len < 22) {
        return NULL;
    }
    size_t max_comment = 65535;
    size_t scan = zip_len < (22 + max_comment) ? zip_len : (22 + max_comment);
    for (size_t i = 0; i + 22 <= scan; i++) {
        size_t off = zip_len - 22 - i;
        if (rd32(zip + off) == ZIP_EOCD_SIG) {
            uint16_t comment = rd16(zip + off + 20);
            if (off + 22 + comment == zip_len) {
                if (eocd_off) {
                    *eocd_off = off;
                }
                return zip + off;
            }
        }
    }
    return NULL;
}

esp_err_t minihub_hub_zip_extract(const uint8_t *zip, size_t zip_len, const char *dest_dir)
{
    if (!zip || !dest_dir || dest_dir[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t eocd_off = 0;
    const uint8_t *eocd = find_eocd(zip, zip_len, &eocd_off);
    if (!eocd) {
        ESP_LOGE(TAG, "not a zip (EOCD missing)");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t disk_entries = rd16(eocd + 8);
    uint16_t total_entries = rd16(eocd + 10);
    uint32_t cd_size = rd32(eocd + 12);
    uint32_t cd_off = rd32(eocd + 16);
    if (disk_entries != total_entries) {
        ESP_LOGE(TAG, "multi-disk zip not supported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (total_entries > HUB_ZIP_MAX_FILES) {
        ESP_LOGE(TAG, "too many zip entries (%u)", (unsigned)total_entries);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)cd_off + (size_t)cd_size > zip_len || (size_t)cd_off + (size_t)cd_size > eocd_off) {
        ESP_LOGE(TAG, "central directory out of range");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_dir(dest_dir);
    if (err != ESP_OK) {
        return err;
    }

    size_t cd_end = cd_off + cd_size;
    size_t pos = cd_off;
    for (uint16_t i = 0; i < total_entries; i++) {
        if (pos + 46 > cd_end || rd32(zip + pos) != ZIP_CD_SIG) {
            ESP_LOGE(TAG, "bad central directory entry %u", (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t method = rd16(zip + pos + 10);
        uint32_t comp_size = rd32(zip + pos + 20);
        uint32_t uncomp_size = rd32(zip + pos + 24);
        uint16_t name_len = rd16(zip + pos + 28);
        uint16_t extra_len = rd16(zip + pos + 30);
        uint16_t comment_len = rd16(zip + pos + 32);
        uint32_t lfh_off = rd32(zip + pos + 42);
        if (pos + 46 + name_len + extra_len + comment_len > cd_end) {
            return ESP_ERR_INVALID_ARG;
        }

        char name[HUB_ZIP_MAX_PATH];
        if (name_len == 0 || name_len >= sizeof(name)) {
            ESP_LOGE(TAG, "bad entry name length");
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(name, zip + pos + 46, name_len);
        name[name_len] = '\0';
        pos += 46 + name_len + extra_len + comment_len;

        if (!minihub_hub_zip_path_ok(name)) {
            ESP_LOGE(TAG, "rejected zip path: %s", name);
            return ESP_ERR_INVALID_ARG;
        }

        size_t namelen = strlen(name);
        bool is_dir = (namelen > 0 && name[namelen - 1] == '/');
        char out_path[256];
        if (snprintf(out_path, sizeof(out_path), "%s/%s", dest_dir, name) >= (int)sizeof(out_path)) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (is_dir) {
            out_path[strlen(out_path) - 1] = '\0';
            err = mkdir_p(out_path);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "mkdir %s failed", out_path);
                return err;
            }
            continue;
        }

        if (lfh_off + 30 > zip_len || rd32(zip + lfh_off) != ZIP_LFH_SIG) {
            ESP_LOGE(TAG, "bad local header for %s", name);
            return ESP_ERR_INVALID_ARG;
        }
        uint16_t l_name = rd16(zip + lfh_off + 26);
        uint16_t l_extra = rd16(zip + lfh_off + 28);
        size_t data_off = (size_t)lfh_off + 30 + l_name + l_extra;
        if (data_off + comp_size > zip_len) {
            ESP_LOGE(TAG, "compressed data OOB for %s", name);
            return ESP_ERR_INVALID_ARG;
        }
        if (uncomp_size > HUB_ZIP_MAX_UNCOMP) {
            ESP_LOGE(TAG, "entry too large: %s (%u)", name, (unsigned)uncomp_size);
            return ESP_ERR_INVALID_SIZE;
        }

        const uint8_t *cdata = zip + data_off;
        if (method == ZIP_METHOD_STORE) {
            if (comp_size != uncomp_size) {
                return ESP_ERR_INVALID_ARG;
            }
            err = write_file_bytes(out_path, cdata, comp_size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "write %s failed", out_path);
                return err;
            }
        } else if (method == ZIP_METHOD_DEFLATE) {
            uint8_t *out = malloc(uncomp_size ? uncomp_size : 1);
            if (!out) {
                return ESP_ERR_NO_MEM;
            }
            size_t written = 0;
            err = inflate_raw(cdata, comp_size, out, uncomp_size, &written);
            if (err != ESP_OK || written != uncomp_size) {
                free(out);
                ESP_LOGE(TAG, "inflate failed for %s", name);
                return ESP_FAIL;
            }
            err = write_file_bytes(out_path, out, written);
            free(out);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "write %s failed", out_path);
                return err;
            }
        } else {
            ESP_LOGE(TAG, "unsupported method %u for %s", (unsigned)method, name);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    ESP_LOGI(TAG, "extracted %u entries -> %s", (unsigned)total_entries, dest_dir);
    return ESP_OK;
}
