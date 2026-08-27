/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

/** Load config/adapters/rules and mark MCP hub ready on success. */
esp_err_t minihub_bringup_start(void);
