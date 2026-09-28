/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"
#include "esp_err.h"

esp_err_t thread_module_rpc(const char *method, const cJSON *params, cJSON **out);
