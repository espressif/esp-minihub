/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/** Host-static RPC entry (unique name — do not share minihub_module_rpc with other adapters). */
esp_err_t ble_mesh_module_rpc(const char *method, const cJSON *params, cJSON **out);
