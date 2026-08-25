/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Reject zip-slip / absolute / empty / overlong paths. */
bool minihub_hub_zip_path_ok(const char *name);

/**
 * Extract a ZIP (store or raw deflate) from memory into dest_dir.
 * Creates dest_dir if needed. Does not delete existing contents.
 */
esp_err_t minihub_hub_zip_extract(const uint8_t *zip, size_t zip_len, const char *dest_dir);

/** Recursively remove a directory tree (ENOENT is OK). */
esp_err_t minihub_hub_remove_tree(const char *path);

#ifdef __cplusplus
}
#endif
