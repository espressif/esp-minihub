/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "update_internal.h"

static char *dup_str(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out) {
        memcpy(out, s, n);
    }
    return out;
}

static void free_item(mh_update_item_t *item)
{
    if (!item) {
        return;
    }
    free(item->id);
    free(item->name);
    free(item->version);
    free(item->url);
    free(item->sha256);
    free(item->type);
    free(item->release_note);
    for (size_t i = 0; i < item->ntargets; i++) {
        free(item->targets[i]);
    }
    free(item->targets);
    for (size_t i = 0; i < item->nboards; i++) {
        free(item->boards[i]);
    }
    free(item->boards);
    memset(item, 0, sizeof(*item));
}

void mh_update_index_free(mh_update_index_t *idx)
{
    if (!idx) {
        return;
    }
    free(idx->schema);
    free(idx->kind);
    free(idx->base_url);
    for (size_t i = 0; i < idx->nitems; i++) {
        free_item(&idx->items[i]);
    }
    free(idx->items);
    memset(idx, 0, sizeof(*idx));
}

static esp_err_t parse_targets(cJSON *node, mh_update_item_t *item)
{
    if (!node || cJSON_IsNull(node)) {
        return ESP_OK;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        item->targets = calloc(1, sizeof(char *));
        if (!item->targets) {
            return ESP_ERR_NO_MEM;
        }
        item->targets[0] = dup_str(node->valuestring);
        if (!item->targets[0]) {
            return ESP_ERR_NO_MEM;
        }
        item->ntargets = 1;
        return ESP_OK;
    }
    if (!cJSON_IsArray(node)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = cJSON_GetArraySize(node);
    if (n < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    item->targets = calloc((size_t)n, sizeof(char *));
    if (n > 0 && !item->targets) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *el = NULL;
    cJSON_ArrayForEach(el, node) {
        if (!cJSON_IsString(el) || !el->valuestring) {
            return ESP_ERR_INVALID_ARG;
        }
        item->targets[item->ntargets] = dup_str(el->valuestring);
        if (!item->targets[item->ntargets]) {
            return ESP_ERR_NO_MEM;
        }
        item->ntargets++;
    }
    return ESP_OK;
}

/** True when a flash part is not a Hub application OTA image. */
static bool path_is_flash_sidecar(const char *path)
{
    if (!path || !path[0]) {
        return true;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    static const char *const kSkip[] = {
        "bootloader", "partition", "merged", "storage", "modstore",
        "ota_data", "otadata", "nvs",
    };
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); i++) {
        if (strcasestr(base, kSkip[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/**
 * Portal GET /api/v1/firmware lists flash plans. Prefer an explicit top-level
 * OTA url; otherwise take a non-merged plan part that looks like an app image.
 * Never pick merged.bin — that is a full-flash image, not an OTA payload.
 */
static esp_err_t fill_url_from_plans(cJSON *plans, mh_update_item_t *item)
{
    if (!cJSON_IsArray(plans)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *plan = NULL;
    cJSON_ArrayForEach(plan, plans) {
        if (!cJSON_IsObject(plan)) {
            continue;
        }
        cJSON *kind = cJSON_GetObjectItem(plan, "kind");
        if (cJSON_IsString(kind) && kind->valuestring &&
            strcasecmp(kind->valuestring, "merged") == 0) {
            continue;
        }
        cJSON *parts = cJSON_GetObjectItem(plan, "parts");
        if (!cJSON_IsArray(parts)) {
            continue;
        }
        cJSON *part = NULL;
        cJSON_ArrayForEach(part, parts) {
            if (!cJSON_IsObject(part)) {
                continue;
            }
            cJSON *purl = cJSON_GetObjectItem(part, "url");
            if (!cJSON_IsString(purl) || !purl->valuestring ||
                path_is_flash_sidecar(purl->valuestring)) {
                continue;
            }
            item->url = dup_str(purl->valuestring);
            if (!item->url) {
                return ESP_ERR_NO_MEM;
            }
            cJSON *sha = cJSON_GetObjectItem(part, "sha256");
            if (cJSON_IsString(sha) && sha->valuestring && !item->sha256) {
                item->sha256 = dup_str(sha->valuestring);
            }
            cJSON *size = cJSON_GetObjectItem(part, "size");
            if (cJSON_IsNumber(size) && size->valuedouble >= 0 && item->size == 0) {
                item->size = (size_t)size->valuedouble;
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t parse_item(cJSON *obj, mh_update_item_t *item)
{
    if (!cJSON_IsObject(obj)) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *id = cJSON_GetObjectItem(obj, "id");
    cJSON *name = cJSON_GetObjectItem(obj, "name");
    cJSON *version = cJSON_GetObjectItem(obj, "version");
    cJSON *url = cJSON_GetObjectItem(obj, "url");
    cJSON *sha = cJSON_GetObjectItem(obj, "sha256");
    cJSON *type = cJSON_GetObjectItem(obj, "type");
    cJSON *note = cJSON_GetObjectItem(obj, "release_note");
    if (!note || cJSON_IsNull(note)) {
        note = cJSON_GetObjectItem(obj, "release_notes");
    }
    cJSON *size = cJSON_GetObjectItem(obj, "size");
    if (cJSON_IsString(url) && url->valuestring) {
        item->url = dup_str(url->valuestring);
    } else {
        /* Portal v1: url lives under plans[].parts[]. */
        esp_err_t perr = fill_url_from_plans(cJSON_GetObjectItem(obj, "plans"), item);
        if (perr != ESP_OK) {
            return perr == ESP_ERR_NO_MEM ? perr : ESP_ERR_INVALID_ARG;
        }
    }
    if (!item->url) {
        return ESP_ERR_INVALID_ARG;
    }
    /* "id" is dead weight (only ever freed) and the asset manifest now omits
     * it; fall back to the url so a compact entry still parses. Older firmware
     * rejects such an item outright, which is why the server still sends it. */
    item->id = (cJSON_IsString(id) && id->valuestring) ? dup_str(id->valuestring)
                                                       : dup_str(item->url);
    if (cJSON_IsString(name) && name->valuestring) {
        item->name = dup_str(name->valuestring);
    }
    if (cJSON_IsString(version) && version->valuestring) {
        item->version = dup_str(version->valuestring);
    }
    if (cJSON_IsString(sha) && sha->valuestring && !item->sha256) {
        item->sha256 = dup_str(sha->valuestring);
    }
    if (cJSON_IsString(type) && type->valuestring) {
        item->type = dup_str(type->valuestring);
    }
    if (cJSON_IsString(note) && note->valuestring) {
        item->release_note = dup_str(note->valuestring);
    }
    if (cJSON_IsNumber(size) && size->valuedouble >= 0 && item->size == 0) {
        item->size = (size_t)size->valuedouble;
    }
    if (!item->id || !item->url) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = parse_targets(cJSON_GetObjectItem(obj, "targets"), item);
    if (err != ESP_OK) {
        return err;
    }
    /* Portal v1 uses singular "chip" instead of targets[]. */
    if (item->ntargets == 0) {
        err = parse_targets(cJSON_GetObjectItem(obj, "chip"), item);
        if (err != ESP_OK) {
            return err;
        }
    }
    /* Optional boards allowlist: "boards": [...] or singular "board": "...".
     * Reuse the targets parser into a temporary slot, then move. */
    char **saved_targets = item->targets;
    size_t saved_ntargets = item->ntargets;
    item->targets = NULL;
    item->ntargets = 0;
    cJSON *boards = cJSON_GetObjectItem(obj, "boards");
    if (!boards || cJSON_IsNull(boards)) {
        boards = cJSON_GetObjectItem(obj, "board");
    }
    err = parse_targets(boards, item);
    if (err != ESP_OK) {
        item->targets = saved_targets;
        item->ntargets = saved_ntargets;
        return err;
    }
    item->boards = item->targets;
    item->nboards = item->ntargets;
    item->targets = saved_targets;
    item->ntargets = saved_ntargets;
    return ESP_OK;
}

static esp_err_t parse_items(cJSON *arr, mh_update_index_t *out)
{
    if (!cJSON_IsArray(arr)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 0 || n > MH_UPDATE_ITEM_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (n == 0) {
        out->items = NULL;
        out->nitems = 0;
        return ESP_OK;
    }
    out->items = calloc((size_t)n, sizeof(mh_update_item_t));
    if (!out->items) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *el = NULL;
    cJSON_ArrayForEach(el, arr) {
        esp_err_t err = parse_item(el, &out->items[out->nitems]);
        if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_FOUND) {
            /* Skip flash-only portal rows (merged plan, no OTA url). */
            free_item(&out->items[out->nitems]);
            continue;
        }
        out->nitems++;
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t mh_update_parse_document(const char *json, size_t len, bool firmware_array,
                                   mh_update_index_t *out)
{
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    if (firmware_array) {
        out->kind = dup_str("firmware");
        err = parse_items(root, out);
    } else {
        cJSON *schema = cJSON_GetObjectItem(root, "schema");
        cJSON *kind = cJSON_GetObjectItem(root, "kind");
        cJSON *base = cJSON_GetObjectItem(root, "base_url");
        if (!cJSON_IsString(schema) || !schema->valuestring ||
            strncmp(schema->valuestring, "minihub.deploy.index/", 21) != 0 ||
            !cJSON_IsString(kind) || !kind->valuestring) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            out->schema = dup_str(schema->valuestring);
            out->kind = dup_str(kind->valuestring);
            if (cJSON_IsString(base) && base->valuestring) {
                out->base_url = dup_str(base->valuestring);
            }
            if (!out->schema || !out->kind) {
                err = ESP_ERR_NO_MEM;
            } else {
                err = parse_items(cJSON_GetObjectItem(root, "items"), out);
            }
        }
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        mh_update_index_free(out);
    }
    return err;
}

bool mh_update_target_match(const mh_update_item_t *item, const char *chip)
{
    if (!item) {
        return false;
    }
    if (item->ntargets == 0) {
        return true;
    }
    for (size_t i = 0; i < item->ntargets; i++) {
        const char *t = item->targets[i];
        if (!t) {
            continue;
        }
        if (strcmp(t, "*") == 0) {
            return true;
        }
        if (chip && strcasecmp(t, chip) == 0) {
            return true;
        }
    }
    return false;
}

bool mh_update_board_match(const mh_update_item_t *item, const char *board)
{
    if (!item) {
        return false;
    }
    /* No boards/board field → chip-wide legacy item; accept any board. The
     * portal must omit such items when the request carries board=. */
    if (item->nboards == 0) {
        return true;
    }
    for (size_t i = 0; i < item->nboards; i++) {
        const char *b = item->boards[i];
        if (!b) {
            continue;
        }
        if (strcmp(b, "*") == 0 || strcmp(b, "ALL") == 0 || strcmp(b, "all") == 0) {
            return true;
        }
        if (board && strcmp(b, board) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Prerelease channel rank (higher = newer). Release (no suffix) beats every
 * channel: 0.1.0 > 0.1.0-pre > 0.1.0-beta > 0.1.0-dev. Within a channel the
 * bare tag is older than a numbered one (pre < pre1 < pre2).
 */
static int mh_prerelease_rank(const char *name, size_t len)
{
    if (len == 3 && strncasecmp(name, "pre", 3) == 0) {
        return 3;
    }
    if (len == 4 && strncasecmp(name, "beta", 4) == 0) {
        return 2;
    }
    if (len == 3 && strncasecmp(name, "dev", 3) == 0) {
        return 1;
    }
    return 0; /* unknown channel — below the known ones */
}

/** Parse "-pre2" / "_beta" / "+build" leftover after the numeric core. */
static void mh_parse_prerelease(const char *s, int *rank, long *num)
{
    *rank = 4; /* plain release */
    *num = 0;
    if (!s || !*s || *s == '+') {
        return;
    }
    if (*s == '-' || *s == '_' || *s == '.') {
        s++;
    }
    if (!*s || *s == '+') {
        return;
    }
    const char *start = s;
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')) {
        s++;
    }
    size_t len = (size_t)(s - start);
    if (len == 0) {
        *rank = 0;
        return;
    }
    *rank = mh_prerelease_rank(start, len);
    long n = 0;
    bool has = false;
    while (*s >= '0' && *s <= '9') {
        has = true;
        n = n * 10 + (*s - '0');
        s++;
    }
    *num = has ? n : 0;
}

int mh_update_version_cmp(const char *a, const char *b)
{
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";
    /* Same leading-v rule as ver_norm(): "v0.1.1" must not rank below "0.1.0". */
    if ((pa[0] == 'v' || pa[0] == 'V') && pa[1] >= '0' && pa[1] <= '9') {
        pa++;
    }
    if ((pb[0] == 'v' || pb[0] == 'V') && pb[1] >= '0' && pb[1] <= '9') {
        pb++;
    }

    for (;;) {
        while (*pa == '.') {
            pa++;
        }
        while (*pb == '.') {
            pb++;
        }
        bool dig_a = (*pa >= '0' && *pa <= '9');
        bool dig_b = (*pb >= '0' && *pb <= '9');
        if (!dig_a && !dig_b) {
            break;
        }
        long na = 0;
        long nb = 0;
        while (*pa >= '0' && *pa <= '9') {
            na = na * 10 + (*pa - '0');
            pa++;
        }
        while (*pb >= '0' && *pb <= '9') {
            nb = nb * 10 + (*pb - '0');
            pb++;
        }
        if (na != nb) {
            return (na > nb) ? 1 : -1;
        }
    }

    int ra = 0;
    int rb = 0;
    long na = 0;
    long nb = 0;
    mh_parse_prerelease(pa, &ra, &na);
    mh_parse_prerelease(pb, &rb, &nb);
    if (ra != rb) {
        return (ra > rb) ? 1 : -1;
    }
    if (na != nb) {
        return (na > nb) ? 1 : -1;
    }
    return 0;
}

esp_err_t mh_update_join_url(char *out, size_t out_len, const char *origin,
                             const char *index_base, const char *rel)
{
    if (!out || out_len == 0 || !origin || !origin[0] || !rel || !rel[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        size_t n = strlen(rel);
        if (n + 1 > out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, rel, n + 1);
        return ESP_OK;
    }

    char base[MH_UPDATE_URL_MAX];
    const char *prefix = origin;
    if (rel[0] != '/' && index_base && index_base[0]) {
        if (strncmp(index_base, "http://", 7) == 0 || strncmp(index_base, "https://", 8) == 0) {
            prefix = index_base;
        } else {
            size_t ol = strlen(origin);
            while (ol > 0 && origin[ol - 1] == '/') {
                ol--;
            }
            const char *ib = index_base;
            while (*ib == '/') {
                ib++;
            }
            int wrote = snprintf(base, sizeof(base), "%.*s/%s", (int)ol, origin, ib);
            if (wrote < 0 || (size_t)wrote >= sizeof(base)) {
                return ESP_ERR_INVALID_SIZE;
            }
            prefix = base;
        }
    }

    size_t pl = strlen(prefix);
    while (pl > 0 && prefix[pl - 1] == '/') {
        pl--;
    }
    const char *rr = rel;
    while (*rr == '/') {
        rr++;
    }
    int wrote = snprintf(out, out_len, "%.*s/%s", (int)pl, prefix, rr);
    if (wrote < 0 || (size_t)wrote >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bool mh_update_rel_ok(const char *rel)
{
    if (!rel || !rel[0] || rel[0] == '/' || rel[0] == '\\') {
        return false;
    }
    for (const char *p = rel; *p; p++) {
        if (*p == '\\') {
            return false;
        }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return false;
        }
    }
    return true;
}
