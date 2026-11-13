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

#define MINIHUB_MATTER_MAP_PARAM_MAX 32
#define MINIHUB_MATTER_MAP_FIELD_MAX 16
#define MINIHUB_MATTER_MAP_MAX_CTRLS  32
#define MINIHUB_MATTER_MAP_MAX_MODELS 32
#define MINIHUB_MATTER_MAP_MAX_CAPARAMS 64

/**
 * Load embedded default (matter_map.json), then overlay (merge) optional files:
 *   /store/preinstall/matter_map.json
 *   /data/config/matter_map.json
 * Later sources upsert same-identity entries and append new ones. init is
 * idempotent; reload clears the tables first so it is safe to call again.
 */
esp_err_t minihub_matter_map_init(void);
esp_err_t minihub_matter_map_reload(void);

/**
 * Control routing for a normalized param: the Matter cluster to write and the
 * field name within it. Returns true (and fills the out params) when the param
 * has a control entry, false otherwise.
 */
bool minihub_matter_map_control_for_param(const char *param,
                                          uint16_t *cluster_out,
                                          const char **field_out);

/**
 * One (cluster, attr) to subscribe / read on Hub restart.
 */
typedef struct {
    uint16_t cluster_id;
    uint16_t attribute_id;
} minihub_matter_map_path_t;

/**
 * Deduped subscribe/read paths from controls[] + sensors[] that declare attr.
 * Always includes OnOff 0x0006/0x0000 when the table is empty so reboot still
 * syncs power. Returns count written to @p out.
 */
size_t minihub_matter_map_collect_paths(minihub_matter_map_path_t *out, size_t out_cap);

/**
 * Report routing: given an incoming (cluster, attr), return the normalized
 * param name to update, or NULL when the map does not cover it. Numeric decode
 * uses minihub_matter_map_sensor_for_cluster() below.
 */
const char *minihub_matter_map_param_for_cluster_attr(uint32_t cluster,
                                                     uint32_t attr);

/**
 * Sensor decode descriptor: how to turn a raw reported integer into the
 * normalized display value. Mirrors a `sensors[]` entry in matter_map.json.
 * `bool_threshold >= 0` marks a boolean sensor (occupancy/motion): a raw value
 * greater than the threshold means "on". Otherwise the display value is
 * `raw * scale`, clamped to [min, max].
 */
typedef struct {
    uint16_t cluster;
    char param[MINIHUB_MATTER_MAP_PARAM_MAX];
    bool is_signed;
    double scale;
    double min;
    double max;
    int bool_threshold; /*!< <0 = numeric sensor; >=0 = boolean (raw > thr => on) */
} minihub_matter_map_sensor_t;

/**
 * Look up the sensor decode descriptor for a reported cluster. Returns true and
 * fills @p out when the map declares a `sensors[]` entry for @p cluster.
 */
bool minihub_matter_map_sensor_for_cluster(uint16_t cluster,
                                           minihub_matter_map_sensor_t *out);
