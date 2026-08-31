/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

/** Load config/adapters/rules and mark MCP hub ready on success. */
esp_err_t minihub_bringup_start(void);

#if CONFIG_MINIHUB_MATTER_ADAPTER
/** Start Matter commissioner when adapters.json enables it (after uplink IP). */
esp_err_t minihub_bringup_matter_start(void);
#endif
