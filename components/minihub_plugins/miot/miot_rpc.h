/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/** Host-static entry (unique). `.mhub` packs still export ABI name `minihub_module_rpc`. */
esp_err_t miot_module_rpc(const char *method, const cJSON *params, cJSON **out);

/** 动态 .so 导出符号（与 MINIHUB_MODULE_RPC_SYMBOL 一致） */
esp_err_t minihub_module_rpc(const char *method, const cJSON *params, cJSON **out);
