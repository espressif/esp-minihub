/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "cJSON.h"

/**
 * @brief Load catalog for UI/install (caller cJSON_Delete).
 * Prefers /store/catalog.json; only hits the LAN store server if local is missing.
 */
cJSON *minihub_store_fetch_catalog(void);

/**
 * @brief Force-fetch catalog from the store server and save to /store/catalog.json.
 * Does not fall back to the bundled copy on failure. Use from explicit refresh.
 */
esp_err_t minihub_store_refresh_catalog(void);

/**
 * @brief Start a catalog refresh on a background task.
 * Returns immediately so the HTTP server can still accept a new store URL
 * while a bad URL is timing out. ESP_ERR_INVALID_STATE if one is already running.
 */
esp_err_t minihub_store_refresh_catalog_async(void);

/**
 * @brief Snapshot of the background refresh.
 *
 * `finished` is false until the first run completes. `aborted` is true when the
 * last run stopped because the store config changed.
 */
void minihub_store_refresh_catalog_status(bool *busy, bool *finished, esp_err_t *result,
                                          bool *aborted);

/**
 * @brief Download and install a store item.
 * @param kind  "module" or "pack".
 * @param id    catalog item id.
 */
esp_err_t minihub_store_install(const char *kind, const char *id);

/**
 * @brief Remove the installed module binary and manifest, or the pack file.
 *
 * Also drops the `adapters.json` slot (account and device config).
 */
esp_err_t minihub_store_uninstall(const char *kind, const char *id);
/**
 * @brief Factory reset: remove all Store-installed modules and packs (preinstall preserved).
 */
esp_err_t minihub_store_factory_reset(void);

/**
 * @brief Upgrade an installed store item to the catalog version.
 * Fails with ESP_ERR_INVALID_STATE if not installed or already latest.
 */
esp_err_t minihub_store_upgrade(const char *kind, const char *id);

/**
 * @brief Replace the installed package from the catalog even when the version matches.
 * Keeps the adapters.json slot (account, devices, enabled). Does not uninstall.
 */
esp_err_t minihub_store_reinstall(const char *kind, const char *id);

/**
 * @brief Merge remote catalog with locally installed items (caller cJSON_Delete).
 */
cJSON *minihub_store_catalog_merged(void);

/**
 * @brief True when every adapter in the catalog entry's requires list is enabled.
 *        A missing or empty list matches every Hub.
 */
bool minihub_store_catalog_requires_met(const cJSON *entry);

/**
 * @brief Runtime store server config (NVS).
 * @param custom_url      out buffer for custom base URL (may be empty).
 * @param custom_url_len  size of custom_url.
 * @param disable_default out: true if firmware default URL is disabled.
 * @param default_url     optional out for Kconfig default base URL.
 * @param default_url_len size of default_url (0 to skip).
 */
esp_err_t minihub_store_get_config(char *custom_url, size_t custom_url_len, bool *disable_default,
                                   char *default_url, size_t default_url_len);

/**
 * @brief Persist custom store URL and whether to disable the firmware default.
 * @param custom_url      NULL or "" clears custom URL; otherwise full base e.g. http://host:8080.
 * @param disable_default if true, only custom URL is used (requires non-empty custom_url).
 */
esp_err_t minihub_store_set_config(const char *custom_url, bool disable_default);

/**
 * @brief Effective deploy origin: custom URL when set, otherwise the Kconfig
 * default, unless that default is disabled. Empty when nothing is configured.
 */
bool minihub_store_copy_base_url(char *out, size_t out_len);

/**
 * @brief Installed plugins whose remote catalog version is newer.
 * `*out` is a JSON array of `{id,name,kind,installed,version}` on
 * success (caller deletes). Does not download or install.
 * ESP_ERR_INVALID_STATE when no deploy URL is configured.
 */
esp_err_t minihub_store_list_upgrades(cJSON **out);

/**
 * @brief littlefs usage of a mounted partition (the numbers the Web console
 *        shows in its Storage / Modstore cards).
 * @param label  Partition label, e.g. "storage" or "modstore".
 * @param total  Receives the partition size in bytes.
 * @param used   Receives the used bytes in bytes.
 * @return
 *     - ESP_OK: Succeed.
 *     - ESP_ERR_INVALID_ARG: NULL argument.
 *     - ESP_ERR_NOT_FOUND: Partition not mounted.
 */
esp_err_t minihub_store_partition_usage(const char *label, size_t *total, size_t *used);
