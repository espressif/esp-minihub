/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_core.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "command_router";

/* Reload-safety without a core<->adapter include cycle:
 * The adapter reload holds the (recursive) adapter lifecycle lock across
 * stop -> dlclose -> load. To stop a command from jumping into a module's
 * freed .text, command dispatch takes the SAME lock. The lock is injected by
 * the adapter component (which owns it) via minihub_command_set_lifecycle_lock,
 * so core need not depend on the adapter headers. A single shared recursive
 * lock means there is no lock-ordering deadlock: a command handler may
 * re-invoke commands or call a module RPC, both of which take the same lock
 * recursively. */
static void (*s_lock_take)(void) = NULL;
static void (*s_lock_give)(void) = NULL;

void minihub_command_set_lifecycle_lock(void (*take)(void), void (*give)(void))
{
    s_lock_take = take;
    s_lock_give = give;
}

static inline void cmd_lock(void) { if (s_lock_take) s_lock_take(); }
static inline void cmd_unlock(void) { if (s_lock_give) s_lock_give(); }

typedef struct {
    char adapter[MINIHUB_ADAPTER_MAX];
    minihub_command_handler_t handler;
} adapter_handler_t;

static adapter_handler_t s_handlers[MINIHUB_MAX_HANDLERS];
static size_t s_handler_count;

esp_err_t minihub_command_router_init(void)
{
    s_handler_count = 0;
    return ESP_OK;
}

esp_err_t minihub_command_register_adapter(const char *adapter, minihub_command_handler_t handler)
{
    if (!adapter || !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    cmd_lock();
    for (size_t i = 0; i < s_handler_count; i++) {
        if (strcmp(s_handlers[i].adapter, adapter) == 0) {
            s_handlers[i].handler = handler;
            cmd_unlock();
            return ESP_OK;
        }
    }

    if (s_handler_count >= MINIHUB_MAX_HANDLERS) {
        cmd_unlock();
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_handlers[s_handler_count].adapter, adapter, MINIHUB_ADAPTER_MAX - 1);
    s_handlers[s_handler_count].handler = handler;
    s_handler_count++;
    ESP_LOGI(TAG, "registered command handler for adapter '%s'", adapter);
    cmd_unlock();
    return ESP_OK;
}

esp_err_t minihub_command_clear_handlers(void)
{
    cmd_lock();
    s_handler_count = 0;
    cmd_unlock();
    return ESP_OK;
}

static minihub_command_handler_t find_handler(const char *adapter)
{
    for (size_t i = 0; i < s_handler_count; i++) {
        if (strcmp(s_handlers[i].adapter, adapter) == 0) {
            return s_handlers[i].handler;
        }
    }
    return NULL;
}

esp_err_t minihub_command_invoke(const minihub_command_t *cmd)
{
    if (!cmd || cmd->point_id[0] == '\0' || cmd->cmd[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    minihub_point_t snapshot = {0};
    esp_err_t err = minihub_point_get_snapshot(cmd->point_id, &snapshot);
    if (err != ESP_OK) {
        return err;
    }

    cmd_lock();
    minihub_command_handler_t handler = find_handler(snapshot.adapter);
    if (!handler) {
        cmd_unlock();
        if (snapshot.props) cJSON_Delete(snapshot.props);
        ESP_LOGE(TAG, "no handler for adapter '%s'", snapshot.adapter);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *invoke_data = cJSON_CreateObject();
    cJSON_AddStringToObject(invoke_data, "point_id", cmd->point_id);
    cJSON_AddStringToObject(invoke_data, "cmd", cmd->cmd);
    minihub_event_bus_publish("command.invoked", invoke_data);
    cJSON_Delete(invoke_data);

    /* Hold the (shared) lifecycle lock across the call so a concurrent reload
     * cannot clear this handler table and dlclose the module's .text beneath
     * us (use-after-free crash when the Web issues a command during an
     * install/uninstall cycle). */
    err = handler(cmd);
    cmd_unlock();

    if (snapshot.props) cJSON_Delete(snapshot.props);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command %s on %s failed: %s", cmd->cmd, cmd->point_id, esp_err_to_name(err));
    }
    return err;
}
