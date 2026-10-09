/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_module.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "minihub_paths.h"
#include "minihub_config.h"
#include "minihub_hub.h"
#include "minihub_adapter.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_module";
static const char *MODULE_DIR = MINIHUB_MODULES_DIR;

#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
esp_err_t minihub_module_host_symbols_register(void);
#endif

#define MAX_MODULES 12

typedef struct {
    minihub_module_info_t info;
    char manifest_path[192];
    char binary_path[256];
    char builtin_id[32];
    char adapter_version_expected[48];
    long binary_size_expected;
    const minihub_adapter_t *adapter;
    minihub_module_rpc_fn rpc;
    /** Secondary RPC for hybrid: JS primary + native fallback (or vice versa). */
    minihub_module_rpc_fn rpc_fallback;
    cJSON *ui_json;
    char ui_file_path[192]; /**< optional on-disk ui.json; reloaded for /api/modules */
    bool is_js; /**< L2 QuickJS — listed/managed here, executed by minihub_js */
    /** Hybrid package entry: 0=none, 1=native (+ JS companion), 2=js (+ native companion) */
    uint8_t hybrid_entry;
    bool auth_cap; /**< module.json capabilities contains "auth" */
} module_slot_t;

static esp_err_t reload_ui_from_file(module_slot_t *slot);

#define MAX_BUILTIN 4

typedef struct {
    char id[32];
    minihub_module_entry_fn entry;
} builtin_module_t;

static builtin_module_t s_builtins[MAX_BUILTIN];
static size_t s_builtin_count;

/** Host-static RPC (rtsp_cam / zigbee / …) survive clear_modules + reload. */
#define MAX_HOST_RPC 8
typedef struct {
    char id[32];
    minihub_module_rpc_fn rpc;
} host_rpc_reg_t;
static host_rpc_reg_t s_host_rpcs[MAX_HOST_RPC];
static size_t s_host_rpc_count;

esp_err_t minihub_module_register_builtin(const char *id, minihub_module_entry_fn entry)
{
    if (!id || !entry || s_builtin_count >= MAX_BUILTIN) {
        return ESP_ERR_INVALID_ARG;
    }
    builtin_module_t *b = &s_builtins[s_builtin_count++];
    strncpy(b->id, id, sizeof(b->id) - 1);
    b->entry = entry;
    return ESP_OK;
}

static minihub_module_entry_fn find_builtin_entry(const char *id)
{
    for (size_t i = 0; i < s_builtin_count; i++) {
        if (strcmp(s_builtins[i].id, id) == 0) {
            return s_builtins[i].entry;
        }
    }
    return NULL;
}

EXT_RAM_BSS_ATTR static module_slot_t s_modules[MAX_MODULES];
static size_t s_module_count;
static minihub_module_host_api_t s_host_api;

static module_slot_t *find_module_by_adapter(const char *adapter_name);
static bool module_binary_installed(const module_slot_t *slot);

static void clear_modules(void)
{
    for (size_t i = 0; i < s_module_count; i++) {
        if (s_modules[i].ui_json) {
            cJSON_Delete(s_modules[i].ui_json);
            s_modules[i].ui_json = NULL;
        }
    }
    memset(s_modules, 0, sizeof(s_modules));
    s_module_count = 0;
}

esp_err_t minihub_module_host_api_fill(minihub_module_host_api_t *api)
{
    if (!api) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api->struct_size < sizeof(minihub_module_host_api_t)) {
        return ESP_ERR_INVALID_VERSION;
    }

    api->abi_version = MINIHUB_MODULE_ABI_VERSION;
    api->core_api_version = MINIHUB_CORE_API_VERSION;
    api->point_register = minihub_point_register;
    api->point_unregister = minihub_point_unregister;
    api->point_get = minihub_point_get;
    api->point_set = minihub_point_set;
    api->command_register_adapter = minihub_command_register_adapter;
    api->event_publish = minihub_event_bus_publish;
    api->config_get_adapters = minihub_config_get_adapters;
    api->config_save_adapters = minihub_config_save_adapters;
    api->hub_reload = minihub_hub_reload_async;
    api->adapter_soft_reload = minihub_hub_soft_reload_adapter_async;
    api->module_read_file = minihub_module_read_file;
    api->event_subscribe = minihub_event_bus_subscribe;
    api->point_merge_params = minihub_point_merge_params;
    api->point_merge_props = minihub_point_merge_props;
    return ESP_OK;
}

static esp_err_t validate_module_info(const minihub_module_info_t *info)
{
    if (!info || info->id[0] == '\0' || info->adapter_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* ABI pinned at 1 by deliberate choice (R2): no product shipped yet, so modules
     * are rebuilt rather than version-gated. Bump MINIHUB_MODULE_ABI_VERSION on any
     * struct layout break (middle-insert/remove/reorder), not just a sizeof() change. */
    if (info->abi_version != MINIHUB_MODULE_ABI_VERSION) {
        ESP_LOGE(TAG, "module '%s' abi %u != host %u",
                 info->id, (unsigned)info->abi_version, MINIHUB_MODULE_ABI_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    if (MINIHUB_CORE_API_VERSION < info->core_api_min) {
        ESP_LOGE(TAG, "module '%s' requires core api min %u, host is %u",
                 info->id, (unsigned)info->core_api_min, MINIHUB_CORE_API_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    /* core_api_max == 0 means "no upper bound" (only core_api_min is gated). */
    if (info->core_api_max != 0 && MINIHUB_CORE_API_VERSION > info->core_api_max) {
        ESP_LOGE(TAG, "module '%s' requires core api max %u, host is %u",
                 info->id, (unsigned)info->core_api_max, MINIHUB_CORE_API_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t minihub_module_attach_adapter(
    const minihub_module_info_t *info,
    const minihub_adapter_t *adapter)
{
    esp_err_t err = validate_module_info(info);
    if (err != ESP_OK) {
        return err;
    }
    if (!adapter || !adapter->name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(adapter->name, info->adapter_name) != 0) {
        ESP_LOGE(TAG, "module '%s' adapter mismatch: info=%s adapter=%s",
                 info->id, info->adapter_name, adapter->name);
        return ESP_ERR_INVALID_ARG;
    }

    err = minihub_adapter_register(adapter);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Factory-preinstalled modules never hit Store install, so adapters.json
     * may lack this adapter. hub load_all skips unregistered keys → start()
     * never runs → Store "enabled" with zero /api/points. Create the slot
     * now (before load_all in the same hub_reload) without force-enabling an
     * existing user-disabled entry.
     */
    (void)minihub_config_ensure_adapter_slot(adapter->name, false);

    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, info->id) == 0) {
            s_modules[i].adapter = adapter;
            s_modules[i].info.loaded = true;
            ESP_LOGI(TAG, "attached module '%s' adapter '%s'", info->id, adapter->name);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/** Drop module-backed adapters from the registry before dlclose frees their .text/.rodata. */
static void detach_all_module_adapters(void)
{
    for (size_t i = 0; i < s_module_count; i++) {
        if (s_modules[i].adapter == NULL) {
            continue;
        }
        /* Reap this module's event subscriptions (their handlers live in the .so
         * we are about to dlclose) so a later point.updated dispatch can't jump
         * into freed code. Core/JS subscribers have an empty owner and survive. */
        minihub_event_bus_unsubscribe_owner(s_modules[i].info.id);
        (void)minihub_adapter_unregister(s_modules[i].adapter);
        s_modules[i].adapter = NULL;
        s_modules[i].info.loaded = false;
        s_modules[i].rpc = NULL;
    }
}

static void parent_dir(const char *path, char *out, size_t out_len)
{
    if (!path || !out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

/** Max bytes for module_read_file (devices.json / small assets). */
#define MODULE_READ_FILE_MAX  65536

static bool module_rel_name_ok(const char *rel_name)
{
    if (!rel_name || !rel_name[0] || rel_name[0] == '/' || rel_name[0] == '\\') {
        return false;
    }
    const char *p = rel_name;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\\' || p[2] == '\0')) {
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

static module_slot_t *find_module_by_adapter_or_id(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }
    module_slot_t *by_adapter = find_module_by_adapter(name);
    if (by_adapter) {
        return by_adapter;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, name) == 0) {
            return &s_modules[i];
        }
    }
    return NULL;
}

esp_err_t minihub_module_read_file(const char *adapter_name, const char *rel_name,
                                   char **out, size_t *out_len)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (!adapter_name || !adapter_name[0] || !module_rel_name_ok(rel_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    module_slot_t *slot = find_module_by_adapter_or_id(adapter_name);
    if (!slot || slot->manifest_path[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    char dir[192];
    parent_dir(slot->manifest_path, dir, sizeof(dir));
    if (dir[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    char path[256];
    if (snprintf(path, sizeof(path), "%s/%s", dir, rel_name) >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long len = ftell(f);
    if (len < 0 || len > MODULE_READ_FILE_MAX) {
        fclose(f);
        return (len > MODULE_READ_FILE_MAX) ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    if (out_len) {
        *out_len = n;
    }
    return ESP_OK;
}

/** Crash / incomplete-load guard: /store/modules/<id>/.load_guard */
#define LOAD_GUARD_NAME ".load_guard"
#define LOAD_GUARD_LOADING "loading"
#define LOAD_GUARD_OK "ok"

typedef enum {
    LOAD_GUARD_NONE = 0,
    LOAD_GUARD_STATE_LOADING,
    LOAD_GUARD_STATE_OK,
} load_guard_state_t;

static bool load_guard_path(const module_slot_t *slot, char *out, size_t out_len)
{
    if (!slot || !out || out_len == 0 || slot->manifest_path[0] == '\0') {
        return false;
    }
    char dir[192];
    parent_dir(slot->manifest_path, dir, sizeof(dir));
    if (!dir[0]) {
        return false;
    }
    return snprintf(out, out_len, "%s/%s", dir, LOAD_GUARD_NAME) < (int)out_len;
}

static load_guard_state_t load_guard_read(const module_slot_t *slot)
{
    char path[220];
    if (!load_guard_path(slot, path, sizeof(path))) {
        return LOAD_GUARD_NONE;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return LOAD_GUARD_NONE;
    }
    char buf[16] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
        buf[--n] = '\0';
    }
    if (strcmp(buf, LOAD_GUARD_LOADING) == 0) {
        return LOAD_GUARD_STATE_LOADING;
    }
    if (strcmp(buf, LOAD_GUARD_OK) == 0) {
        return LOAD_GUARD_STATE_OK;
    }
    return LOAD_GUARD_NONE;
}

static esp_err_t load_guard_write(const module_slot_t *slot, const char *state)
{
    char path[220];
    if (!load_guard_path(slot, path, sizeof(path)) || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGW(TAG, "cannot write load guard %s: errno=%d", path, errno);
        return ESP_FAIL;
    }
    fputs(state, f);
    fputc('\n', f);
    fclose(f);
    return ESP_OK;
}

static void load_guard_clear_slot(const module_slot_t *slot)
{
    char path[220];
    if (!load_guard_path(slot, path, sizeof(path))) {
        return;
    }
    unlink(path);
}

static const char *load_guard_state_str(load_guard_state_t st)
{
    switch (st) {
    case LOAD_GUARD_STATE_LOADING:
        return "loading";
    case LOAD_GUARD_STATE_OK:
        return "ok";
    default:
        return "none";
    }
}

static cJSON *load_json_file_capped(const char *path, long max_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len <= 0 || len > max_len) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static cJSON *load_ui_beside_manifest(const char *manifest_path, const char *ui_name)
{
    if (!manifest_path || !ui_name || !ui_name[0]) {
        return NULL;
    }
    char dir[192];
    parent_dir(manifest_path, dir, sizeof(dir));
    if (dir[0] == '\0') {
        return NULL;
    }
    char ui_path[256];
    if (ui_name[0] == '/') {
        strncpy(ui_path, ui_name, sizeof(ui_path) - 1);
        ui_path[sizeof(ui_path) - 1] = '\0';
    } else {
        snprintf(ui_path, sizeof(ui_path), "%s/%s", dir, ui_name);
    }
    cJSON *ui = load_json_file_capped(ui_path, 16384);
    if (ui && !cJSON_IsObject(ui)) {
        cJSON_Delete(ui);
        return NULL;
    }
    return ui;
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static esp_err_t parse_manifest(const char *path, module_slot_t *slot);

static bool try_add_manifest(const char *path)
{
    if (s_module_count >= MAX_MODULES) {
        ESP_LOGW(TAG, "module table full, skip %s", path);
        return false;
    }
    module_slot_t *slot = &s_modules[s_module_count];
    memset(slot, 0, sizeof(*slot));
    if (parse_manifest(path, slot) != ESP_OK) {
        return false;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, slot->info.id) == 0) {
            ESP_LOGW(TAG, "duplicate module id '%s', skip %s",
                     slot->info.id, path);
            if (slot->ui_json) {
                cJSON_Delete(slot->ui_json);
                slot->ui_json = NULL;
            }
            return false;
        }
    }
    s_module_count++;
    return true;
}

static esp_err_t parse_manifest(const char *path, module_slot_t *slot)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "invalid module manifest: %s", path);
        return ESP_FAIL;
    }

    minihub_module_info_t *info = &slot->info;
    info->enabled = true;
    info->loaded = false;
    info->abi_version = MINIHUB_MODULE_ABI_VERSION;
    info->core_api_min = MINIHUB_CORE_API_VERSION;
    /* 0 = unbounded upper bound; modules may omit core_api_max in module.json. */
    info->core_api_max = 0;

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *name = cJSON_GetObjectItem(root, "name");
    const cJSON *version = cJSON_GetObjectItem(root, "version");
    const cJSON *adapter = cJSON_GetObjectItem(root, "adapter");
    const cJSON *binary = cJSON_GetObjectItem(root, "binary");
    const cJSON *builtin = cJSON_GetObjectItem(root, "builtin");
    const cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    const cJSON *abi = cJSON_GetObjectItem(root, "abi_version");
    const cJSON *core_min = cJSON_GetObjectItem(root, "core_api_min");
    const cJSON *core_max = cJSON_GetObjectItem(root, "core_api_max");
    const cJSON *binary_size = cJSON_GetObjectItem(root, "binary_size");
    const cJSON *adapter_ver = cJSON_GetObjectItem(root, "adapter_version");
    const cJSON *config_only = cJSON_GetObjectItem(root, "config_only");
    const cJSON *inject = cJSON_GetObjectItem(root, "inject");
    const cJSON *runtime = cJSON_GetObjectItem(root, "runtime");
    const cJSON *capabilities = cJSON_GetObjectItem(root, "capabilities");

    slot->auth_cap = false;
    if (cJSON_IsArray(capabilities)) {
        const cJSON *cap = NULL;
        cJSON_ArrayForEach(cap, capabilities) {
            if (cJSON_IsString(cap) && strcmp(cap->valuestring, "auth") == 0) {
                slot->auth_cap = true;
                break;
            }
        }
    }

    const bool is_js = cJSON_IsString(runtime) && strcmp(runtime->valuestring, "js") == 0;
    const bool is_hybrid =
        cJSON_IsString(runtime) && strcmp(runtime->valuestring, "hybrid") == 0;
    const cJSON *entry = cJSON_GetObjectItem(root, "entry");
    slot->hybrid_entry = 0;
    if (is_hybrid) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, "native") == 0) {
            slot->hybrid_entry = 1;
        } else if (cJSON_IsString(entry) && strcmp(entry->valuestring, "js") == 0) {
            slot->hybrid_entry = 2;
        } else {
            ESP_LOGE(TAG, "hybrid module '%s' needs exactly one entry (native|js)",
                     cJSON_IsString(id) ? id->valuestring : "?");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* JSON-only / pack-shaped manifests are handled by minihub_pack, not dlopen */
    if (!is_js && !is_hybrid &&
        (cJSON_IsTrue(config_only) ||
         (cJSON_IsString(runtime) && strcmp(runtime->valuestring, "pack") == 0) ||
         (cJSON_IsObject(inject) && !cJSON_IsString(binary) && !cJSON_IsString(builtin)))) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (cJSON_IsString(id)) {
        strncpy(info->id, id->valuestring, sizeof(info->id) - 1);
    }
    if (cJSON_IsString(name)) {
        strncpy(info->name, name->valuestring, sizeof(info->name) - 1);
    }
    if (cJSON_IsString(version)) {
        strncpy(info->version, version->valuestring, sizeof(info->version) - 1);
    }
    if (info->version[0] == '\0') {
        strncpy(info->version, "0.0.0", sizeof(info->version) - 1);
    }
    if (cJSON_IsString(adapter)) {
        strncpy(info->adapter_name, adapter->valuestring, sizeof(info->adapter_name) - 1);
    }
    if (cJSON_IsBool(enabled)) {
        info->enabled = cJSON_IsTrue(enabled);
    }
    if (cJSON_IsNumber(abi)) {
        info->abi_version = (uint32_t)abi->valuedouble;
    }
    if (cJSON_IsNumber(core_min)) {
        info->core_api_min = (uint32_t)core_min->valuedouble;
    }
    if (cJSON_IsNumber(core_max)) {
        info->core_api_max = (uint32_t)core_max->valuedouble;
    }
    if (cJSON_IsNumber(binary_size)) {
        slot->binary_size_expected = (long)binary_size->valuedouble;
    }
    if (cJSON_IsString(adapter_ver)) {
        strncpy(slot->adapter_version_expected, adapter_ver->valuestring,
                sizeof(slot->adapter_version_expected) - 1);
    }

    strncpy(slot->manifest_path, path, sizeof(slot->manifest_path) - 1);

    char manifest_dir[192];
    parent_dir(path, manifest_dir, sizeof(manifest_dir));

    if (is_js) {
        /* L2: register for Store installed list; script run by minihub_js */
        slot->is_js = true;
        if (manifest_dir[0] == '\0' ||
            snprintf(slot->binary_path, sizeof(slot->binary_path), "%s/main.js",
                     manifest_dir) >= (int)sizeof(slot->binary_path)) {
            ESP_LOGE(TAG, "JS module path too long for '%s'", info->id);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        const cJSON *ui = cJSON_GetObjectItem(root, "ui");
        const cJSON *ui_file = cJSON_GetObjectItem(root, "ui_file");
        if (cJSON_IsObject(ui)) {
            slot->ui_json = cJSON_Duplicate(ui, 1);
        } else if (cJSON_IsString(ui)) {
            slot->ui_json = load_ui_beside_manifest(path, ui->valuestring);
        } else if (cJSON_IsString(ui_file)) {
            slot->ui_json = load_ui_beside_manifest(path, ui_file->valuestring);
        } else {
            slot->ui_json = load_ui_beside_manifest(path, "ui.json");
        }
        cJSON_Delete(root);
        ESP_LOGI(TAG, "manifest '%s' -> JS module %s", path, info->id);
        return ESP_OK;
    }

    if (cJSON_IsString(binary)) {
        const char *bin = binary->valuestring;
        if (bin[0] == '/') {
            strncpy(slot->binary_path, bin, sizeof(slot->binary_path) - 1);
            slot->binary_path[sizeof(slot->binary_path) - 1] = '\0';
        } else if (manifest_dir[0]) {
            if (strlen(manifest_dir) + 1 + strlen(bin) >= sizeof(slot->binary_path)) {
                ESP_LOGE(TAG, "binary path too long for module '%s'", info->id);
                cJSON_Delete(root);
                return ESP_ERR_INVALID_SIZE;
            }
            snprintf(slot->binary_path, sizeof(slot->binary_path), "%s/%s", manifest_dir, bin);
        } else {
            snprintf(slot->binary_path, sizeof(slot->binary_path),
                     "%s/%s", MINIHUB_MODULES_BIN_DIR, bin);
        }
    }
    if (cJSON_IsString(builtin)) {
        strncpy(slot->builtin_id, builtin->valuestring, sizeof(slot->builtin_id) - 1);
    }

    const cJSON *ui = cJSON_GetObjectItem(root, "ui");
    const cJSON *ui_file = cJSON_GetObjectItem(root, "ui_file");
    if (cJSON_IsObject(ui)) {
        slot->ui_json = cJSON_Duplicate(ui, 1);
    } else if (cJSON_IsString(ui)) {
        slot->ui_json = load_ui_beside_manifest(path, ui->valuestring);
    } else if (cJSON_IsString(ui_file)) {
        slot->ui_json = load_ui_beside_manifest(path, ui_file->valuestring);
    } else {
        slot->ui_json = load_ui_beside_manifest(path, "ui.json");
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "manifest '%s' -> module %s (adapter %s)",
             path, info->id, info->adapter_name);
    return ESP_OK;
}

static esp_err_t scan_manifests(void)
{
    DIR *dir = opendir(MODULE_DIR);
    if (!dir) {
        ESP_LOGD(TAG, "module dir %s not found, skip", MODULE_DIR);
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        size_t name_len = strlen(ent->d_name);
        /* Module package ids are short; reject wild filesystem names early. */
        if (name_len == 0 || name_len > 48) {
            continue;
        }
        char name[49];
        memcpy(name, ent->d_name, name_len);
        name[name_len] = '\0';

        char path[80];
        /* "/store/modules" + '/' + name(<=48) fits in 80 */
        snprintf(path, sizeof(path), "%s/%s", MODULE_DIR, name);
        if (path_is_dir(path)) {
            /* Preferred: /store/modules/<id>/module.json */
            char nested[96];
            snprintf(nested, sizeof(nested), "%s/module.json", path);
            (void)try_add_manifest(nested);
            continue;
        }
        /* Legacy flat: /store/modules/<id>.json */
        if (name_len < 6 || strcmp(name + name_len - 5, ".json") != 0) {
            continue;
        }
        if (name_len >= 13 && strcmp(name + name_len - 13, ".example.json") == 0) {
            continue;
        }
        (void)try_add_manifest(path);
    }
    closedir(dir);
    return ESP_OK;
}

static const cJSON *adapter_config_for(const char *adapter_name, const cJSON *adapters_root)
{
    if (!adapters_root || !adapter_name) {
        return NULL;
    }
    const cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    if (!adapters) {
        return NULL;
    }
    return cJSON_GetObjectItem(adapters, adapter_name);
}

static esp_err_t load_builtin_module(module_slot_t *slot, const cJSON *adapter_cfg)
{
    if (slot->builtin_id[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    minihub_module_entry_fn entry = find_builtin_entry(slot->builtin_id);
    if (!entry) {
        ESP_LOGW(TAG, "no builtin entry for '%s'", slot->builtin_id);
        return ESP_ERR_NOT_FOUND;
    }

    const minihub_adapter_t *adapter = NULL;
    minihub_event_bus_set_current_owner(slot->info.id);
    esp_err_t err = entry(&s_host_api, adapter_cfg, &adapter);
    minihub_event_bus_set_current_owner("");
    if (err != ESP_OK || !adapter) {
        ESP_LOGE(TAG, "builtin '%s' entry failed: %s", slot->builtin_id, esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = minihub_module_attach_adapter(&slot->info, adapter);
    if (err == ESP_OK) {
        slot->adapter = adapter;
        slot->info.loaded = true;
    }
    return err;
}

static esp_err_t validate_mhub_header(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    minihub_module_image_header_t hdr;
    size_t n = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (n != sizeof(hdr) || hdr.magic != MINIHUB_MODULE_MAGIC) {
        return ESP_ERR_INVALID_CRC;
    }
    if (hdr.abi_version != MINIHUB_MODULE_ABI_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

static esp_err_t try_load_binary(module_slot_t *slot, const cJSON *adapter_cfg)
{
    if (slot->builtin_id[0] == '\0' && find_builtin_entry(slot->info.id) != NULL) {
        strncpy(slot->builtin_id, slot->info.id, sizeof(slot->builtin_id) - 1);
    }
    if (slot->builtin_id[0] != '\0') {
        esp_err_t berr = load_builtin_module(slot, adapter_cfg);
        if (berr == ESP_OK) {
            load_guard_clear_slot(slot);
        }
        return berr;
    }

    if (slot->is_js) {
        /* Execution is minihub_js_reload(); just mark presence for UI / Store. */
        struct stat st;
        if (slot->binary_path[0] && stat(slot->binary_path, &st) == 0 && S_ISREG(st.st_mode)) {
            slot->info.loaded = slot->info.enabled;
            /* Heal installs that predate L2 adapters.json slot creation. */
            if (slot->info.enabled && slot->info.adapter_name[0]) {
                (void)minihub_config_ensure_adapter_slot(slot->info.adapter_name, false);
            }
            ESP_LOGI(TAG, "JS module '%s' ready (script %ld bytes)",
                     slot->info.id, (long)st.st_size);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "JS module '%s' missing main.js", slot->info.id);
        return ESP_ERR_NOT_FOUND;
    }

    if (slot->binary_path[0] == '\0') {
        ESP_LOGW(TAG, "module '%s' has no binary or builtin", slot->info.id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Host-linked adapter wins over an obsolete .mhub (e.g. zigbee host stack). */
    if (minihub_adapter_find(slot->info.adapter_name) != NULL) {
        ESP_LOGW(TAG, "skip module '%s' — adapter '%s' already registered",
                 slot->info.id, slot->info.adapter_name);
        return ESP_OK;
    }

#if CONFIG_MINIHUB_DYNAMIC_MODULES
    size_t blen = strlen(slot->binary_path);
    bool needs_mhmd = (blen >= 5 && strcmp(slot->binary_path + blen - 5, ".mhub") == 0) ||
                      (blen >= 3 && strcmp(slot->binary_path + blen - 3, ".so") == 0);
    if (needs_mhmd) {
        esp_err_t err = validate_mhub_header(slot->binary_path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "invalid MHMD header: %s (%s)", slot->binary_path, esp_err_to_name(err));
            return err;
        }
    }

    /* Persist "loading" before dlopen/entry so a crash leaves a sticky skip flag. */
    (void)load_guard_write(slot, LOAD_GUARD_LOADING);

    minihub_module_rpc_fn rpc = NULL;
    void *handle = NULL;
    const minihub_adapter_t *adapter = NULL;
    esp_err_t err = minihub_module_dl_load_and_enter(slot->binary_path, &s_host_api, adapter_cfg,
                                                     &adapter, &rpc, &handle, slot->info.id);
    if (err != ESP_OK || !adapter) {
        /* Soft failure: clear guard so a later retry (disk free, new package) can proceed. */
        load_guard_clear_slot(slot);
        ESP_LOGE(TAG, "module '%s' entry/load failed: %s", slot->info.id,
                 esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    if (rpc) {
        /* Hybrid entry=js: JS may already own primary RPC — keep it, stash native. */
        if (slot->rpc && slot->rpc != rpc) {
            slot->rpc_fallback = rpc;
        } else {
            slot->rpc = rpc;
        }
    }

    err = minihub_module_attach_adapter(&slot->info, adapter);
    if (err == ESP_OK) {
        slot->adapter = adapter;
        slot->info.loaded = true;
        (void)load_guard_write(slot, LOAD_GUARD_OK);
        struct stat st;
        if (stat(slot->binary_path, &st) == 0) {
            ESP_LOGI(TAG, "module '%s' loaded binary %ld bytes adapter %s",
                     slot->info.id, (long)st.st_size,
                     adapter->version ? adapter->version : "?");
            if (slot->binary_size_expected > 0 &&
                (long)st.st_size != slot->binary_size_expected) {
                ESP_LOGW(TAG,
                         "module '%s' binary size %ld != manifest %ld (store upgrade or re-flash modstore)",
                         slot->info.id, (long)st.st_size, slot->binary_size_expected);
            }
            if (slot->adapter_version_expected[0] && adapter->version &&
                strcmp(slot->adapter_version_expected, adapter->version) != 0) {
                ESP_LOGW(TAG,
                         "module '%s' adapter %s != manifest %s (binary/manifest out of sync)",
                         slot->info.id, adapter->version, slot->adapter_version_expected);
            }
        }
    } else {
        load_guard_clear_slot(slot);
    }
    (void)handle;
    return err;
#else
    ESP_LOGI(TAG, "module '%s' binary '%s' skipped (CONFIG_MINIHUB_DYNAMIC_MODULES=n)",
             slot->info.id, slot->binary_path);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_module_loader_init(void)
{
    clear_modules();
    memset(&s_host_api, 0, sizeof(s_host_api));
    s_host_api.struct_size = sizeof(minihub_module_host_api_t);
    esp_err_t err = minihub_module_host_api_fill(&s_host_api);
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_MINIHUB_DYNAMIC_MODULES && defined(CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT) && CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
    err = minihub_module_host_symbols_register();
    if (err != ESP_OK) {
        return err;
    }
#endif
    return scan_manifests();
}

size_t minihub_module_count(void)
{
    return s_module_count;
}

size_t minihub_module_loaded_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_module_count; i++) {
        if (s_modules[i].info.loaded) {
            n++;
        }
    }
    return n;
}

static bool module_binary_installed(const module_slot_t *slot)
{
    if (!slot || slot->binary_path[0] == '\0') {
        return false;
    }
    struct stat st;
    return stat(slot->binary_path, &st) == 0;
}

static bool module_slot_installed(const module_slot_t *slot)
{
    if (!slot) {
        return false;
    }
    if (slot->manifest_path[0] != '\0') {
        return slot->builtin_id[0] != '\0' || module_binary_installed(slot);
    }
    /* Host RPC listing without a Store package (zigbee / rtsp_cam). */
    return slot->rpc != NULL;
}

/** Radio host modules need the matching Kconfig SDK; hide them when not compiled in. */
static bool module_radio_host_firmware_enabled(const char *module_id)
{
    if (!module_id || module_id[0] == '\0') {
        return true;
    }
    if (strcmp(module_id, "zigbee") == 0) {
#if CONFIG_MINIHUB_ZIGBEE_SDK
        return true;
#else
        return false;
#endif
    }
    if (strcmp(module_id, "ble_mesh") == 0) {
#if CONFIG_MINIHUB_BLE_MESH_SDK
        return true;
#else
        return false;
#endif
    }
    if (strcmp(module_id, "matter") == 0) {
#if CONFIG_MINIHUB_MATTER_ADAPTER
        return true;
#else
        return false;
#endif
    }
    if (strcmp(module_id, "thread") == 0) {
#if CONFIG_MINIHUB_THREAD_ADAPTER
        return true;
#else
        return false;
#endif
    }
    return true;
}

static void remove_ephemeral_module_slot(const char *module_id)
{
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, module_id) != 0) {
            continue;
        }
        if (s_modules[i].manifest_path[0] != '\0') {
            return;
        }
        if (s_modules[i].ui_json) {
            cJSON_Delete(s_modules[i].ui_json);
            s_modules[i].ui_json = NULL;
        }
        if (i + 1 < s_module_count) {
            memmove(&s_modules[i], &s_modules[i + 1],
                    (s_module_count - i - 1) * sizeof(s_modules[i]));
        }
        s_module_count--;
        return;
    }
}

cJSON *minihub_module_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_module_count; i++) {
        const minihub_module_info_t *info = &s_modules[i].info;
        if (!module_radio_host_firmware_enabled(info->id)) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", info->id);
        cJSON_AddStringToObject(item, "name", info->name);
        cJSON_AddStringToObject(item, "version", info->version[0] ? info->version : "0.0.0");
        cJSON_AddStringToObject(item, "adapter", info->adapter_name);
        const minihub_adapter_t *runtime = s_modules[i].adapter;
        if (!runtime) {
            runtime = minihub_adapter_find(info->adapter_name);
        }
        if (runtime && runtime->version && runtime->version[0]) {
            cJSON_AddStringToObject(item, "adapter_version", runtime->version);
        }
        if (s_modules[i].adapter_version_expected[0]) {
            cJSON_AddStringToObject(item, "adapter_version_expected",
                                    s_modules[i].adapter_version_expected);
        }
        if (s_modules[i].binary_size_expected > 0) {
            cJSON_AddNumberToObject(item, "binary_size_expected",
                                    (double)s_modules[i].binary_size_expected);
        }
        if (s_modules[i].binary_path[0]) {
            struct stat st;
            if (stat(s_modules[i].binary_path, &st) == 0) {
                cJSON_AddNumberToObject(item, "binary_size", (double)st.st_size);
                bool stale = s_modules[i].binary_size_expected > 0 &&
                               (long)st.st_size != s_modules[i].binary_size_expected;
                cJSON_AddBoolToObject(item, "binary_stale", stale);
            }
        }
        cJSON_AddNumberToObject(item, "abi_version", info->abi_version);
        cJSON_AddBoolToObject(item, "enabled", info->enabled);
        cJSON_AddBoolToObject(item, "loaded", info->loaded);
        {
            load_guard_state_t gst = load_guard_read(&s_modules[i]);
            cJSON_AddStringToObject(item, "load_guard", load_guard_state_str(gst));
            cJSON_AddBoolToObject(item, "load_blocked", gst == LOAD_GUARD_STATE_LOADING);
        }
        if (s_modules[i].is_js) {
            cJSON_AddStringToObject(item, "runtime", "js");
        } else if (s_modules[i].hybrid_entry) {
            cJSON_AddStringToObject(item, "runtime", "hybrid");
            cJSON_AddStringToObject(item, "entry",
                                   s_modules[i].hybrid_entry == 2 ? "js" : "native");
        } else if (s_modules[i].manifest_path[0]) {
            char js_path[256];
            char dir[192];
            parent_dir(s_modules[i].manifest_path, dir, sizeof(dir));
            if (dir[0] &&
                snprintf(js_path, sizeof(js_path), "%s/main.js", dir) < (int)sizeof(js_path)) {
                struct stat st_js;
                if (stat(js_path, &st_js) == 0 && S_ISREG(st_js.st_mode)) {
                    cJSON_AddStringToObject(item, "runtime", "hybrid");
                } else {
                    cJSON_AddStringToObject(item, "runtime", "native");
                }
            }
        }
#if CONFIG_MINIHUB_DYNAMIC_MODULES
        cJSON_AddBoolToObject(item, "loader_enabled", true);
#else
        cJSON_AddBoolToObject(item, "loader_enabled", false);
#endif
        if (s_modules[i].binary_path[0]) {
            cJSON_AddStringToObject(item, "binary", s_modules[i].binary_path);
        }
        /* Store-managed = has a package under /store/modules. Preferring a Host
         * builtin entry for load must not hide Disable/Delete (factory demos). */
        bool managed = s_modules[i].manifest_path[0] != '\0';
        cJSON_AddBoolToObject(item, "managed", managed);
        bool installed = module_slot_installed(&s_modules[i]);
        cJSON_AddBoolToObject(item, "installed", installed);
        cJSON_AddBoolToObject(item, "has_rpc", s_modules[i].rpc != NULL);
        if (s_modules[i].ui_file_path[0]) {
            (void)reload_ui_from_file(&s_modules[i]);
        }
        if (s_modules[i].ui_json) {
            cJSON_AddItemToObject(item, "ui", cJSON_Duplicate(s_modules[i].ui_json, 1));
        }
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}

static bool adapter_config_enabled(const char *adapter_name)
{
    cJSON *cfg = minihub_config_get_adapters();
    if (!cfg) {
        return false;
    }
    const cJSON *adapters = cJSON_GetObjectItem(cfg, "adapters");
    if (!cJSON_IsObject(adapters)) {
        return false;
    }
    const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter_name);
    if (!adapter_cfg) {
        return false;
    }
    const cJSON *enabled = cJSON_GetObjectItem(adapter_cfg, "enabled");
    return !cJSON_IsBool(enabled) || cJSON_IsTrue(enabled);
}

bool minihub_module_overview_adapter_visible(const char *adapter_name)
{
    if (!adapter_name || adapter_name[0] == '\0') {
        return false;
    }
    if (!module_radio_host_firmware_enabled(adapter_name)) {
        return false;
    }

#if CONFIG_MINIHUB_DYNAMIC_MODULES
    bool managed = false;
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.adapter_name, adapter_name) != 0) {
            continue;
        }
        managed = true;
        if (!s_modules[i].info.enabled) {
            return false;
        }
        bool installed = module_slot_installed(&s_modules[i]);
        if (!installed) {
            return false;
        }
        /* L2 JS: no C adapter registry. Missing adapters.json slot used to hide
         * points (Store only ensured slots for native). Treat absent as enabled. */
        if (s_modules[i].is_js) {
            cJSON *cfg = minihub_config_get_adapters();
            const cJSON *adapters = cfg ? cJSON_GetObjectItem(cfg, "adapters") : NULL;
            const cJSON *adapter_cfg =
                cJSON_IsObject(adapters) ? cJSON_GetObjectItem(adapters, adapter_name) : NULL;
            if (!adapter_cfg) {
                return true;
            }
            return adapter_config_enabled(adapter_name);
        }
        if (minihub_adapter_find(adapter_name) != NULL) {
            return adapter_config_enabled(adapter_name);
        }
        /* Host-static RPC (rtsp_cam etc.): no registry adapter / adapters.json entry. */
        if (s_modules[i].rpc != NULL) {
            return adapter_config_enabled(adapter_name);
        }
        return adapter_config_enabled(adapter_name);
    }
    if (managed) {
        return false;
    }
#endif

    if (!minihub_adapter_find(adapter_name)) {
        return false;
    }
    return adapter_config_enabled(adapter_name);
}

bool minihub_module_adapter_has_auth(const char *adapter_name)
{
    module_slot_t *slot = find_module_by_adapter(adapter_name);
    if (!slot) {
        return false;
    }
    if (slot->auth_cap) {
        return true;
    }
    if (slot->ui_json) {
        const cJSON *auth = cJSON_GetObjectItem(slot->ui_json, "auth");
        if (cJSON_IsTrue(auth)) {
            return true;
        }
    }
    return false;
}

bool minihub_module_pick_auth_adapter(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    for (size_t i = 0; i < s_module_count; i++) {
        const char *adapter = s_modules[i].info.adapter_name;
        if (!adapter[0]) {
            continue;
        }
        if (!minihub_module_overview_adapter_visible(adapter)) {
            continue;
        }
        if (!minihub_module_adapter_has_auth(adapter)) {
            continue;
        }
        strncpy(out, adapter, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    return false;
}

bool minihub_module_ui_label(const char *adapter_name, const char *key,
                             char *out, size_t out_len)
{
    if (!adapter_name || !key || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    module_slot_t *slot = find_module_by_adapter(adapter_name);
    if (!slot || !slot->ui_json) {
        return false;
    }
    const cJSON *labels = cJSON_GetObjectItem(slot->ui_json, "labels");
    const cJSON *item = cJSON_IsObject(labels) ? cJSON_GetObjectItem(labels, key)
                                               : cJSON_GetObjectItem(slot->ui_json, key);
    if (!cJSON_IsString(item) || !item->valuestring[0]) {
        return false;
    }
    strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

static module_slot_t *find_module_by_adapter(const char *adapter_name)
{
    if (!adapter_name) {
        return NULL;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.adapter_name, adapter_name) == 0) {
            return &s_modules[i];
        }
    }
    return NULL;
}

#define OVERVIEW_ADAPTER_CAP 24

static bool overview_name_seen(const char *const *names, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void overview_add_name(const char **names, size_t *n, const char *name)
{
    if (!name || !name[0] || *n >= OVERVIEW_ADAPTER_CAP) {
        return;
    }
    if (overview_name_seen(names, *n, name)) {
        return;
    }
    names[(*n)++] = name;
}

cJSON *minihub_module_overview_adapters_json(void)
{
    const char *names[OVERVIEW_ADAPTER_CAP];
    size_t n = 0;

    /* Reads the live module/adapter/config trees; must not race hub_reload,
     * which frees and replaces s_adapters_cfg (minihub_config_reload) and tears
     * down s_modules / the adapter registry under this same lock. Without it a
     * concurrent reload can swap the config tree mid-iteration and crash us in
     * cJSON_GetObjectItem (freed child->string). */
    if (minihub_adapter_lifecycle_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "overview adapters: lifecycle lock timeout");
        return cJSON_CreateObject();
    }

    /* adapters.json keys (show chips even when .mhub failed to dlopen). */
    cJSON *cfg = minihub_config_get_adapters();
    if (cfg) {
        const cJSON *adapters = cJSON_GetObjectItem(cfg, "adapters");
        if (cJSON_IsObject(adapters)) {
            for (cJSON *child = adapters->child; child != NULL; child = child->next) {
                overview_add_name(names, &n, child->string);
            }
        }
    }

    for (size_t i = 0; i < s_module_count; i++) {
        overview_add_name(names, &n, s_modules[i].info.adapter_name);
    }

    size_t reg = minihub_adapter_count();
    for (size_t i = 0; i < reg; i++) {
        const minihub_adapter_t *adapter = minihub_adapter_get(i);
        if (adapter) {
            overview_add_name(names, &n, adapter->name);
        }
    }

    cJSON *root = cJSON_CreateObject();
    for (size_t i = 0; i < n; i++) {
        const char *name = names[i];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddBoolToObject(entry, "enabled", minihub_module_overview_adapter_visible(name));
        cJSON_AddBoolToObject(entry, "auth", minihub_module_adapter_has_auth(name));

        module_slot_t *slot = find_module_by_adapter(name);
        if (slot && slot->info.version[0]) {
            cJSON_AddStringToObject(entry, "module_id", slot->info.id);
            cJSON_AddStringToObject(entry, "module_name", slot->info.name);
            cJSON_AddStringToObject(entry, "module_version", slot->info.version);
        }

        const minihub_adapter_t *adapter = minihub_adapter_find(name);
        if (adapter && adapter->version && adapter->version[0]) {
            cJSON_AddStringToObject(entry, "adapter_version", adapter->version);
        }

        cJSON_AddItemToObject(root, name, entry);
    }
    minihub_adapter_lifecycle_unlock();
    return root;
}

static module_slot_t *find_module(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, id) == 0) {
            return &s_modules[i];
        }
    }
    return NULL;
}

esp_err_t minihub_module_register_rpc(const char *module_id, minihub_module_rpc_fn rpc)
{
    if (!module_id || !module_id[0] || !rpc) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!module_radio_host_firmware_enabled(module_id)) {
        ESP_LOGI(TAG, "skip host RPC '%s': radio SDK not enabled in firmware", module_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Remember so hub reload / clear_modules can re-bind. */
    host_rpc_reg_t *reg = NULL;
    for (size_t i = 0; i < s_host_rpc_count; i++) {
        if (strcmp(s_host_rpcs[i].id, module_id) == 0) {
            reg = &s_host_rpcs[i];
            break;
        }
    }
    if (!reg) {
        if (s_host_rpc_count >= MAX_HOST_RPC) {
            ESP_LOGW(TAG, "cannot register RPC '%s': host RPC table full", module_id);
            return ESP_ERR_NO_MEM;
        }
        reg = &s_host_rpcs[s_host_rpc_count++];
        memset(reg, 0, sizeof(*reg));
        strncpy(reg->id, module_id, sizeof(reg->id) - 1);
    }
    reg->rpc = rpc;

    module_slot_t *slot = find_module(module_id);
    if (!slot) {
        /* Host-static RPC (e.g. rtsp_cam) — create a lightweight slot. */
        if (s_module_count >= MAX_MODULES) {
            ESP_LOGW(TAG, "cannot register RPC '%s': module table full", module_id);
            return ESP_ERR_NO_MEM;
        }
        slot = &s_modules[s_module_count++];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->info.id, module_id, sizeof(slot->info.id) - 1);
        strncpy(slot->info.name, module_id, sizeof(slot->info.name) - 1);
        strncpy(slot->info.version, "1.0.0", sizeof(slot->info.version) - 1);
        strncpy(slot->info.adapter_name, module_id, sizeof(slot->info.adapter_name) - 1);
        slot->info.enabled = true;
        slot->info.loaded = true;
        slot->info.abi_version = MINIHUB_MODULE_ABI_VERSION;
        slot->info.core_api_min = MINIHUB_CORE_API_VERSION;
        slot->info.core_api_max = MINIHUB_CORE_API_VERSION;
        ESP_LOGI(TAG, "host RPC module slot created: %s", module_id);
    }
    /* Hybrid: JS registerRpc becomes primary; demote prior native RPC to fallback. */
    if (slot->rpc && slot->rpc != rpc) {
        slot->rpc_fallback = slot->rpc;
    }
    slot->rpc = rpc;
    slot->info.loaded = true;
    return ESP_OK;
}

esp_err_t minihub_module_forget_host_rpc(const char *module_id)
{
    if (!module_id || !module_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    bool found = false;
    for (size_t i = 0; i < s_host_rpc_count; i++) {
        if (strcmp(s_host_rpcs[i].id, module_id) != 0) {
            continue;
        }
        if (i + 1 < s_host_rpc_count) {
            memmove(&s_host_rpcs[i], &s_host_rpcs[i + 1],
                    (s_host_rpc_count - i - 1) * sizeof(s_host_rpcs[i]));
        }
        s_host_rpc_count--;
        found = true;
        break;
    }

    module_slot_t *slot = find_module(module_id);
    if (slot) {
        if (slot->rpc_fallback) {
            slot->rpc = slot->rpc_fallback;
            slot->rpc_fallback = NULL;
            found = true;
        }
        if (slot->manifest_path[0] == '\0') {
            remove_ephemeral_module_slot(module_id);
            found = true;
        }
    }

    if (found) {
        ESP_LOGI(TAG, "forgot host RPC module '%s'", module_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_module_set_ui_json(const char *module_id, const char *json)
{
    if (!module_id || !module_id[0] || !json || !json[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    module_slot_t *slot = find_module(module_id);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *parsed = cJSON_Parse(json);
    if (!parsed || !cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    if (slot->ui_json) {
        cJSON_Delete(slot->ui_json);
    }
    slot->ui_json = parsed;
    slot->ui_file_path[0] = '\0';
    return ESP_OK;
}

static esp_err_t reload_ui_from_file(module_slot_t *slot)
{
    if (!slot || slot->ui_file_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *parsed = load_json_file_capped(slot->ui_file_path, 16384);
    if (!parsed || !cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->ui_json) {
        cJSON_Delete(slot->ui_json);
    }
    slot->ui_json = parsed;
    return ESP_OK;
}

esp_err_t minihub_module_set_ui_file(const char *module_id, const char *path)
{
    if (!module_id || !module_id[0] || !path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    module_slot_t *slot = find_module(module_id);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *parsed = load_json_file_capped(path, 16384);
    if (!parsed || !cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->ui_json) {
        cJSON_Delete(slot->ui_json);
    }
    slot->ui_json = parsed;
    strncpy(slot->ui_file_path, path, sizeof(slot->ui_file_path) - 1);
    slot->ui_file_path[sizeof(slot->ui_file_path) - 1] = '\0';
    return ESP_OK;
}

static void reapply_host_rpcs(void)
{
    for (size_t i = 0; i < s_host_rpc_count; i++) {
        const host_rpc_reg_t *reg = &s_host_rpcs[i];
        if (!reg->id[0] || !reg->rpc) {
            continue;
        }
        esp_err_t err = minihub_module_register_rpc(reg->id, reg->rpc);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reapply host RPC '%s' failed: %s", reg->id, esp_err_to_name(err));
        }
    }
}

esp_err_t minihub_module_invoke_rpc(const char *module_id, const char *method,
                                    const cJSON *params, cJSON **out)
{
    if (!module_id || !method || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    module_slot_t *slot = find_module(module_id);
    if (!slot) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Re-check and dereference the module's rpc function under the adapter
     * lifecycle lock so a concurrent reload (detach -> dlclose) cannot free
     * the module code between the check and the call. */
    if (minihub_adapter_lifecycle_lock(1000) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!slot->info.loaded || !slot->rpc) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = slot->rpc(method, params, out);
    if (err == ESP_ERR_NOT_FOUND && slot->rpc_fallback) {
        err = slot->rpc_fallback(method, params, out);
    }
    minihub_adapter_lifecycle_unlock();
    return err;
}

static esp_err_t write_manifest_enabled(const char *path, bool enabled)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    if (enabled_item) {
        cJSON_ReplaceItemInObject(root, "enabled", cJSON_CreateBool(enabled));
    } else {
        cJSON_AddBoolToObject(root, "enabled", enabled);
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    f = fopen(path, "w");
    if (!f) {
        free(json);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    free(json);
    return ESP_OK;
}

esp_err_t minihub_module_set_enabled(const char *id, bool enabled)
{
    module_slot_t *slot = find_module(id);
    /* Pure Host listings without a Store package cannot be toggled. */
    if (!slot || slot->manifest_path[0] == '\0') {
        return slot ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = write_manifest_enabled(slot->manifest_path, enabled);
    if (err != ESP_OK) {
        return err;
    }
    slot->info.enabled = enabled;
    /* Enabling again is an explicit retry — drop a sticky crash guard. */
    if (enabled) {
        load_guard_clear_slot(slot);
        /* Re-enable should also revive a missing/disabled adapters.json slot. */
        if (slot->info.adapter_name[0]) {
            (void)minihub_config_ensure_adapter_slot(slot->info.adapter_name, true);
        }
    }
    ESP_LOGI(TAG, "module '%s' enabled=%d", id, enabled);
    return ESP_OK;
}

esp_err_t minihub_module_clear_load_guard(const char *id)
{
    module_slot_t *slot = find_module(id);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    load_guard_clear_slot(slot);
    ESP_LOGI(TAG, "module '%s' load guard cleared", id);
    return ESP_OK;
}

esp_err_t minihub_module_delete(const char *id)
{
    module_slot_t *slot = find_module(id);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }

    if (slot->manifest_path[0] == '\0') {
        char adapter[sizeof(slot->info.adapter_name)];
        strncpy(adapter, slot->info.adapter_name, sizeof(adapter) - 1);
        adapter[sizeof(adapter) - 1] = '\0';
        (void)minihub_module_forget_host_rpc(id);
        (void)minihub_config_remove_adapter(adapter);
        ESP_LOGI(TAG, "removed orphan module listing '%s'", id);
        return ESP_OK;
    }

    /* Host-linked demos may set builtin_id for preferred load; Store package
     * delete is still allowed (next boot skips without the manifest). */
    if (!module_binary_installed(slot) && slot->builtin_id[0] == '\0' &&
        find_builtin_entry(slot->info.id) == NULL) {
        ESP_LOGW(TAG, "module '%s' not installed (binary missing)", id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    /*
     * Do not clear the module table here. Web/store callers follow with
     * minihub_hub_reload(), which stop_all → detach_all → dl_unload_all.
     * Calling loader_init() first orphans registered adapters (vtable in .so)
     * so detach cannot unregister them, and the next start/init jumps into
     * freed module memory (Illegal instruction).
     */
    if (slot->binary_path[0]) {
        unlink(slot->binary_path);
    }
    /* Sibling ui.json (per-module directory layout) */
    char dir[192];
    parent_dir(slot->manifest_path, dir, sizeof(dir));
    if (dir[0]) {
        char ui_path[256];
        snprintf(ui_path, sizeof(ui_path), "%s/ui.json", dir);
        unlink(ui_path);
        char guard_path[256];
        snprintf(guard_path, sizeof(guard_path), "%s/%s", dir, LOAD_GUARD_NAME);
        unlink(guard_path);
    }
    if (unlink(slot->manifest_path) != 0) {
        ESP_LOGE(TAG, "cannot delete %s", slot->manifest_path);
        return ESP_FAIL;
    }
    if (dir[0] && strcmp(dir, MINIHUB_MODULES_DIR) != 0) {
        (void)rmdir(dir);
    }
    (void)minihub_module_forget_host_rpc(id);
    (void)minihub_config_remove_adapter(slot->info.adapter_name);
    ESP_LOGI(TAG, "deleted module '%s'", id);
    return ESP_OK;
}

esp_err_t minihub_module_load_binary(const char *manifest_path)
{
    if (!manifest_path) {
        return ESP_ERR_INVALID_ARG;
    }

    module_slot_t slot = {0};
    esp_err_t err = parse_manifest(manifest_path, &slot);
    if (err != ESP_OK) {
        return err;
    }
    if (!slot.info.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    return try_load_binary(&slot, NULL);
}

esp_err_t minihub_module_load_all(const cJSON *adapters_root)
{
    ESP_LOGI(TAG, "load_all: %u module(s) to consider", (unsigned)s_module_count);
    for (size_t i = 0; i < s_module_count; i++) {
        module_slot_t *slot = &s_modules[i];
        if (!slot->info.enabled) {
            ESP_LOGI(TAG, "skip '%s' (disabled)", slot->info.id);
            continue;
        }
        if (slot->info.loaded) {
            ESP_LOGI(TAG, "skip '%s' (already loaded)", slot->info.id);
            continue;
        }

        esp_err_t err = validate_module_info(&slot->info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skip '%s' (validate: %s)", slot->info.id, esp_err_to_name(err));
            continue;
        }

        /* Host-linked demo / factory modules: prefer builtin over .so dlopen. */
        if (slot->builtin_id[0] == '\0' && find_builtin_entry(slot->info.id) != NULL) {
            strncpy(slot->builtin_id, slot->info.id, sizeof(slot->builtin_id) - 1);
            ESP_LOGI(TAG, "module '%s' uses Host builtin entry", slot->info.id);
        }

        /* Native packages only: sticky "loading" means last boot crashed mid-entry.
         * Skip the guard when a Host builtin can load instead of the .so. */
        if (!slot->is_js && slot->builtin_id[0] == '\0' &&
            load_guard_read(slot) == LOAD_GUARD_STATE_LOADING) {
            ESP_LOGW(TAG,
                     "skip '%s' — load guard stuck in loading (previous crash?). "
                     "Clear via POST /api/modules {\"id\":\"%s\",\"clear_load_guard\":true} "
                     "or enable=true, then reload.",
                     slot->info.id, slot->info.id);
            continue;
        }

        ESP_LOGI(TAG, "loading '%s' binary='%s'", slot->info.id,
                 slot->binary_path[0] ? slot->binary_path : "(none)");
        if (slot->hybrid_entry == 2) {
            /* entry=js: JS setup runs first (minihub_js_reload); .so companion after. */
            ESP_LOGI(TAG, "defer '%s' .so until after JS entry", slot->info.id);
            continue;
        }
        const cJSON *adapter_cfg = adapter_config_for(slot->info.adapter_name, adapters_root);
        err = try_load_binary(slot, adapter_cfg);
        if (err == ESP_OK) {
            slot->info.loaded = true;
            ESP_LOGI(TAG, "module '%s' load ok (has_rpc=%d)", slot->info.id,
                     slot->rpc != NULL);
        } else {
            ESP_LOGW(TAG, "module '%s' load failed: %s", slot->info.id, esp_err_to_name(err));
        }
    }
    reapply_host_rpcs();
    return ESP_OK;
}

esp_err_t minihub_module_load_deferred(const cJSON *adapters_root)
{
    for (size_t i = 0; i < s_module_count; i++) {
        module_slot_t *slot = &s_modules[i];
        if (slot->hybrid_entry != 2 || !slot->info.enabled || slot->info.loaded) {
            continue;
        }
        esp_err_t err = validate_module_info(&slot->info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skip deferred '%s' (validate: %s)", slot->info.id, esp_err_to_name(err));
            continue;
        }
        if (load_guard_read(slot) == LOAD_GUARD_STATE_LOADING) {
            ESP_LOGW(TAG, "skip deferred '%s' — load guard stuck in loading", slot->info.id);
            continue;
        }
        ESP_LOGI(TAG, "loading deferred companion '%s' binary='%s'",
                 slot->info.id, slot->binary_path[0] ? slot->binary_path : "(none)");
        const cJSON *adapter_cfg = adapter_config_for(slot->info.adapter_name, adapters_root);
        err = try_load_binary(slot, adapter_cfg);
        if (err == ESP_OK) {
            slot->info.loaded = true;
            ESP_LOGI(TAG, "deferred module '%s' load ok", slot->info.id);
            if (slot->info.adapter_name[0]) {
                esp_err_t soft = minihub_adapter_soft_reload(slot->info.adapter_name, adapters_root);
                if (soft != ESP_OK) {
                    ESP_LOGW(TAG, "soft-start deferred '%s': %s",
                             slot->info.adapter_name, esp_err_to_name(soft));
                }
            }
        } else {
            ESP_LOGW(TAG, "deferred module '%s' load failed: %s",
                     slot->info.id, esp_err_to_name(err));
        }
    }
    reapply_host_rpcs();
    return ESP_OK;
}

esp_err_t minihub_module_unload(const char *module_id)
{
    if (!module_id) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i].info.id, module_id) != 0) {
            continue;
        }
        if (!s_modules[i].info.loaded) {
            return ESP_ERR_INVALID_STATE;
        }
        /* Reserved: esp_dl_close, adapter stop/deinit, unregister */
        ESP_LOGW(TAG, "module unload not implemented: %s", module_id);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_module_reload(const cJSON *adapters_root)
{
    detach_all_module_adapters();
#if CONFIG_MINIHUB_DYNAMIC_MODULES
    minihub_module_dl_unload_all();
#endif
    esp_err_t err = minihub_module_loader_init();
    if (err != ESP_OK) {
        return err;
    }
    return minihub_module_load_all(adapters_root);
}
