/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** LittleFS path (relative to /data) written by minihub_web_fetch_map_snapshot(). */
#define MINIHUB_MAP_SNAPSHOT_REL "ui/cache/map.png"
/** Absolute VFS path (PNG snapshot for LVGL file source). */
#define MINIHUB_MAP_SNAPSHOT_ABS "/data/ui/cache/map.png"

/** Optional hooks (weak); LCD UI may pause LVGL to free memory during tile download. */
void minihub_map_download_begin(void);
void minihub_map_download_end(void);

/** True when cache file is missing or hub lat/lon moved beyond ~2 km. */
bool minihub_web_map_snapshot_stale(double lat, double lon);

/** Like stale(), but also respects backoff after repeated download failures. */
bool minihub_web_map_snapshot_should_fetch(double lat, double lon);

/**
 * Download a static OpenStreetMap PNG centered on Hub (lat, lon).
 * Runs HTTPS on the module HTTP worker task (safe stack size). Blocks until complete.
 */
esp_err_t minihub_web_fetch_map_snapshot(double lat, double lon, int width_px, int height_px);

typedef void (*minihub_map_snapshot_done_cb_t)(esp_err_t err, void *user_ctx);

/**
 * Same as minihub_web_fetch_map_snapshot(), but returns immediately.
 * Callback runs on the HTTP worker task when the download finishes.
 */
esp_err_t minihub_web_fetch_map_snapshot_async(double lat, double lon, int width_px, int height_px,
                                                 minihub_map_snapshot_done_cb_t cb, void *user_ctx);
