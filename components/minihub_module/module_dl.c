/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_module.h"
#include "minihub_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "module_dl";

#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
#include "esp_dlfcn.h"
#endif

typedef struct {
    void *handle;
    char path[160];
} dl_slot_t;

static dl_slot_t s_dl_slots[8];
static size_t s_dl_count;

static bool path_ends_with(const char *path, const char *suffix)
{
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    if (plen < slen) {
        return false;
    }
    return strcmp(path + plen - slen, suffix) == 0;
}

static esp_err_t ensure_module_tmp_dir(void)
{
    struct stat st;
    if (stat(MINIHUB_MODULE_TMP_DIR, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(MINIHUB_MODULE_TMP_DIR, 0755) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", MINIHUB_MODULE_TMP_DIR, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * Extract .mhub → .so using one FILE* at a time. With a high
 * CONFIG_LWIP_MAX_SOCKETS, VFS non-socket FDs are scarce (FD_SETSIZE -
 * MAX_SOCKETS); holding src+dst open together often fails fopen(dst).
 */
static esp_err_t extract_mhub_payload(const char *src, char *dst, size_t dst_len)
{
    (void)dst_len;
    esp_err_t dir_err = ensure_module_tmp_dir();
    if (dir_err != ESP_OK) {
        return dir_err;
    }

    FILE *in = fopen(src, "rb");
    if (!in) {
        ESP_LOGE(TAG, "cannot open mhub %s: errno=%d", src, errno);
        return ESP_ERR_NOT_FOUND;
    }

    minihub_module_image_header_t hdr;
    size_t n = fread(&hdr, 1, sizeof(hdr), in);
    if (n != sizeof(hdr) || hdr.magic != MINIHUB_MODULE_MAGIC) {
        fclose(in);
        ESP_LOGE(TAG, "bad mhub header: %s", src);
        return ESP_ERR_INVALID_CRC;
    }
    if (hdr.abi_version != MINIHUB_MODULE_ABI_VERSION) {
        fclose(in);
        ESP_LOGE(TAG, "mhub abi %u != host %u: %s", (unsigned)hdr.abi_version,
                 (unsigned)MINIHUB_MODULE_ABI_VERSION, src);
        return ESP_ERR_INVALID_VERSION;
    }

    /* Prefer declared size; fall back to rest-of-file. */
    size_t payload_len = hdr.image_size;
    if (payload_len == 0) {
        long pos = ftell(in);
        if (fseek(in, 0, SEEK_END) != 0) {
            fclose(in);
            return ESP_FAIL;
        }
        long end = ftell(in);
        if (pos < 0 || end < pos) {
            fclose(in);
            return ESP_FAIL;
        }
        payload_len = (size_t)(end - pos);
        if (fseek(in, pos, SEEK_SET) != 0) {
            fclose(in);
            return ESP_FAIL;
        }
    }

    uint8_t *payload = heap_caps_malloc(payload_len ? payload_len : 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
        payload = malloc(payload_len ? payload_len : 1);
    }
    if (!payload) {
        fclose(in);
        ESP_LOGE(TAG, "no mem for mhub payload (%u B)", (unsigned)payload_len);
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(payload, 1, payload_len, in);
    fclose(in);
    in = NULL;
    if (got != payload_len) {
        free(payload);
        ESP_LOGE(TAG, "short mhub read %u / %u: %s", (unsigned)got, (unsigned)payload_len, src);
        return ESP_FAIL;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        free(payload);
        ESP_LOGE(TAG, "cannot create extract path %s: errno=%d", dst, errno);
        return ESP_FAIL;
    }
    size_t written = fwrite(payload, 1, payload_len, out);
    fclose(out);
    free(payload);

    if (written != payload_len) {
        unlink(dst);
        ESP_LOGE(TAG, "short mhub write %u / %u: %s", (unsigned)written, (unsigned)payload_len, dst);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void module_mhub_tmp_paths(unsigned stamp, char *rel, size_t rel_len,
                                  char *abs, size_t abs_len)
{
    snprintf(rel, rel_len, "tmp/mod_%u.so", stamp);
    snprintf(abs, abs_len, "%s/mod_%u.so", MINIHUB_MODULE_TMP_DIR, stamp);
}

static esp_err_t resolve_load_path(const char *binary_path, char *rel_out, size_t rel_len,
                                   char *abs_out, size_t abs_len)
{
    if (path_ends_with(binary_path, ".mhub") || path_ends_with(binary_path, ".so")) {
        unsigned stamp = (unsigned)(esp_log_timestamp() & 0xFFFF);
        /* dlopen path is relative to CONFIG_ELF_FILE_SYSTEM_BASE_PATH (/data) */
        module_mhub_tmp_paths(stamp, rel_out, rel_len, abs_out, abs_len);
        return extract_mhub_payload(binary_path, abs_out, abs_len);
    }
    abs_out[0] = '\0';
    /* Strip VFS prefix so esp_elf_open(FS_PATH/name) resolves correctly */
    if (strncmp(binary_path, MINIHUB_VFS_MOUNT, strlen(MINIHUB_VFS_MOUNT)) == 0 &&
        binary_path[strlen(MINIHUB_VFS_MOUNT)] == '/') {
        strncpy(rel_out, binary_path + strlen(MINIHUB_VFS_MOUNT) + 1, rel_len - 1);
        rel_out[rel_len - 1] = '\0';
        return ESP_OK;
    }
    strncpy(rel_out, binary_path, rel_len - 1);
    rel_out[rel_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t minihub_module_dl_load(const char *binary_path,
                                 minihub_module_entry_fn *out_entry,
                                 minihub_module_rpc_fn *out_rpc,
                                 void **out_handle)
{
    if (!binary_path || !out_entry) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_rpc) {
        *out_rpc = NULL;
    }

#if !CONFIG_MINIHUB_DYNAMIC_MODULES
    (void)out_handle;
    return ESP_ERR_NOT_SUPPORTED;
#elif !defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) || !CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
    (void)out_handle;
    ESP_LOGW(TAG, "dynamic SO load disabled; enable CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT");
    return ESP_ERR_NOT_SUPPORTED;
#else
    char load_path[64];
    char abs_path[160];
    esp_err_t err = resolve_load_path(binary_path, load_path, sizeof(load_path),
                                      abs_path, sizeof(abs_path));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "resolve/extract failed for %s: %s", binary_path, esp_err_to_name(err));
        return err;
    }

    void *handle = dlopen(load_path, RTLD_NOW);
    if (!handle) {
        const char *msg = dlerror();
        ESP_LOGE(TAG, "dlopen failed: %s (%s)", load_path, msg ? msg : "unknown");
        if (abs_path[0]) {
            unlink(abs_path);
        }
        return ESP_FAIL;
    }

    minihub_module_entry_fn entry = (minihub_module_entry_fn)dlsym(handle, MINIHUB_MODULE_ENTRY_SYMBOL);
    if (!entry) {
        const char *msg = dlerror();
        ESP_LOGE(TAG, "dlsym %s failed: %s", MINIHUB_MODULE_ENTRY_SYMBOL, msg ? msg : "unknown");
        dlclose(handle);
        if (abs_path[0]) {
            unlink(abs_path);
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (out_rpc) {
        minihub_module_rpc_fn rpc =
            (minihub_module_rpc_fn)dlsym(handle, MINIHUB_MODULE_RPC_SYMBOL);
        *out_rpc = rpc;
        if (rpc) {
            ESP_LOGI(TAG, "module RPC symbol loaded");
        }
    }

    if (s_dl_count < 8) {
        dl_slot_t *slot = &s_dl_slots[s_dl_count++];
        slot->handle = handle;
        if (abs_path[0]) {
            strncpy(slot->path, abs_path, sizeof(slot->path) - 1);
            slot->path[sizeof(slot->path) - 1] = '\0';
        } else {
            strncpy(slot->path, load_path, sizeof(slot->path) - 1);
            slot->path[sizeof(slot->path) - 1] = '\0';
        }
    }

    *out_entry = entry;
    if (out_handle) {
        *out_handle = handle;
    }
    ESP_LOGI(TAG, "loaded module binary: %s", binary_path);
    return ESP_OK;
#endif
}

void minihub_module_dl_unload_all(void)
{
#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
    for (size_t i = 0; i < s_dl_count; i++) {
        if (s_dl_slots[i].handle) {
            dlclose(s_dl_slots[i].handle);
            s_dl_slots[i].handle = NULL;
        }
        if (s_dl_slots[i].path[0]) {
            unlink(s_dl_slots[i].path);
            s_dl_slots[i].path[0] = '\0';
        }
    }
    s_dl_count = 0;
#endif
}
