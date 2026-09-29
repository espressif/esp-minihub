/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "cJSON.h"

/**
 * Load catalog for UI/install (caller cJSON_Delete).
 * Prefers /store/catalog.json; only hits the LAN store server if local is missing.
 */
cJSON *minihub_store_fetch_catalog(void);

/**
 * Force-fetch catalog from the store server and save to /store/catalog.json.
 * Does not fall back to the bundled copy on failure. Use from explicit refresh.
 */
esp_err_t minihub_store_refresh_catalog(void);

/**
 * Download and install a store item.
 * @param kind  "module" or "pack"
 * @param id    catalog item id
 */
esp_err_t minihub_store_install(const char *kind, const char *id);

/** Remove installed module binary + manifest, or pack file from modstore/config.
 * Also drops the adapters.json slot (account and device config). */
esp_err_t minihub_store_uninstall(const char *kind, const char *id);

/**
 * Upgrade an installed store item to the catalog version.
 * Fails with ESP_ERR_INVALID_STATE if not installed or already latest.
 */
esp_err_t minihub_store_upgrade(const char *kind, const char *id);

/**
 * Replace the installed package from the catalog even when the version matches.
 * Keeps the adapters.json slot (account, devices, enabled). Does not uninstall.
 */
esp_err_t minihub_store_reinstall(const char *kind, const char *id);

/** Merge remote catalog with locally installed items (caller cJSON_Delete). */
cJSON *minihub_store_catalog_merged(void);

/**
 * Runtime store server config (NVS).
 * @param custom_url      out buffer for custom base URL (may be empty)
 * @param custom_url_len  size of custom_url
 * @param disable_default out: true if firmware default URL is disabled
 * @param default_url     optional out for Kconfig default base URL
 * @param default_url_len size of default_url (0 to skip)
 */
esp_err_t minihub_store_get_config(char *custom_url, size_t custom_url_len, bool *disable_default,
                                   char *default_url, size_t default_url_len);

/**
 * Persist custom store URL and whether to disable the firmware default.
 * @param custom_url      NULL or "" clears custom URL; otherwise full base e.g. http://host:8080
 * @param disable_default if true, only custom URL is used (requires non-empty custom_url)
 */
esp_err_t minihub_store_set_config(const char *custom_url, bool disable_default);
