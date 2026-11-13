/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_sensor.h"

#include "esp_log.h"

static const char *TAG = "bm_sensor";

/* Mesh Model 1.1 §1.5: all multiple-octet numeric values are little-endian. */
static uint32_t raw_le(const uint8_t *raw, size_t len)
{
    uint32_t v = 0;
    for (size_t i = 0; i < len && i < 4; i++) {
        v |= (uint32_t)raw[i] << (8 * i);
    }
    return v;
}

bool minihub_bm_mpid_next(const uint8_t *buf, size_t len, size_t *off, minihub_bm_mpid_t *out)
{
    if (!buf || !off || !out) {
        return false;
    }
    size_t i = *off;
    if (i >= len) {
        return false;
    }

    uint8_t format = buf[i] & 0x01u;
    uint16_t property_id = 0;
    uint8_t length_field = 0;
    size_t header = 0;
    if (format == 0) {
        /* Format A: 4-bit Length, 11-bit Property ID, 2 octets. */
        if (i + 2 > len) {
            return false;
        }
        length_field = (uint8_t)((buf[i] >> 1) & 0x0Fu);
        property_id = (uint16_t)((buf[i] >> 5) | ((uint16_t)buf[i + 1] << 3));
        header = 2;
    } else {
        /* Format B: 7-bit Length, 16-bit Property ID, 3 octets. */
        if (i + 3 > len) {
            return false;
        }
        length_field = (uint8_t)(buf[i] >> 1);
        property_id = (uint16_t)(buf[i + 1] | ((uint16_t)buf[i + 2] << 8));
        header = 3;
    }

    const minihub_bm_sensor_t *row = minihub_bm_sensor_by_property(property_id);
    size_t value_len;
    if (row && row->size) {
        value_len = row->size;
        /* The Length field is 1-based per Mesh Model 1.1; IDF writes the raw
         * length instead. Accept either, report anything else. */
        if (length_field + 1u != row->size && length_field != row->size) {
            ESP_LOGW(TAG, "property 0x%04x: MPID length %u does not match the table size %u",
                     (unsigned)property_id, (unsigned)length_field, (unsigned)row->size);
        }
    } else {
        /* Unknown property: the Length field is the only length available.
         * Mesh Model 1.1 makes it 1-based. */
        value_len = (size_t)length_field + 1u;
    }

    if (i + header + value_len > len) {
        ESP_LOGW(TAG, "property 0x%04x: value of %u octets does not fit the message",
                 (unsigned)property_id, (unsigned)value_len);
        return false;
    }

    out->property_id = property_id;
    out->data = buf + i + header;
    out->len = value_len;
    out->format = format;
    out->length_field = length_field;
    *off = i + header + value_len;
    return true;
}

bool minihub_bm_sensor_value(const minihub_bm_sensor_t *s, const uint8_t *raw, size_t len,
                             double *out)
{
    if (!s || !raw || !out) {
        return false;
    }
    if (!s->size || len < s->size) {
        ESP_LOGW(TAG, "parameter '%s': need %u octets, got %u", s->param, (unsigned)s->size,
                 (unsigned)len);
        return false;
    }

    uint32_t u = raw_le(raw, s->size);
    if (s->has_unknown && u == (uint32_t)s->unknown) {
        /* The property says "value is not known": report nothing. */
        return false;
    }

    double value;
    if (s->has_bool_threshold) {
        value = (u > (uint32_t)s->bool_threshold) ? 1.0 : 0.0;
    } else {
        int64_t v = (int64_t)u;
        if (s->is_signed) {
            if (s->size == 1) {
                v = (int8_t)u;
            } else if (s->size == 2) {
                v = (int16_t)u;
            } else if (s->size == 4) {
                v = (int32_t)u;
            }
        }
        value = (double)v * s->scale;
    }

    if (s->has_min && value < s->min) {
        value = s->min;
    }
    if (s->has_max && value > s->max) {
        value = s->max;
    }
    *out = value;
    return true;
}

size_t minihub_bm_descriptor_props(const uint8_t *buf, size_t len, uint16_t *ids, size_t max)
{
    if (!buf || !ids || max == 0) {
        return 0;
    }
    size_t count = 0;
    /* Mesh Model 1.1: the Sensor Descriptor is a sequence of 8-octet records,
     * each starting with the 2-octet property id. */
    for (size_t off = 0; off + 2 <= len && count < max; off += 8) {
        ids[count++] = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
    }
    return count;
}
