/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Classify MiniHub type from caps + HA app device id (table-driven; see zigbee_cluster_map). */
const char *minihub_zb_classify_type(uint8_t caps, uint16_t app_device_id);

/**
 * Format point_id as "zigbee.zb_{short:04x}" (two-segment, protocol-prefixed).
 * Returns 0 on success, -1 on error.
 */
int minihub_zb_format_point_id(char *out, size_t out_len, uint16_t short_addr);

/**
 * Rewrite legacy "{type}/zb_XXXX" slash ids to "zigbee.zb_XXXX".
 * No-op when already dotted / unrecognized. Returns true if rewritten.
 */
bool minihub_zb_normalize_point_id(char *point_id, size_t len);

/** Parse IEEE hex string (optional 0x prefix). Returns 0 on success, -1 on error. */
int minihub_zb_parse_ieee(const char *s, uint64_t *out);
