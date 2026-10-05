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
    cJSON *size = cJSON_GetObjectItem(obj, "size");
    if (!cJSON_IsString(url) || !url->valuestring) {
        return ESP_ERR_INVALID_ARG;
    }
    item->url = dup_str(url->valuestring);
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
    if (cJSON_IsString(sha) && sha->valuestring) {
        item->sha256 = dup_str(sha->valuestring);
    }
    if (cJSON_IsString(type) && type->valuestring) {
        item->type = dup_str(type->valuestring);
    }
    if (cJSON_IsString(note) && note->valuestring) {
        item->release_note = dup_str(note->valuestring);
    }
    if (cJSON_IsNumber(size) && size->valuedouble >= 0) {
        item->size = (size_t)size->valuedouble;
    }
    if (!item->id || !item->url) {
        return ESP_ERR_NO_MEM;
    }
    return parse_targets(cJSON_GetObjectItem(obj, "targets"), item);
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

int mh_update_version_cmp(const char *a, const char *b)
{
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";
    while (*pa || *pb) {
        long na = 0;
        long nb = 0;
        bool da = false;
        bool db = false;
        if (*pa == '.') {
            pa++;
        }
        if (*pb == '.') {
            pb++;
        }
        while (*pa >= '0' && *pa <= '9') {
            da = true;
            na = na * 10 + (*pa - '0');
            pa++;
        }
        while (*pb >= '0' && *pb <= '9') {
            db = true;
            nb = nb * 10 + (*pb - '0');
            pb++;
        }
        if (da || db) {
            if (na != nb) {
                return (na > nb) ? 1 : -1;
            }
            continue;
        }
        if (*pa != *pb) {
            if (*pa == '\0') {
                return -1;
            }
            if (*pb == '\0') {
                return 1;
            }
            return (*pa > *pb) ? 1 : -1;
        }
        if (*pa == '\0') {
            return 0;
        }
        pa++;
        pb++;
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
