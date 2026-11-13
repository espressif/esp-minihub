/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_mesh_map.h"

/** One Marshalled Property ID (MPID) record of a Sensor Status message. */
typedef struct {
    uint16_t property_id;
    const uint8_t *data;  /* raw value octets, inside the caller's buffer */
    size_t len;           /* octet count of the raw value */
    uint8_t format;       /* 0 = Format A, 1 = Format B */
    uint8_t length_field; /* Length field the MPID carried */
} minihub_bm_mpid_t;

/**
 * Decode the next MPID record of a Sensor Status message.
 *
 * The value length comes from the map table (the size the SIG gives the
 * property), not from the MPID Length field: Mesh Model 1.1 makes that field
 * 1-based, while IDF encodes the raw length as-is, so the two disagree on
 * every single-octet property. The field is cross-checked and a mismatch is
 * logged. @p off advances past the record.
 *
 * A property the table does not know is skipped with the Length field, and
 * the caller sees it in out->property_id.
 */
bool minihub_bm_mpid_next(const uint8_t *buf, size_t len, size_t *off, minihub_bm_mpid_t *out);

/**
 * Decode one raw sensor value: unknown sentinel, boolean threshold, scale and
 * the catalog bounds. False when the value must not be reported (unknown
 * sentinel, or fewer octets than the table's size).
 */
bool minihub_bm_sensor_value(const minihub_bm_sensor_t *s, const uint8_t *raw, size_t len,
                             double *out);

/**
 * Property ids of a Sensor Descriptor Status payload: a sequence of 8-octet
 * records whose first 2 octets are the property id (little-endian).
 * Returns how many ids were written.
 */
size_t minihub_bm_descriptor_props(const uint8_t *buf, size_t len, uint16_t *ids, size_t max);
