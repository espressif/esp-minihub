/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "minihub_ble_mesh_types.h"

const char *minihub_bm_classify_type(uint8_t caps);

/**
 * Format point_id as "ble_mesh.bm_{unicast:04x}" (two-segment, protocol-prefixed).
 * Type stays on the point's `type` field — do not encode it in the id.
 */
int minihub_bm_format_point_id(char *out, size_t out_len, uint16_t unicast);

/**
 * Rewrite legacy "{type}/bm_XXXX" slash ids to "ble_mesh.bm_XXXX".
 * Returns true when point_id was rewritten in place.
 */
bool minihub_bm_normalize_point_id(char *point_id, size_t len);

/**
 * Pointer to the stable "bm_XXXX" suffix inside point_id (dotted or slash),
 * or NULL if the id does not contain one.
 */
const char *minihub_bm_stable_suffix(const char *point_id);

int minihub_bm_uuid_to_hex(const uint8_t uuid[16], char *out, size_t out_len);
int minihub_bm_hex_to_uuid(const char *hex, uint8_t uuid[16]);
