/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    /**
     * @brief Path template: zoom, tile_x, tile_y (OSM slippy).
     */
    MINIHUB_MAP_TILE_FMT_ZXY = 0,
    /**
     * @brief Query template: tile_x, tile_y, zoom (Gaode/Amap).
     */
    MINIHUB_MAP_TILE_FMT_AMAP_XYZ,
} minihub_map_tile_fmt_t;

typedef struct {
    /**
     * @brief Printf-style template; argument order depends on `format`.
     */
    const char *url_template;
    /**
     * @brief When url uses a literal IP, set SNI/Host (e.g. "tile.openstreetmap.org").
     */
    const char *http_host;
    minihub_map_tile_fmt_t format;
    /**
     * @brief Convert input WGS84 lat/lon to GCJ-02 before tile projection (Gaode).
     */
    bool use_gcj02;
} minihub_map_tile_source_t;

/**
 * @brief Rough mainland-China bounding box for map provider selection.
 */
bool minihub_map_in_china(double lat, double lon);

/**
 * @brief WGS84 → GCJ-02 (no-op outside China bbox).
 */
void minihub_map_wgs84_to_gcj02(double wgs_lat, double wgs_lon, double *gcj_lat, double *gcj_lon);

/**
 * @brief Stitch map tiles into a PNG snapshot (exact width_px x height_px, Hub centered).
 */
esp_err_t minihub_map_build_tile_snapshot(double lat, double lon, int zoom, int width_px, int height_px,
                                          const minihub_map_tile_source_t *source, const char *out_path);
