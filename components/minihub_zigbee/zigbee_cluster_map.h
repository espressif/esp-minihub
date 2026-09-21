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
#include "minihub_zigbee_types.h"
#include "minihub_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINIHUB_ZB_MAP_PARAM_MAX   32
#define MINIHUB_ZB_MAP_TYPE_MAX    MINIHUB_TYPE_MAX
#define MINIHUB_ZB_MAP_REPORT_MAX  24
#define MINIHUB_ZB_MAP_FIELD_MAX   24
#define MINIHUB_ZB_MAP_KIND_MAX    24
#define MINIHUB_ZB_MAP_MAX_ATTRS   48
#define MINIHUB_ZB_MAP_MAX_CTRLS   16
#define MINIHUB_ZB_MAP_MAX_CLASSIFY 8
#define MINIHUB_ZB_MAP_MAX_CLUSTERS 16
#define MINIHUB_ZB_MAP_MAX_ALIASES 4
#define MINIHUB_ZB_MAP_MAX_HA_IDS  8
#define MINIHUB_ZB_MAP_MAX_ANY_CAPS 4

typedef struct {
    uint16_t cluster_id;
    uint8_t cap_bit; /* MINIHUB_ZB_CAP_*; 0 if unknown / none */
} minihub_zb_map_cluster_t;

typedef struct {
    char type[MINIHUB_ZB_MAP_TYPE_MAX];
    int priority;
    uint16_t ha_device_ids[MINIHUB_ZB_MAP_MAX_HA_IDS];
    size_t ha_device_id_count;
    uint8_t any_caps_mask; /* OR of CAP bits; match if (caps & mask) != 0 */
} minihub_zb_map_classify_t;

typedef struct {
    uint16_t cluster;
    uint16_t attr;
    char param[MINIHUB_ZB_MAP_PARAM_MAX];
    char access[8]; /* "R" / "RW" / "W" */
    char report[MINIHUB_ZB_MAP_REPORT_MAX];
    char field[MINIHUB_ZB_MAP_FIELD_MAX]; /* contact sub-field, optional */
    bool summary;
} minihub_zb_map_attr_t;

typedef enum {
    MINIHUB_ZB_CTRL_NONE = 0,
    MINIHUB_ZB_CTRL_ON_OFF,
    MINIHUB_ZB_CTRL_MOVE_TO_LEVEL,
    MINIHUB_ZB_CTRL_WRITE_ATTR,
} minihub_zb_ctrl_kind_t;

typedef struct {
    char param[MINIHUB_ZB_MAP_PARAM_MAX];
    char aliases[MINIHUB_ZB_MAP_MAX_ALIASES][MINIHUB_ZB_MAP_PARAM_MAX];
    size_t alias_count;
    minihub_zb_ctrl_kind_t kind;
    uint16_t cluster;
    uint16_t attr;     /* write_attr */
    uint8_t zcl_type;  /* write_attr */
} minihub_zb_map_control_t;

/**
 * Load embedded default, then overlay (merge) optional files:
 *   /store/preinstall/zigbee_cluster_map.json
 *   /data/config/zigbee_cluster_map.json
 * Later sources replace same-identity entries and append new ones.
 */
esp_err_t minihub_zb_cluster_map_init(void);
esp_err_t minihub_zb_cluster_map_reload(void);

/** Derive CAP bits from interviewed cluster list using clusters[].cap. */
uint8_t minihub_zb_map_caps_from_clusters(const uint16_t *clusters, size_t count);

/** Classify MiniHub type from caps + HA app device id using classify[] rules. */
const char *minihub_zb_map_classify_type(uint8_t caps, uint16_t app_device_id);

/** True if access is read-only (R / read). Empty access → false. */
bool minihub_zb_map_access_is_readonly(const char *access);

/**
 * List attr-map params that apply to @p caps (cluster has matching CAP bit).
 * Returns count written to @p out_params (each MINIHUB_ZB_MAP_PARAM_MAX).
 */
size_t minihub_zb_map_params_for_caps(uint8_t caps, char out_params[][MINIHUB_ZB_MAP_PARAM_MAX],
                                      size_t cap);

/** Find first attr entry for a props.params name. */
const minihub_zb_map_attr_t *minihub_zb_map_attr_by_param(const char *param);

/** Find first attr entry whose report name matches (e.g. "onoff", "temp_c"). */
const minihub_zb_map_attr_t *minihub_zb_map_attr_by_report(const char *report);

/**
 * Collect all attr entries with the given report name (e.g. contact multi-field).
 * Returns count written to @p out.
 */
size_t minihub_zb_map_attrs_by_report(const char *report, const minihub_zb_map_attr_t **out,
                                      size_t out_cap);

/** Lookup control by param name or alias. */
const minihub_zb_map_control_t *minihub_zb_map_control_for_param(const char *param);

/** True if type (or any control) supports on_off kind. */
bool minihub_zb_map_type_has_onoff(const char *type);

/** Default summary value for a newly registered device. */
const char *minihub_zb_map_default_summary(const minihub_zigbee_device_t *dev);

/** Map attr_kind enum → report string used in JSON. */
const char *minihub_zb_map_report_name_for_kind(minihub_zigbee_attr_kind_t kind);

#ifdef __cplusplus
}
#endif
