/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MINIHUB_UPDATE_FIRMWARE, /* GET /api/firmware, then minihub_ota_start_url() */
    MINIHUB_UPDATE_ASSET,    /* GET /api/assets → model and images */
    MINIHUB_UPDATE_WEB,      /* GET /api/web → /data/web */
    MINIHUB_UPDATE_UI,       /* GET /api/ui → /data/ui (LCD Brookesia) */
} minihub_update_kind_t;

/**
 * @brief Compare the deploy server at `base_url` (origin, no trailing path) with
 * what is already on the device.
 *
 * Returns 0 when nothing is newer, a positive count of pending items, or a
 * negative value on failure (`-(int)esp_err_t`; `ESP_FAIL` is already
 * negative). Firmware counts as one item: the highest version whose
 * `targets` include this chip (or `*`). Asset and web items compare
 * size and sha256 of the local file.
 */
int minihub_update_check(minihub_update_kind_t kind, const char *base_url);

/**
 * @brief Download and apply whatever minihub_update_check() would report.
 * Firmware hands the bin URL to `minihub_ota_start_url()` and returns
 * without waiting for the reboot. Asset `devices.csv` / `parameters.csv`
 * go through `minihub_device_widget_put_model()`. Other model files land
 * in `/data/config/model`, images in `/data/images`, web files in
 * `/data/web`.
 *
 * Returns 0 on success, or a negative value on failure (same encoding as
 * `minihub_update_check`).
 */
int minihub_update_apply(minihub_update_kind_t kind, const char *base_url);

/**
 * @brief Like minihub_update_apply(), but re-fetches every selected item even when the
 * local file already matches (same-version re-install / repair). File-based
 * items are streamed to a same-directory "<dest>.part" temp file and only
 * renamed over the existing file after the sha256/size check passes, so an
 * interrupted download never corrupts the previously installed file. Firmware
 * re-runs OTA even when the running version already matches.
 */
int minihub_update_apply_force(minihub_update_kind_t kind, const char *base_url, bool force);

/**
 * @brief After the network has an address, compare deploy firmware and installed
 * plugin versions once, then again every hour. Records a hint list only.
 * Does not start OTA or install plugins. Uses the store deploy URL.
 */
void minihub_update_hint_start(void);

/**
 * @brief Last hint list. Object ``{ "firmware": null|{version,current,note},
 * "plugins": [...], "web": null|{version,current,note},
 * "assets": null|{pending} }``. Caller deletes. Empty when nothing is pending.
 */
cJSON *minihub_update_hints_json(void);

/**
 * @brief Ask the resident hint task to re-check firmware, plugins, web and device
 * assets now. Non-blocking: it never runs the probes on the caller's task (they
 * would block an httpd handler for the full select() timeout against an
 * unreachable deploy server) and it never spawns a second task, so the hint
 * cache keeps a single writer. A request arriving during a probe just queues
 * one more pass. Returns ESP_OK if the hint subsystem is running,
 * ESP_ERR_INVALID_STATE if it never started. Results land in the cache
 * GET /api/update/hints reads; poll minihub_update_hints_refresh_status() to
 * learn when the probe finished.
 */
esp_err_t minihub_update_hints_refresh_async(void);

/**
 * @brief Report whether a hint probe is running.
 *
 * `*busy` is true while a probe is running, or one was requested and not yet
 * served (for example, there is still no IP, so the probes cannot run).
 */
void minihub_update_hints_refresh_status(bool *busy);

/**
 * @brief Local versions and release notes for the console.
 * Object `{firmware_version, firmware_note, web_version, web_note}`.
 * Strings are empty when that piece is not on the device yet. Caller deletes.
 * Does not contact the deploy server.
 */
cJSON *minihub_update_notes_json(void);

/**
 * @brief Fetch one device type's avatar straight from the deploy root
 * (`GET <base>/files/Assets/images/devices/<type>.svg`) into
 * /data/images/devices/<type>.svg -- the path GET /img/ serves.
 *
 * The name is derived from the type by convention, so no metadata request is
 * needed; this also covers a plugin-owned type that the shared model does not
 * know. `base_url` may be NULL to use the configured store URL.
 *
 * A type with no artwork is HTTP 404 (``ESP_ERR_NOT_FOUND``) and leaves the tree
 * untouched. An icon that is already present (non-empty regular file) is NOT
 * fetched again -- so replacing the artwork on the server does not reach
 * devices that already have it; delete the file to force a refresh.
 */
esp_err_t minihub_update_fetch_device_icon(const char *base_url, const char *type);

#ifdef __cplusplus
}
#endif
