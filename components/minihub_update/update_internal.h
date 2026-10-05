/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "minihub_update.h"

#define MH_UPDATE_ITEM_MAX  192
#define MH_UPDATE_INDEX_MAX (256 * 1024)
#define MH_UPDATE_URL_MAX   512

typedef struct {
    char *id;
    char *name;
    char *version;
    char *url;
    char *sha256;
    char *type;
    char *release_note;
    char **targets;
    size_t ntargets;
    size_t size;
} mh_update_item_t;

typedef struct {
    char *schema;
    char *kind;
    char *base_url;
    mh_update_item_t *items;
    size_t nitems;
} mh_update_index_t;

void mh_update_index_free(mh_update_index_t *idx);
esp_err_t mh_update_parse_document(const char *json, size_t len, bool firmware_array,
                                   mh_update_index_t *out);

esp_err_t mh_update_http_get(const char *url, char **body, size_t *len, size_t max_len);
esp_err_t mh_update_download_verified(const char *url, const char *dest,
                                      const char *expect_sha, size_t expect_size);
esp_err_t mh_update_sha256_file(const char *path, char hex[65], size_t *size_out);
bool mh_update_target_match(const mh_update_item_t *item, const char *chip);
int mh_update_version_cmp(const char *a, const char *b);
esp_err_t mh_update_join_url(char *out, size_t out_len, const char *origin,
                             const char *index_base, const char *rel);
bool mh_update_rel_ok(const char *rel);
