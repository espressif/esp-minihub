/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/**
 * Replace stock mcp_list_tools / mcp_call_tool with in-process MiniHub dispatch.
 * Prefer registering via app_capabilities_register_external_group() before
 * app_claw_start() so the group is LLM-visible; safe to call again (idempotent).
 */
esp_err_t minihub_local_mcp_register(void);
