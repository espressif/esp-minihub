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

    for (size_t i = 0; i < s_handler_count; i++) {
        if (strcmp(s_handlers[i].adapter, adapter) == 0) {
            s_handlers[i].handler = handler;
            return ESP_OK;
        }
    }

    if (s_handler_count >= MINIHUB_MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }

    strncpy(s_handlers[s_handler_count].adapter, adapter, MINIHUB_ADAPTER_MAX - 1);
    s_handlers[s_handler_count].handler = handler;
    s_handler_count++;
    ESP_LOGI(TAG, "registered command handler for adapter '%s'", adapter);
    return ESP_OK;
}

esp_err_t minihub_command_clear_handlers(void)
{
    s_handler_count = 0;
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

    minihub_command_handler_t handler = find_handler(snapshot.adapter);
    if (snapshot.props) {
        cJSON_Delete(snapshot.props);
    }
    if (!handler) {
        ESP_LOGE(TAG, "no handler for adapter '%s'", snapshot.adapter);
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *invoke_data = cJSON_CreateObject();
    cJSON_AddStringToObject(invoke_data, "point_id", cmd->point_id);
    cJSON_AddStringToObject(invoke_data, "cmd", cmd->cmd);
    minihub_event_bus_publish("command.invoked", invoke_data);
    cJSON_Delete(invoke_data);

    err = handler(cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "command %s on %s failed: %s", cmd->cmd, cmd->point_id, esp_err_to_name(err));
    }
    return err;
}
