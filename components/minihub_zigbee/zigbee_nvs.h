/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "minihub_zigbee_types.h"

esp_err_t minihub_zb_nvs_load(minihub_zigbee_device_t *out, size_t cap, size_t *count);
esp_err_t minihub_zb_nvs_save(const minihub_zigbee_device_t *devs, size_t count);
esp_err_t minihub_zb_nvs_upsert(const minihub_zigbee_device_t *dev,
                                minihub_zigbee_device_t *table, size_t *count, size_t cap);
/** Remove by point_id from table and persist. ESP_ERR_NOT_FOUND if missing. */
esp_err_t minihub_zb_nvs_remove(const char *point_id, minihub_zigbee_device_t *table, size_t *count);
