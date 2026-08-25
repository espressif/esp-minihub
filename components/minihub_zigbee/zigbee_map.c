/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_map.h"
#include "minihub_zigbee_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Zigbee HA profile application device IDs (lighting). */
#define ZB_HA_ON_OFF_LIGHT            0x0100
#define ZB_HA_DIMMABLE_LIGHT          0x0101
#define ZB_HA_COLOR_DIMMABLE_LIGHT    0x0102
#define ZB_HA_COLOR_TEMPERATURE_LIGHT 0x010C
#define ZB_HA_EXTENDED_COLOR_LIGHT    0x010D

static bool is_ha_light_device(uint16_t app_device_id)
{
    switch (app_device_id) {
    case ZB_HA_ON_OFF_LIGHT:
    case ZB_HA_DIMMABLE_LIGHT:
    case ZB_HA_COLOR_DIMMABLE_LIGHT:
    case ZB_HA_COLOR_TEMPERATURE_LIGHT:
    case ZB_HA_EXTENDED_COLOR_LIGHT:
        return true;
    default:
        return false;
    }
}

const char *minihub_zb_classify_type(uint8_t caps, uint16_t app_device_id)
{
    if (caps & MINIHUB_ZB_CAP_SENSOR_MASK) {
        return "sensor";
    }
    if (is_ha_light_device(app_device_id)) {
        return "light";
    }
    if (caps & MINIHUB_ZB_CAP_ONOFF) {
        return "switch";
    }
    /* Interview incomplete — never invent a switch/light. */
    return "unknown";
}

int minihub_zb_format_point_id(char *out, size_t out_len, const char *type, uint16_t short_addr)
{
    if (!out || out_len == 0 || !type || type[0] == '\0') {
        return -1;
    }
    int n = snprintf(out, out_len, "%s/zb_%04x", type, (unsigned)short_addr);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return 0;
}

int minihub_zb_parse_ieee(const char *s, uint64_t *out)
{
    if (!s || !out || s[0] == '\0') {
        return -1;
    }
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 16);
    if (!end || end == p || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}
