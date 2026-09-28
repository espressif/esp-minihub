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

int minihub_bm_format_point_id(char *out, size_t out_len, uint16_t unicast)
{
    if (!out || out_len == 0) {
        return -1;
    }
    int n = snprintf(out, out_len, "ble_mesh.bm_%04x", (unsigned)unicast);
    if (n < 0 || (size_t)n >= out_len) {
        return -1;
    }
    return 0;
}

const char *minihub_bm_stable_suffix(const char *point_id)
{
    if (!point_id || point_id[0] == '\0') {
        return NULL;
    }
    if (strncmp(point_id, "ble_mesh.", 9) == 0) {
        const char *s = point_id + 9;
        return (s[0] == 'b' && s[1] == 'm' && s[2] == '_') ? s : NULL;
    }
    const char *slash = strrchr(point_id, '/');
    if (slash && slash[1] == 'b' && slash[2] == 'm' && slash[3] == '_') {
        return slash + 1;
    }
    if (point_id[0] == 'b' && point_id[1] == 'm' && point_id[2] == '_') {
        return point_id;
    }
    return NULL;
}

bool minihub_bm_normalize_point_id(char *point_id, size_t len)
{
    if (!point_id || len == 0 || point_id[0] == '\0') {
        return false;
    }
    if (strncmp(point_id, "ble_mesh.", 9) == 0) {
        return false;
    }
    /* Legacy discovery form: "{type}/bm_XXXX" */
    const char *slash = strchr(point_id, '/');
    if (!slash || slash[1] != 'b' || slash[2] != 'm' || slash[3] != '_') {
        return false;
    }
    const char *stable = slash + 1;
    char buf[MINIHUB_POINT_ID_MAX];
    int n = snprintf(buf, sizeof(buf), "ble_mesh.%s", stable);
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
