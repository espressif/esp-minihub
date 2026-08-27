/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_module.h"
#include "minihub_config.h"
#include "minihub_paths.h"
#include "minihub_task.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "module_dl";

#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
#include "esp_dlfcn.h"
#if defined(CONFIG_ELF_LOADER_LOAD_PSRAM) && CONFIG_ELF_LOADER_LOAD_PSRAM
void esp_elf_arch_flush(void);
#if CONFIG_IDF_TARGET_ESP32S31
#include "soc/cache_reg.h"
#include "soc/soc.h"
#endif

/** Re-sync I/D cache after dlopen so varied PT_LOAD layouts stay executable on S31. */
static void module_dl_sync_psram_exec(void)
{
    esp_elf_arch_flush();
}

/**
 * Arm I-fetch from PSRAM heap immediately before jumping into the module.
 * On ESP32-S31, PSRAM .so text is not in the XIP-mapped firmware region; IBUS1
 * can be shut again by flash/cache traffic (esp. SPI_FLASH_AUTO_SUSPEND) after
 * the earlier flush, causing Instruction access fault at minihub_module_entry.
 */
static void module_dl_arm_psram_entry(void)
{
    esp_elf_arch_flush();
#if CONFIG_IDF_TARGET_ESP32S31
    portDISABLE_INTERRUPTS();
    REG_CLR_BIT(CACHE_L1_ICACHE_CTRL_REG, CACHE_L1_ICACHE_SHUT_IBUS1);
    portENABLE_INTERRUPTS();
#endif
}
#endif
/* QuickJS ships weak dlopen stubs; reference a symbol unique to elf_loader's
 * dlfcn.o so its strong dlopen/dlsym win at link time. */
extern void dllist(dl_list_t type);
__attribute__((used)) static void *const s_force_elf_dlfcn = (void *)&dllist;
#endif

typedef struct {
    void *handle;
    char path[160];
} dl_slot_t;

static dl_slot_t s_dl_slots[8];
static size_t s_dl_count;

#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
static SemaphoreHandle_t s_dl_mu;

typedef struct {
    const char *binary_path;
    minihub_module_entry_fn *out_entry;
    minihub_module_rpc_fn *out_rpc;
    void **out_handle;
    const minihub_module_host_api_t *host;
    const cJSON *config;
    const minihub_adapter_t **out_adapter;
    bool invoke_entry;
    esp_err_t err;
    SemaphoreHandle_t done;
} module_dl_job_t;

static esp_err_t module_dl_load_on_task(const char *binary_path,
                                        minihub_module_entry_fn *out_entry,
                                        minihub_module_rpc_fn *out_rpc,
                                        void **out_handle,
                                        const minihub_module_host_api_t *host,
                                        const cJSON *config,
                                        const minihub_adapter_t **out_adapter,
                                        bool invoke_entry);
static void module_dl_worker(void *arg);

static esp_err_t module_dl_run_job(module_dl_job_t *job)
{
    if (!s_dl_mu) {
        s_dl_mu = xSemaphoreCreateMutex();
        if (!s_dl_mu) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_dl_mu, pdMS_TO_TICKS(120000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) {
        xSemaphoreGive(s_dl_mu);
        return ESP_ERR_NO_MEM;
    }
    job->done = done;
    job->err = ESP_FAIL;

    TaskHandle_t worker = NULL;
    BaseType_t ok = minihub_task_create_internal(module_dl_worker, "mod_dl", 8192, job, 5, &worker);
    if (ok != pdPASS) {
        vSemaphoreDelete(done);
        xSemaphoreGive(s_dl_mu);
        ESP_LOGE(TAG, "cannot create internal DRAM loader task");
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(done, pdMS_TO_TICKS(120000)) != pdTRUE) {
        ESP_LOGE(TAG, "internal DRAM loader timed out");
        vSemaphoreDelete(done);
        xSemaphoreGive(s_dl_mu);
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(done);
    xSemaphoreGive(s_dl_mu);
    return job->err;
}
#endif

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
    const char *tmp_dir = minihub_paths_module_tmp_dir();
    struct stat st;
    if (stat(tmp_dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(tmp_dir, 0755) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed: errno=%d", tmp_dir, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool module_tmp_path_in_use(const char *abs_path)
{
    if (!abs_path || !abs_path[0]) {
        return false;
    }
    for (size_t i = 0; i < s_dl_count; i++) {
        if (s_dl_slots[i].path[0] && strcmp(s_dl_slots[i].path, abs_path) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Delete orphaned mod_*.so under the module tmp dir.
 * Crash / reboot never reaches dl_unload_all(), so timestamped extracts otherwise
 * accumulate and fill claw's small /fatfs.
 */
static void purge_orphan_module_tmp(void)
{
    const char *tmp_dir = minihub_paths_module_tmp_dir();
    DIR *dir = opendir(tmp_dir);
    if (!dir) {
        return;
    }

    unsigned removed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "mod_", 4) != 0) {
            continue;
        }
        size_t nlen = strlen(name);
        if (nlen < 8 || strcmp(name + nlen - 3, ".so") != 0) {
            continue;
        }

        char abs[160];
        if (snprintf(abs, sizeof(abs), "%s/%s", tmp_dir, name) >= (int)sizeof(abs)) {
            continue;
        }
        if (module_tmp_path_in_use(abs)) {
            continue;
        }
        if (unlink(abs) == 0) {
            removed++;
        }
    }
    closedir(dir);

    if (removed > 0) {
        ESP_LOGI(TAG, "purged %u orphan module tmp .so under %s", removed, tmp_dir);
    }
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
    const char *tmp_dir = minihub_paths_module_tmp_dir();
    snprintf(abs, abs_len, "%s/mod_%u.so", tmp_dir, stamp);
    /* dlopen path is relative to CONFIG_ELF_FILE_SYSTEM_BASE_PATH */
#if defined(CONFIG_ELF_FILE_SYSTEM_BASE_PATH)
    const char *base = CONFIG_ELF_FILE_SYSTEM_BASE_PATH;
    size_t base_len = strlen(base);
    if (base_len > 0 && strncmp(abs, base, base_len) == 0 && abs[base_len] == '/') {
        strncpy(rel, abs + base_len + 1, rel_len - 1);
        rel[rel_len - 1] = '\0';
        return;
    }
#endif
    snprintf(rel, rel_len, "tmp/mod_%u.so", stamp);
}

static esp_err_t resolve_load_path(const char *binary_path, char *rel_out, size_t rel_len,
                                   char *abs_out, size_t abs_len)
{
    if (path_ends_with(binary_path, ".mhub") || path_ends_with(binary_path, ".so")) {
        /* Drop leftovers from previous boots / failed loads before allocating space. */
        purge_orphan_module_tmp();
        unsigned stamp = (unsigned)(esp_log_timestamp() & 0xFFFF);
        module_mhub_tmp_paths(stamp, rel_out, rel_len, abs_out, abs_len);
        return extract_mhub_payload(binary_path, abs_out, abs_len);
    }
    abs_out[0] = '\0';
    /* Strip VFS prefix so esp_elf_open(FS_PATH/name) resolves correctly */
    const char *data_root = minihub_paths_data_root();
    size_t root_len = strlen(data_root);
    if (strncmp(binary_path, data_root, root_len) == 0 && binary_path[root_len] == '/') {
        strncpy(rel_out, binary_path + root_len + 1, rel_len - 1);
        rel_out[rel_len - 1] = '\0';
        return ESP_OK;
    }
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
    /* Always hop to an internal DRAM stack. FAT/LittleFS flash reads disable the
     * cache (esp_task_stack_is_sane_cache_disabled); claw bringup/reload often
     * run with a PSRAM stack, and S31 XIP layouts can mis-classify SP. */
    module_dl_job_t job = {
        .binary_path = binary_path,
        .out_entry = out_entry,
        .out_rpc = out_rpc,
        .out_handle = out_handle,
        .host = NULL,
        .config = NULL,
        .out_adapter = NULL,
        .invoke_entry = false,
    };
    return module_dl_run_job(&job);
#endif
}

esp_err_t minihub_module_dl_load_and_enter(const char *binary_path,
                                           const minihub_module_host_api_t *host,
                                           const cJSON *config,
                                           const minihub_adapter_t **out_adapter,
                                           minihub_module_rpc_fn *out_rpc,
                                           void **out_handle)
{
    if (!binary_path || !host || !out_adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_adapter = NULL;
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
    minihub_module_entry_fn entry_unused = NULL;
    module_dl_job_t job = {
        .binary_path = binary_path,
        .out_entry = &entry_unused,
        .out_rpc = out_rpc,
        .out_handle = out_handle,
        .host = host,
        .config = config,
        .out_adapter = out_adapter,
        .invoke_entry = true,
    };
    return module_dl_run_job(&job);
#endif
}

#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
static esp_err_t module_dl_load_on_task(const char *binary_path,
                                        minihub_module_entry_fn *out_entry,
                                        minihub_module_rpc_fn *out_rpc,
                                        void **out_handle,
                                        const minihub_module_host_api_t *host,
                                        const cJSON *config,
                                        const minihub_adapter_t **out_adapter,
                                        bool invoke_entry)
{
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

#if defined(CONFIG_ELF_LOADER_LOAD_PSRAM) && CONFIG_ELF_LOADER_LOAD_PSRAM
    module_dl_sync_psram_exec();
#endif

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

    if (s_dl_count >= 8) {
        ESP_LOGE(TAG, "too many loaded modules (max %u)", (unsigned)(sizeof(s_dl_slots) / sizeof(s_dl_slots[0])));
        dlclose(handle);
        if (abs_path[0]) {
            unlink(abs_path);
        }
        return ESP_ERR_NO_MEM;
    }

    dl_slot_t *slot = &s_dl_slots[s_dl_count++];
    slot->handle = handle;
    if (abs_path[0]) {
        strncpy(slot->path, abs_path, sizeof(slot->path) - 1);
        slot->path[sizeof(slot->path) - 1] = '\0';
    } else {
        strncpy(slot->path, load_path, sizeof(slot->path) - 1);
        slot->path[sizeof(slot->path) - 1] = '\0';
    }

    if (out_entry) {
        *out_entry = entry;
    }
    if (out_handle) {
        *out_handle = handle;
    }
    ESP_LOGI(TAG, "loaded module binary: %s", binary_path);

    if (invoke_entry) {
        ESP_LOGI(TAG, "calling module entry %p on DRAM stack", (void *)entry);
#if defined(CONFIG_ELF_LOADER_LOAD_PSRAM) && CONFIG_ELF_LOADER_LOAD_PSRAM
        /* Log first: ESP_LOGI can touch flash; arm IBUS1 immediately before jump. */
        module_dl_arm_psram_entry();
#endif
        err = entry(host, config, out_adapter);
        if (err != ESP_OK || !out_adapter || !*out_adapter) {
            ESP_LOGE(TAG, "module entry failed: %s", esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
            /* Keep handle tracked so unload_all can dlclose + unlink tmp. */
            return err != ESP_OK ? err : ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void module_dl_worker(void *arg)
{
    module_dl_job_t *job = (module_dl_job_t *)arg;
    job->err = module_dl_load_on_task(job->binary_path, job->out_entry, job->out_rpc, job->out_handle,
                                      job->host, job->config, job->out_adapter, job->invoke_entry);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}
#endif

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
    /* Also drop any crash leftovers that were never tracked in RAM. */
    purge_orphan_module_tmp();
#endif
}
