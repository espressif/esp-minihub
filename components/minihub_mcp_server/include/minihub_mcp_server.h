/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t minihub_mcp_server_init(void);
esp_err_t minihub_mcp_server_register_tools(void);
void minihub_mcp_set_hub_ready(bool ready);
bool minihub_mcp_hub_ready(void);

/**
 * @brief In-process MiniHub MCP tools (no HTTP to self / no esp-claw patch).
 * `arguments_json` may be NULL or "{}" for tools with no args.
 * On success, writes tool JSON text into `out`.
 */
esp_err_t minihub_mcp_local_call_tool(const char *tool_name, const char *arguments_json,
                                      char *out, size_t out_len);

/**
 * @brief Format a compact tools/list text for the on-device agent.
 */
esp_err_t minihub_mcp_local_list_tools(char *out, size_t out_len);
