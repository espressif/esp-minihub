/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_mesh_host_bluetooth_init(void);
void ble_mesh_host_get_dev_uuid(uint8_t *dev_uuid);
