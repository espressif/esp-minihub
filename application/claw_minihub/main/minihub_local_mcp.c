/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stock esp-claw cap_mcp_client HTTP path never creates an esp_mcp_t instance,
 * so mcp_list_tools/mcp_call_tool return ESP_ERR_INVALID_STATE
 * ("MCP instance not configured"). Replace those two caps with in-process
 * MiniHub tool dispatch; leave mcp_discover on the upstream group.
 */

#include "minihub_local_mcp.h"

#include "claw_cap.h"
#include "minihub_mcp_server.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "minihub_local_mcp";

static const char *pick_server_url(const cJSON *input)
{
    static const char *keys[] = {"server_url", "server_name", "url"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *item = cJSON_GetObjectItem(input, keys[i]);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
            return item->valuestring;
        }
    }
    return NULL;
}

static esp_err_t local_list_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char *output,
                                    size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return minihub_mcp_local_list_tools(output, output_size);
}

static esp_err_t local_call_execute(const char *input_json,
                                    const claw_cap_call_context_t *ctx,
                                    char *output,
                                    size_t output_size)
{
    cJSON *input = NULL;
    const cJSON *tool_name_item;
    const cJSON *arguments;
    char *arguments_json = NULL;
    const char *tool_name;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: invalid mcp_call_tool JSON");
        return ESP_ERR_INVALID_ARG;
    }

    /* Accept server_url / server_name / url; local MiniHub ignores host. */
    (void)pick_server_url(input);

    tool_name_item = cJSON_GetObjectItem(input, "tool_name");
    if (!cJSON_IsString(tool_name_item) || !tool_name_item->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: tool_name required");
        return ESP_ERR_INVALID_ARG;
    }
    tool_name = tool_name_item->valuestring;

    arguments = cJSON_GetObjectItem(input, "arguments");
    if (arguments && cJSON_IsObject(arguments)) {
        arguments_json = cJSON_PrintUnformatted(arguments);
    } else {
        arguments_json = strdup("{}");
    }
    if (!arguments_json) {
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }

    err = minihub_mcp_local_call_tool(tool_name, arguments_json, output, output_size);
    free(arguments_json);
    cJSON_Delete(input);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK; /* output already has JSON error */
    }
    return err;
}

static const claw_cap_descriptor_t s_local_mcp_caps[] = {
    {
        .id = "mcp_list_tools",
        .name = "mcp_list_tools",
        .family = "mcp",
        .description =
            "List MiniHub MCP tools (in-process). server_url is optional; use http://127.0.0.1:18791.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"server_url\":{\"type\":\"string\",\"description\":\"Ignored for local MiniHub; prefer http://127.0.0.1:18791\"},"
            "\"server_name\":{\"type\":\"string\",\"description\":\"Alias of server_url\"},"
            "\"endpoint\":{\"type\":\"string\"},\"cursor\":{\"type\":\"string\"}}}",
        .execute = local_list_execute,
    },
    {
        .id = "mcp_call_tool",
        .name = "mcp_call_tool",
        .family = "mcp",
        .description =
            "Call a MiniHub MCP tool in-process. Pass server_url or server_name "
            "(http://127.0.0.1:18791), tool_name, and arguments.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"server_url\":{\"type\":\"string\",\"description\":\"http://127.0.0.1:18791\"},"
            "\"server_name\":{\"type\":\"string\",\"description\":\"Alias of server_url\"},"
            "\"endpoint\":{\"type\":\"string\"},"
            "\"tool_name\":{\"type\":\"string\"},"
            "\"arguments\":{\"type\":\"object\"}},"
            "\"required\":[\"tool_name\"]}",
        .execute = local_call_execute,
    },
};

static const claw_cap_group_t s_local_mcp_group = {
    .group_id = "cap_minihub_local_mcp",
    .plugin_name = "cap_minihub_local_mcp",
    .version = "1.0.0",
    .descriptors = s_local_mcp_caps,
    .descriptor_count = sizeof(s_local_mcp_caps) / sizeof(s_local_mcp_caps[0]),
};

esp_err_t minihub_local_mcp_register(void)
{
    esp_err_t err;

    /* Stock group cannot unregister single descriptors (member_count!=1).
     * Drop the whole group; mcp_discover is unused by MiniHub skill (loopback). */
    err = claw_cap_unregister_group("cap_mcp_client", 3000);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "unregister cap_mcp_client: %s", esp_err_to_name(err));
    }

    if (claw_cap_group_exists(s_local_mcp_group.group_id)) {
        ESP_LOGI(TAG, "local MCP caps already registered");
        return ESP_OK;
    }

    err = claw_cap_register_group(&s_local_mcp_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register local MCP caps: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "replaced cap_mcp_client with in-process MiniHub mcp_list_tools/mcp_call_tool");
    return ESP_OK;
}
