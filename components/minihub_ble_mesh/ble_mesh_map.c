/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble_mesh_map.h"

#include <stdio.h>
#include <string.h>

const char *minihub_bm_classify_type(uint8_t caps)
{
    if (caps & (MINIHUB_BM_CAP_LIGHTNESS | MINIHUB_BM_CAP_CTL)) {
        return "light";
    }
    if (caps & MINIHUB_BM_CAP_LEVEL) {
        return "light";
    }
    if (caps & MINIHUB_BM_CAP_ONOFF) {
        return "switch";
    }
    return "switch";
}

int minihub_bm_format_point_id(char *out, size_t out_len, const char *type, uint16_t unicast)
{
    if (!out || out_len == 0 || !type || type[0] == '\0') {
        return -1;
    }
    int n = snprintf(out, out_len, "%s/bm_%04x", type, (unsigned)unicast);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return 0;
}

int minihub_bm_uuid_to_hex(const uint8_t uuid[16], char *out, size_t out_len)
{
    if (!uuid || !out || out_len < MINIHUB_BM_UUID_HEX_LEN) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", uuid[i]);
    }
    out[32] = '\0';
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int minihub_bm_hex_to_uuid(const char *hex, uint8_t uuid[16])
{
    if (!hex || !uuid) {
        return -1;
    }
    const char *p = hex;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    if (strlen(p) != 32) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        int hi = hex_nibble(p[i * 2]);
        int lo = hex_nibble(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        uuid[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
