/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdlib.h>

/* Capacity limits. Each is overridable from the build system (e.g. an
 * application's CMakeLists.txt via target_compile_definitions(minihub_adapter
 * PRIVATE MINIHUB_MAX_ADAPTERS=32)) without editing this file. All array
 * dimensions and bounds checks below use these macros, so a custom value is
 * always safe. */
#ifndef MINIHUB_MAX_ADAPTERS
#define MINIHUB_MAX_ADAPTERS 16
#endif
#ifndef MINIHUB_OWNER_LEN
#define MINIHUB_OWNER_LEN 28
#endif
#ifndef MINIHUB_POLL_HOOKS_MAX
#define MINIHUB_POLL_HOOKS_MAX 8
#endif

static const char *TAG = "adapter_registry";

static const minihub_adapter_t *s_registered[MINIHUB_MAX_ADAPTERS];
static size_t s_registered_count;

static const minihub_adapter_t *s_active[MINIHUB_MAX_ADAPTERS];
static size_t s_active_count;

/** Non-blocking poll hooks (e.g. JS runtime) invoked each adapter_poll_task
 *  tick, after the lifecycle lock is released. Must return quickly. */
static minihub_adapter_poll_hook_t s_poll_hooks[MINIHUB_POLL_HOOKS_MAX];
static size_t s_poll_hooks_count;

/** Radio-host adapters left running by stop_all — start_all must not call start() on these. */
static const minihub_adapter_t *s_kept_radio[MINIHUB_MAX_ADAPTERS];
static size_t s_kept_radio_count;

/** Recursive: hub_reload may lock then call stop_all/soft_reload which also lock. */
static SemaphoreHandle_t s_lifecycle_mu;

/* Poll and command handlers do network I/O. Holding s_lifecycle_mu across that
 * blocks /api/adapters (1 s) and the overview drops every device. The mutex
 * only covers the registry. In-flight calls are counted in s_external and
 * drained by lock_quiesced before anything is stopped or dlclosed. */
static bool s_quiesce;
static int s_external;
#define LIFECYCLE_PIN_SLOTS 8
static TaskHandle_t s_pin_task[LIFECYCLE_PIN_SLOTS];
static uint8_t s_pin_depth[LIFECYCLE_PIN_SLOTS];

static int pin_find_locked(TaskHandle_t self)
{
    for (int i = 0; i < LIFECYCLE_PIN_SLOTS; i++) {
        if (s_pin_task[i] == self) {
            return i;
        }
    }
    return -1;
}

static int pin_alloc_locked(TaskHandle_t self)
{
    for (int i = 0; i < LIFECYCLE_PIN_SLOTS; i++) {
        if (s_pin_task[i] == NULL) {
            s_pin_task[i] = self;
            s_pin_depth[i] = 1;
            return i;
        }
    }
    return -1;
}

/* Caller does not hold the mutex. Nested calls on the same task do not wait
 * for quiesce, so a poll/command may re-enter command_invoke. */
static void lifecycle_external_enter(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    for (;;) {
        if (minihub_adapter_lifecycle_lock(portMAX_DELAY) != ESP_OK) {
            return;
        }
        int slot = pin_find_locked(self);
        if (slot >= 0) {
            if (s_pin_depth[slot] < UINT8_MAX) {
                s_pin_depth[slot]++;
            }
            s_external++;
            minihub_adapter_lifecycle_unlock();
            return;
        }
        if (!s_quiesce) {
            if (pin_alloc_locked(self) < 0) {
                ESP_LOGW(TAG, "lifecycle pin table full");
            }
            s_external++;
            minihub_adapter_lifecycle_unlock();
            return;
        }
        minihub_adapter_lifecycle_unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void lifecycle_external_leave(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (minihub_adapter_lifecycle_lock(portMAX_DELAY) != ESP_OK) {
        return;
    }
    int slot = pin_find_locked(self);
    if (slot >= 0) {
        if (s_pin_depth[slot] > 1) {
            s_pin_depth[slot]--;
        } else {
            s_pin_task[slot] = NULL;
            s_pin_depth[slot] = 0;
        }
    }
    if (s_external > 0) {
        s_external--;
    }
    minihub_adapter_lifecycle_unlock();
}

/* Already holding the mutex. Used by the poll task after it has decided to run. */
static void lifecycle_external_enter_locked(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    int slot = pin_find_locked(self);
    if (slot >= 0) {
        if (s_pin_depth[slot] < UINT8_MAX) {
            s_pin_depth[slot]++;
        }
    } else if (pin_alloc_locked(self) < 0) {
        ESP_LOGW(TAG, "lifecycle pin table full; unload will still wait on s_external");
    }
    s_external++;
}

static void cmd_router_lock_take(void) { (void)minihub_adapter_lifecycle_lock(portMAX_DELAY); }
static void cmd_router_lock_give(void) { minihub_adapter_lifecycle_unlock(); }

/* Per-registered-adapter owner (module id), parallel to s_registered. Lets us
 * tag a module's event subscriptions and reap them on unload (UAF guard). */
static char s_owner_by_name[MINIHUB_MAX_ADAPTERS][MINIHUB_OWNER_LEN];

static esp_err_t ensure_lifecycle_mu(void)
{
    if (s_lifecycle_mu) {
        return ESP_OK;
    }
    s_lifecycle_mu = xSemaphoreCreateRecursiveMutex();
    if (!s_lifecycle_mu) {
        return ESP_ERR_NO_MEM;
    }
    /* Share this same recursive lock with the command router so a command
     * dispatched during an install/uninstall reload cannot jump into a
     * module's dlclosed .text. */
    minihub_command_set_lifecycle_lock(cmd_router_lock_take, cmd_router_lock_give);
    minihub_command_set_lifecycle_call(lifecycle_external_enter, lifecycle_external_leave);
    return ESP_OK;
}

esp_err_t minihub_adapter_lifecycle_lock_quiesced(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    if (minihub_adapter_lifecycle_lock(timeout_ms) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    s_quiesce = true;
    while (s_external > 0) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            s_quiesce = false;
            minihub_adapter_lifecycle_unlock();
            return ESP_ERR_TIMEOUT;
        }
        minihub_adapter_lifecycle_unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
        if (minihub_adapter_lifecycle_lock(timeout_ms) != ESP_OK) {
            /* We already set s_quiesce. Take the mutex without a timeout so it
             * cannot stay set after this function returns unlocked. */
            if (minihub_adapter_lifecycle_lock(portMAX_DELAY) == ESP_OK) {
                s_quiesce = false;
                minihub_adapter_lifecycle_unlock();
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

void minihub_adapter_lifecycle_unlock_quiesced(void)
{
    s_quiesce = false;
    minihub_adapter_lifecycle_unlock();
}

esp_err_t minihub_adapter_lifecycle_lock(uint32_t timeout_ms)
{
    if (ensure_lifecycle_mu() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lifecycle_mu, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void minihub_adapter_lifecycle_unlock(void)
{
    if (s_lifecycle_mu) {
        xSemaphoreGiveRecursive(s_lifecycle_mu);
    }
}

static bool adapter_is_radio_host(const minihub_adapter_t *adapter)
{
    if (!adapter || !adapter->name) {
        return false;
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    if (strcmp(adapter->name, "zigbee") == 0) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_BLE_MESH_SDK
    if (strcmp(adapter->name, "ble_mesh") == 0) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
    if (strcmp(adapter->name, "thread") == 0) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
    if (strcmp(adapter->name, "wifi") == 0) {
        return true;
    }
#endif
    return false;
}

static bool adapter_was_kept(const minihub_adapter_t *adapter)
{
    for (size_t k = 0; k < s_kept_radio_count; k++) {
        if (s_kept_radio[k] == adapter) {
            return true;
        }
    }
    return false;
}

static int radio_start_rank(const char *name)
{
    if (!name) {
        return 10;
    }
    if (strcmp(name, "wifi") == 0) {
        return 0;
    }
    if (strcmp(name, "zigbee") == 0 || strcmp(name, "thread") == 0) {
        return 1;
    }
    if (strcmp(name, "ble_mesh") == 0) {
        return 2;
    }
    return 3;
}

static bool adapter_name_is(const minihub_adapter_t *adapter, const char *name)
{
    return adapter && adapter->name && name && strcmp(adapter->name, name) == 0;
}

/* True when @p adapter is enabled in the adapters.json object (present and not
 * explicitly enabled=false). Mirrors the enabled check used by load_all. */
static bool adapter_enabled_in_config(const cJSON *adapters, const minihub_adapter_t *adapter)
{
    if (!adapters || !adapter || !adapter->name) {
        return false;
    }
    const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter->name);
    if (!adapter_cfg) {
        return false;
    }
    if (cJSON_IsObject(adapter_cfg)) {
        const cJSON *enabled = cJSON_GetObjectItem(adapter_cfg, "enabled");
        if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) {
            return false;
        }
    }
    return true;
}

/* 2.4 GHz radio arbitration (docs/zh_CN/radios.rst):
 *  - Zigbee and Thread share IEEE 802.15.4 → Thread loses when both enabled.
 *  - Wi-Fi + (Zigbee|Thread) + BLE Mesh is the third group → BLE Mesh loses. */
static void compute_radio_skips(bool has_wifi, bool has_zigbee, bool has_thread, bool has_ble_mesh,
                                bool *skip_thread, bool *skip_ble_mesh)
{
    *skip_thread = has_zigbee && has_thread;
    *skip_ble_mesh = has_wifi && (has_zigbee || has_thread) && has_ble_mesh;
}

/* The losing radio adapter for the current arbitration (Thread, or BLE Mesh). */
static bool adapter_is_radio_loser(const minihub_adapter_t *adapter, bool skip_thread, bool skip_ble_mesh)
{
    return adapter &&
           ((skip_thread && adapter_name_is(adapter, "thread")) ||
            (skip_ble_mesh && adapter_name_is(adapter, "ble_mesh")));
}

static int radio_start_cmp(const void *a, const void *b)
{
    const minihub_adapter_t *const *pa = (const minihub_adapter_t *const *)a;
    const minihub_adapter_t *const *pb = (const minihub_adapter_t *const *)b;
    int ra = radio_start_rank((*pa) ? (*pa)->name : NULL);
    int rb = radio_start_rank((*pb) ? (*pb)->name : NULL);
    return ra - rb;
}

/* Owner (module id) recorded at registration for a given adapter, or "" for core. */
static const char *owner_of_adapter(const minihub_adapter_t *adapter)
{
    if (!adapter) {
        return "";
    }
    for (size_t i = 0; i < s_registered_count; i++) {
        if (s_registered[i] == adapter) {
            return s_owner_by_name[i];
        }
    }
    return "";
}

esp_err_t minihub_adapter_registry_init(void)
{
    s_registered_count = 0;
    s_active_count = 0;
    s_kept_radio_count = 0;
    memset(s_owner_by_name, 0, sizeof(s_owner_by_name));
    return ensure_lifecycle_mu();
}

esp_err_t minihub_adapter_register_poll_hook(minihub_adapter_poll_hook_t hook)
{
    if (!hook) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_poll_hooks_count; i++) {
        if (s_poll_hooks[i] == hook) {
            return ESP_OK; /* idempotent */
        }
    }
    if (s_poll_hooks_count >= MINIHUB_POLL_HOOKS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    s_poll_hooks[s_poll_hooks_count++] = hook;
    return ESP_OK;
}

esp_err_t minihub_adapter_unregister_poll_hook(minihub_adapter_poll_hook_t hook)
{
    if (!hook) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t write = 0;
    bool found = false;
    for (size_t i = 0; i < s_poll_hooks_count; i++) {
        if (s_poll_hooks[i] == hook) {
            found = true;
            continue;
        }
        s_poll_hooks[write++] = s_poll_hooks[i];
    }
    s_poll_hooks_count = write;
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_adapter_register(const minihub_adapter_t *adapter)
{
    if (!adapter || !adapter->name) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_registered_count; i++) {
        const minihub_adapter_t *existing = s_registered[i];
        if (existing == adapter) {
            return ESP_OK;
        }
        if (existing && existing->name && strcmp(existing->name, adapter->name) == 0) {
            ESP_LOGW(TAG, "adapter '%s' already registered (v%s); skip duplicate v%s",
                     adapter->name,
                     existing->version ? existing->version : "?",
                     adapter->version ? adapter->version : "?");
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s_registered_count >= 16) {
        return ESP_ERR_NO_MEM;
    }
    /* Tag this adapter with the module id active during registration (set by the
     * module loader around entry), so the adapter's event subscriptions can be
     * reaped when the owning module is unloaded. */
    strncpy(s_owner_by_name[s_registered_count], minihub_event_bus_current_owner(),
            sizeof(s_owner_by_name[0]) - 1);
    s_registered[s_registered_count++] = adapter;
    return ESP_OK;
}

esp_err_t minihub_adapter_unregister(const minihub_adapter_t *adapter)
{
    if (!adapter) {
        return ESP_ERR_INVALID_ARG;
    }
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    size_t write = 0;
    bool found = false;
    for (size_t i = 0; i < s_registered_count; i++) {
        if (s_registered[i] == adapter) {
            found = true;
            continue;
        }
        s_registered[write] = s_registered[i];
        strncpy(s_owner_by_name[write], s_owner_by_name[i], sizeof(s_owner_by_name[0]) - 1);
        write++;
    }
    s_registered_count = write;

    write = 0;
    for (size_t i = 0; i < s_active_count; i++) {
        if (s_active[i] == adapter) {
            continue;
        }
        s_active[write++] = s_active[i];
    }
    s_active_count = write;

    minihub_adapter_lifecycle_unlock();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static const minihub_adapter_t **adapter_ptr_begin(void)
{
    return (const minihub_adapter_t **)s_registered;
}

static const minihub_adapter_t **adapter_ptr_end(void)
{
    return (const minihub_adapter_t **)&s_registered[s_registered_count];
}

const minihub_adapter_t *minihub_adapter_get(size_t index)
{
    if (index >= s_registered_count) {
        return NULL;
    }
    return s_registered[index];
}

const minihub_adapter_t *minihub_adapter_find(const char *name)
{
    size_t count = minihub_adapter_count();
    for (size_t i = 0; i < count; i++) {
        const minihub_adapter_t *adapter = minihub_adapter_get(i);
        if (adapter && adapter->name && strcmp(adapter->name, name) == 0) {
            return adapter;
        }
    }
    return NULL;
}

size_t minihub_adapter_count(void)
{
    return (size_t)(adapter_ptr_end() - adapter_ptr_begin());
}

esp_err_t minihub_adapter_load_all(const cJSON *config)
{
    if (!config || !cJSON_IsObject(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *adapters = cJSON_GetObjectItem(config, "adapters");
    if (!adapters || !cJSON_IsObject(adapters)) {
        ESP_LOGW(TAG, "no adapters section in config");
        return ESP_OK;
    }

    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    /* Resolve the 2.4 GHz radio arbitration up front, so the losing adapter
     * (Thread when Zigbee is also enabled, or BLE Mesh when all three groups
     * are) is never initialised — not just never started. See
     * docs/zh_CN/radios.rst. */
    bool has_wifi = false, has_zigbee = false, has_thread = false, has_ble_mesh = false;
    size_t rc = minihub_adapter_count();
    for (size_t i = 0; i < rc; i++) {
        const minihub_adapter_t *a = minihub_adapter_get(i);
        if (!a || !a->name || !adapter_enabled_in_config(adapters, a)) {
            continue;
        }
        if (strcmp(a->name, "wifi") == 0) {
            has_wifi = true;
        } else if (strcmp(a->name, "zigbee") == 0) {
            has_zigbee = true;
        } else if (strcmp(a->name, "thread") == 0) {
            has_thread = true;
        } else if (strcmp(a->name, "ble_mesh") == 0) {
            has_ble_mesh = true;
        }
    }
    bool skip_thread = false, skip_ble_mesh = false;
    compute_radio_skips(has_wifi, has_zigbee, has_thread, has_ble_mesh,
                        &skip_thread, &skip_ble_mesh);

    /* Preserve radio-host adapters left active by stop_all (Zigbee / BLE Mesh).
     * Drop them if adapters.json now has enabled=false; otherwise refresh init
     * (packs / provision policy) without tearing the radio stack down. A losing
     * radio adapter is stopped and dropped from the active set instead. */
    const minihub_adapter_t *survivors[MINIHUB_MAX_ADAPTERS];
    size_t survivor_in = s_active_count < MINIHUB_MAX_ADAPTERS ? s_active_count : MINIHUB_MAX_ADAPTERS;
    size_t survivor_count = 0;
    for (size_t i = 0; i < survivor_in; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (!adapter || !adapter->name) {
            continue;
        }
        const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter->name);
        bool enabled = true;
        if (cJSON_IsObject(adapter_cfg)) {
            const cJSON *enabled_j = cJSON_GetObjectItem(adapter_cfg, "enabled");
            if (cJSON_IsBool(enabled_j) && !cJSON_IsTrue(enabled_j)) {
                enabled = false;
            }
        } else if (!adapter_cfg) {
            enabled = false;
        }
        if (!enabled) {
            ESP_LOGI(TAG, "radio-host adapter '%s' disabled in adapters.json — stopping",
                     adapter->name);
            if (adapter->stop) {
                adapter->stop();
            }
            /* Skip deinit: radio stacks keep controller/mesh state for a later
             * re-enable; freeing while NimBLE/Zigbee still runs caused crashes. */
            minihub_point_unregister_by_adapter(adapter->name);
            continue;
        }
        if (adapter_is_radio_loser(adapter, skip_thread, skip_ble_mesh)) {
            ESP_LOGW(TAG, "radio mutex: '%s' loses 2.4GHz arbitration — stopping",
                     adapter->name);
            if (adapter->stop) {
                adapter->stop();
            }
            minihub_point_unregister_by_adapter(adapter->name);
            continue;
        }
        if (adapter->init && adapter_cfg) {
            minihub_event_bus_set_current_owner(owner_of_adapter(adapter));
            esp_err_t err = adapter->init(adapter_cfg);
            minihub_event_bus_set_current_owner("");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "radio-host '%s' config refresh failed: %s",
                         adapter->name, esp_err_to_name(err));
            }
        }
        if (survivor_count < MINIHUB_MAX_ADAPTERS) {
            survivors[survivor_count++] = adapter;
        }
    }

    s_active_count = 0;
    for (size_t i = 0; i < survivor_count && s_active_count < MINIHUB_MAX_ADAPTERS; i++) {
        s_active[s_active_count++] = survivors[i];
    }

    size_t count = minihub_adapter_count();
    for (size_t idx = 0; idx < count && s_active_count < MINIHUB_MAX_ADAPTERS; idx++) {
        const minihub_adapter_t *adapter = minihub_adapter_get(idx);
        if (!adapter) {
            continue;
        }
        bool already = false;
        for (size_t s = 0; s < s_active_count; s++) {
            if (s_active[s] == adapter) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }

        const cJSON *adapter_cfg = cJSON_GetObjectItem(adapters, adapter->name);
        if (!adapter_cfg) {
            continue;
        }

        if (cJSON_IsObject(adapter_cfg)) {
            const cJSON *enabled = cJSON_GetObjectItem(adapter_cfg, "enabled");
            if (cJSON_IsBool(enabled) && !cJSON_IsTrue(enabled)) {
                ESP_LOGI(TAG, "adapter '%s' disabled", adapter->name);
                continue;
            }
        }

        if (adapter_is_radio_loser(adapter, skip_thread, skip_ble_mesh)) {
            /* Never bring up the losing radio adapter: apply the arbitration at
             * init time, not only at start time. */
            ESP_LOGW(TAG, "radio mutex: '%s' not initialised (loses 2.4GHz arbitration)",
                     adapter->name ? adapter->name : "?");
            continue;
        }

        esp_err_t err = ESP_OK;
        if (adapter->init) {
            minihub_event_bus_set_current_owner(owner_of_adapter(adapter));
            err = adapter->init(adapter_cfg);
            minihub_event_bus_set_current_owner("");
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "adapter '%s' init failed: %s", adapter->name, esp_err_to_name(err));
                continue;
            }
        }

        s_active[s_active_count++] = adapter;
        ESP_LOGI(TAG, "loaded adapter '%s' v%s", adapter->name, adapter->version ? adapter->version : "?");
    }

    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_start_all(void)
{
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    bool has_wifi = false, has_zigbee = false, has_thread = false, has_ble_mesh = false;
    for (size_t i = 0; i < s_active_count; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (adapter_name_is(adapter, "wifi")) {
            has_wifi = true;
        } else if (adapter_name_is(adapter, "zigbee")) {
            has_zigbee = true;
        } else if (adapter_name_is(adapter, "thread")) {
            has_thread = true;
        } else if (adapter_name_is(adapter, "ble_mesh")) {
            has_ble_mesh = true;
        }
    }

    bool skip_thread = false, skip_ble_mesh = false;
    compute_radio_skips(has_wifi, has_zigbee, has_thread, has_ble_mesh,
                        &skip_thread, &skip_ble_mesh);
    if (skip_thread) {
        ESP_LOGW(TAG, "radio mutex: zigbee and thread both enabled — skipping thread");
    }
    if (skip_ble_mesh) {
        ESP_LOGW(TAG, "radio mutex: wifi + 802.15.4 + ble_mesh — skipping ble_mesh");
    }

    const minihub_adapter_t *ordered[MINIHUB_MAX_ADAPTERS];
    size_t n = s_active_count < MINIHUB_MAX_ADAPTERS ? s_active_count : MINIHUB_MAX_ADAPTERS;
    for (size_t i = 0; i < n; i++) {
        ordered[i] = s_active[i];
    }
    qsort(ordered, n, sizeof(ordered[0]), radio_start_cmp);

    for (size_t i = 0; i < n; i++) {
        const minihub_adapter_t *adapter = ordered[i];
        if (!adapter) {
            continue;
        }
        bool skip = (skip_thread && adapter_name_is(adapter, "thread")) ||
                    (skip_ble_mesh && adapter_name_is(adapter, "ble_mesh"));
        bool was_kept = adapter_was_kept(adapter);
        if (skip) {
            ESP_LOGW(TAG, "adapter '%s' not started (radio mutex)",
                     adapter->name ? adapter->name : "?");
            if (was_kept && adapter->stop) {
                adapter->stop();
            }
            continue;
        }
        if (!adapter->start) {
            continue;
        }
        /* Only skip start for radio hosts that were actually kept running.
         * Newly enabled radio (e.g. zigbee→ble_mesh) must still call start(). */
        if (was_kept) {
            if (adapter->handle_command) {
                minihub_command_register_adapter(adapter->name, adapter->handle_command);
            }
            continue;
        }
        minihub_event_bus_set_current_owner(owner_of_adapter(adapter));
        esp_err_t err = adapter->start();
        minihub_event_bus_set_current_owner("");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adapter '%s' start failed: %s", adapter->name, esp_err_to_name(err));
            continue;
        }
        if (adapter->handle_command) {
            minihub_command_register_adapter(adapter->name, adapter->handle_command);
        }
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "adapter", adapter->name);
        minihub_event_bus_publish("adapter.loaded", data);
        cJSON_Delete(data);
    }
    s_kept_radio_count = 0;
    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_stop_all(void)
{
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    const minihub_adapter_t *kept[MINIHUB_MAX_ADAPTERS];
    size_t kept_count = 0;
    s_kept_radio_count = 0;

    for (size_t i = 0; i < s_active_count; i++) {
        const minihub_adapter_t *adapter = s_active[i];
        if (adapter_is_radio_host(adapter)) {
            if (kept_count < MINIHUB_MAX_ADAPTERS) {
                kept[kept_count++] = adapter;
            }
            if (s_kept_radio_count < MINIHUB_MAX_ADAPTERS) {
                s_kept_radio[s_kept_radio_count++] = adapter;
            }
            ESP_LOGI(TAG, "keeping radio-host adapter '%s' across reload", adapter->name);
            continue;
        }
        if (adapter->stop) {
            adapter->stop();
        }
        if (adapter->deinit) {
            adapter->deinit();
        }
        if (adapter->name) {
            minihub_point_unregister_by_adapter(adapter->name);
        }
    }
    minihub_command_clear_handlers();
    s_active_count = 0;
    for (size_t i = 0; i < kept_count; i++) {
        s_active[s_active_count++] = kept[i];
        if (kept[i]->handle_command) {
            minihub_command_register_adapter(kept[i]->name, kept[i]->handle_command);
        }
    }
    minihub_adapter_lifecycle_unlock();
    return ESP_OK;
}

esp_err_t minihub_adapter_soft_reload(const char *name, const cJSON *adapters_root)
{
    if (!name || !adapters_root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (minihub_adapter_lifecycle_lock_quiesced(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    const minihub_adapter_t *adapter = minihub_adapter_find(name);
    if (!adapter) {
        minihub_adapter_lifecycle_unlock_quiesced();
        return ESP_ERR_NOT_FOUND;
    }
    if (adapter_is_radio_host(adapter)) {
        minihub_adapter_lifecycle_unlock_quiesced();
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    const cJSON *adapter_cfg = adapters ? cJSON_GetObjectItem(adapters, name) : NULL;
    if (!adapter_cfg) {
        minihub_adapter_lifecycle_unlock_quiesced();
        return ESP_ERR_NOT_FOUND;
    }

    if (adapter->stop) {
        adapter->stop();
    }
    if (adapter->deinit) {
        adapter->deinit();
    }
    minihub_point_unregister_by_adapter(name);

    esp_err_t err = ESP_OK;
    minihub_event_bus_set_current_owner(owner_of_adapter(adapter));
    if (adapter->init) {
        err = adapter->init(adapter_cfg);
        if (err != ESP_OK) {
            minihub_event_bus_set_current_owner("");
            minihub_adapter_lifecycle_unlock_quiesced();
            return err;
        }
    }
    if (adapter->start) {
        err = adapter->start();
        if (err != ESP_OK) {
            minihub_event_bus_set_current_owner("");
            minihub_adapter_lifecycle_unlock_quiesced();
            return err;
        }
    }
    minihub_event_bus_set_current_owner("");
    if (adapter->handle_command) {
        minihub_command_register_adapter(name, adapter->handle_command);
    }
    ESP_LOGI(TAG, "soft-reloaded adapter '%s'", name);
    minihub_adapter_lifecycle_unlock_quiesced();
    return ESP_OK;
}

static bool adapter_is_active(const minihub_adapter_t *adapter)
{
    for (size_t i = 0; i < s_active_count; i++) {
        if (s_active[i] == adapter) {
            return true;
        }
    }
    return false;
}

esp_err_t minihub_adapter_resync(const char *name, const cJSON *adapters_root)
{
    if (!name || !adapters_root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (minihub_adapter_lifecycle_lock(60000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }

    /* minihub_adapter_find() walks the whole linker section, so it also answers
     * for adapters that are compiled in but disabled in adapters.json. Only a
     * running adapter may republish points. */
    const minihub_adapter_t *adapter = minihub_adapter_find(name);
    if (!adapter || !adapter_is_active(adapter)) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (!adapter->resync) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }

    const cJSON *adapters = cJSON_GetObjectItem(adapters_root, "adapters");
    const cJSON *adapter_cfg = adapters ? cJSON_GetObjectItem(adapters, name) : NULL;
    if (!adapter_cfg) {
        minihub_adapter_lifecycle_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = adapter->resync(adapter_cfg);
    ESP_LOGI(TAG, "resynced adapter '%s': %s", name, esp_err_to_name(err));
    minihub_adapter_lifecycle_unlock();
    return err;
}

esp_err_t minihub_adapter_reload_all(const cJSON *config)
{
    esp_err_t err = minihub_adapter_stop_all();
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_adapter_load_all(config);
    if (err != ESP_OK) {
        return err;
    }
    return minihub_adapter_start_all();
}

static void adapter_poll_task(void *arg)
{
    (void)arg;
    while (true) {
        const minihub_adapter_t *batch[MINIHUB_MAX_ADAPTERS];
        size_t n = 0;
        /* Snapshot under the mutex, then poll outside it. LAN/cloud reads
         * inside poll() take seconds; holding the mutex that long made
         * /api/adapters time out and the overview hide every device. */
        if (minihub_adapter_lifecycle_lock(1000) == ESP_OK) {
            if (!s_quiesce) {
                for (size_t i = 0; i < s_active_count && n < MINIHUB_MAX_ADAPTERS; i++) {
                    const minihub_adapter_t *adapter = s_active[i];
                    if (adapter && adapter->poll) {
                        batch[n++] = adapter;
                    }
                }
                if (n > 0) {
                    lifecycle_external_enter_locked();
                }
            }
            minihub_adapter_lifecycle_unlock();
        }
        for (size_t i = 0; i < n; i++) {
            batch[i]->poll();
        }
        if (n > 0) {
            lifecycle_external_leave();
        }
        /* Fire non-blocking poll hooks (e.g. the JS runtime) after releasing
         * the lock so a hook can never delay reload or adapter polling. */
        for (size_t i = 0; i < s_poll_hooks_count; i++) {
            if (s_poll_hooks[i]) {
                s_poll_hooks[i]();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t minihub_adapter_start_poll_task(void)
{
    const uint32_t stack_words = 8192;
    BaseType_t ok = minihub_task_create_psram(adapter_poll_task, "adapter_poll", stack_words, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
