/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_map.h"
#include "zigbee_cluster_map.h"
#include "minihub_zigbee_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *minihub_zb_classify_type(uint8_t caps, uint16_t app_device_id)
{
    (void)minihub_zb_cluster_map_init();
    return minihub_zb_map_classify_type(caps, app_device_id);
}

int minihub_zb_format_point_id(char *out, size_t out_len, uint16_t short_addr)
{
    if (!out || out_len == 0) {
        return -1;
    }
    int n = snprintf(out, out_len, "zigbee.zb_%04x", (unsigned)short_addr);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return 0;
}

bool minihub_zb_normalize_point_id(char *point_id, size_t len)
{
    if (!point_id || len == 0 || point_id[0] == '\0') {
        return false;
    }
    if (strncmp(point_id, "zigbee.", 7) == 0) {
        return false;
    }
    /* Legacy discovery form: "{type}/zb_XXXX" */
    const char *slash = strchr(point_id, '/');
    if (!slash || slash[1] != 'z' || slash[2] != 'b' || slash[3] != '_') {
        return false;
    }
    const char *stable = slash + 1;
    char buf[MINIHUB_POINT_ID_MAX];
    int n = snprintf(buf, sizeof(buf), "zigbee.%s", stable);
    if (n < 0 || (size_t)n >= len || (size_t)n >= sizeof(buf)) {
        return false;
    }
    if (strcmp(point_id, buf) == 0) {
        return false;
    }
    strncpy(point_id, buf, len - 1);
    point_id[len - 1] = '\0';
    return true;
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
