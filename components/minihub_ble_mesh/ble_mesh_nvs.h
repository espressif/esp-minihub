/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "minihub_ble_mesh_types.h"

esp_err_t minihub_bm_nvs_load(minihub_ble_mesh_device_t *out, size_t cap, size_t *count);
esp_err_t minihub_bm_nvs_upsert(const minihub_ble_mesh_device_t *dev,
                                minihub_ble_mesh_device_t *table, size_t *count, size_t cap);
esp_err_t minihub_bm_nvs_remove(const char *point_id, minihub_ble_mesh_device_t *table, size_t *count);
