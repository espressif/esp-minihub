/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/** Classify MiniHub type from caps + HA app device id (sensor > light > switch). */
const char *minihub_zb_classify_type(uint8_t caps, uint16_t app_device_id);

/** Format point_id as "{type}/zb_{short:04x}". Returns 0 on success, -1 on error. */
int minihub_zb_format_point_id(char *out, size_t out_len, const char *type, uint16_t short_addr);

/** Parse IEEE hex string (optional 0x prefix). Returns 0 on success, -1 on error. */
int minihub_zb_parse_ieee(const char *s, uint64_t *out);
