/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "minihub_ble_mesh_types.h"

const char *minihub_bm_classify_type(uint8_t caps);

int minihub_bm_format_point_id(char *out, size_t out_len, const char *type, uint16_t unicast);

int minihub_bm_uuid_to_hex(const uint8_t uuid[16], char *out, size_t out_len);
int minihub_bm_hex_to_uuid(const char *hex, uint8_t uuid[16]);
