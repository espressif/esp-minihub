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
#include "minihub_ble_mesh_types.h"

/**
 * Format point_id as "ble_mesh.bm_{unicast:04x}" (two-segment, protocol-prefixed).
 * Type stays on the point's `type` field — do not encode it in the id.
 */
int minihub_bm_format_point_id(char *out, size_t out_len, uint16_t unicast);

/**
 * Rewrite legacy "{type}/bm_XXXX" slash ids to "ble_mesh.bm_XXXX".
 * Returns true when point_id was rewritten in place.
 */
bool minihub_bm_normalize_point_id(char *point_id, size_t len);

/**
 * Pointer to the stable "bm_XXXX" suffix inside point_id (dotted or slash),
 * or NULL if the id does not contain one.
 */
const char *minihub_bm_stable_suffix(const char *point_id);

int minihub_bm_uuid_to_hex(const uint8_t uuid[16], char *out, size_t out_len);
int minihub_bm_hex_to_uuid(const char *hex, uint8_t uuid[16]);

#define MINIHUB_BM_MAP_PARAM_MAX    32
#define MINIHUB_BM_MAP_TYPE_MAX     MINIHUB_TYPE_MAX
#define MINIHUB_BM_MAP_KIND_MAX     16
#define MINIHUB_BM_MAP_NAME_MAX     24
#define MINIHUB_BM_MAP_ACCESS_MAX   8
#define MINIHUB_BM_MAP_INIT_MAX     16
#define MINIHUB_BM_MAP_MAX_PROPS    MINIHUB_BM_MAX_PROPS
#define MINIHUB_BM_MAP_MAX_MODELS   32
#define MINIHUB_BM_MAP_MAX_CLASSIFY 32
#define MINIHUB_BM_MAP_MAX_PARAMS   64
#define MINIHUB_BM_MAP_MAX_CTRLS    32
#define MINIHUB_BM_MAP_MAX_SENSORS  32

/**
 * Status reporting: the group the Server models publish to, and the Hub's own
 * subscription to it. Absent `report` in the map keeps the Get-only behaviour,
 * so a product that does not ask for reporting gets no publication written to
 * any node.
 */
typedef struct {
    bool enabled;
    uint16_t group;             /* group address the Server models publish to */
    uint16_t period_s;          /* whole seconds between periodic Status publishes */
    uint8_t period_octet;       /* period_s as the Model Publication Period octet */
    uint8_t ttl;                /* 0 = keep the node's current publication TTL */
    uint8_t retransmit_count;   /* 0 = no retransmission */
    uint16_t retransmit_ms;     /* interval between retransmissions */
} minihub_bm_report_t;

/**
 * Encode whole seconds into the Model Publication Period octet: bits 6-7 select
 * the step size (100 ms / 1 s / 10 s / 10 min) and bits 0-5 count the steps.
 * Returns false when no encoding is exact, so a rounded period is never written
 * to a node without being asked for. seconds == 0 encodes 0x00, which means "no
 * periodic publishing" while state-change publishing still works.
 */
bool minihub_bm_encode_pub_period(uint16_t seconds, uint8_t *octet_out);

/** The parsed reporting block; never NULL. */
const minihub_bm_report_t *minihub_bm_report(void);

typedef struct {
    uint16_t model_id;
    uint16_t cap_bits; /* MINIHUB_BM_CAP_* of cap + also, OR'd */
    uint16_t client_id;
    uint16_t get_opcode; /* SIG Get that reads this model's state back; 0 = no read-back */
    char name[MINIHUB_BM_MAP_NAME_MAX];  /* config name = the cap name, e.g. "onoff" */
    char alias[MINIHUB_BM_MAP_NAME_MAX]; /* published long name, e.g. "gen_onoff_srv" */
} minihub_bm_map_model_t;

typedef struct {
    char type[MINIHUB_BM_MAP_TYPE_MAX];
    int priority;
    uint16_t any_caps_mask; /* OR of CAP bits; 0 = rule does not look at caps */
    uint16_t all_props[MINIHUB_BM_MAP_MAX_PROPS];
    size_t all_prop_count;
    uint16_t any_props[MINIHUB_BM_MAP_MAX_PROPS];
    size_t any_prop_count;
} minihub_bm_map_classify_t;

typedef struct {
    uint16_t cap_bit;     /* 0 when the row keys on a sensor property */
    uint16_t property_id; /* 0 when the row keys on a cap */
    char param[MINIHUB_BM_MAP_PARAM_MAX];
    char access[MINIHUB_BM_MAP_ACCESS_MAX]; /* "R" / "RW" */
    char init[MINIHUB_BM_MAP_INIT_MAX];     /* default value text; empty when the row has none */
    bool init_is_number;                    /* the init text is a number, not a string */
} minihub_bm_map_param_t;

/**
 * @brief Mesh writer a control param is routed to.
 *
 * The names match the `kind` / `fallback` strings in the map JSON.
 */
typedef enum {
    MINIHUB_BM_CTRL_NONE = 0,
    MINIHUB_BM_CTRL_ONOFF,
    MINIHUB_BM_CTRL_LEVEL,
    MINIHUB_BM_CTRL_LIGHTNESS,
    MINIHUB_BM_CTRL_CTL,
    MINIHUB_BM_CTRL_HSL,
} minihub_bm_ctrl_kind_t;

typedef struct {
    char param[MINIHUB_BM_MAP_PARAM_MAX];
    minihub_bm_ctrl_kind_t kind;
    uint16_t model_id;
    minihub_bm_ctrl_kind_t fallback; /* alternate kind when the node lacks model_id */
    char field[MINIHUB_BM_MAP_KIND_MAX]; /* message field this param drives */
} minihub_bm_map_control_t;

typedef struct {
    uint16_t property_id;
    char param[MINIHUB_BM_MAP_PARAM_MAX];
    uint8_t size;
    bool is_signed;
    double scale;
    int64_t unknown; /* raw sentinel; -1 when the table has no "unknown" key */
    bool has_unknown;
    int bool_threshold; /* -1 when absent */
    bool has_bool_threshold;
    double min; /* catalog bounds; absent when the catalog has none */
    double max;
    bool has_min;
    bool has_max;
} minihub_bm_sensor_t;

/**
 * Load embedded default, then overlay (merge) optional files:
 *   /store/preinstall/ble_mesh_map.json
 *   /data/config/ble_mesh_map.json
 * Later sources upsert same-identity entries and append new ones. init is idempotent;
 * reload clears the tables first so it is safe to call again.
 */
esp_err_t minihub_bm_map_init(void);
esp_err_t minihub_bm_map_reload(void);

/**
 * Match classify[] rules: all_props present AND any_props hit AND any_caps hit, where an
 * empty condition list is ignored. Highest priority wins. NULL = no rule matched, i.e. the
 * node is unknown — callers must not invent a type.
 */
const char *minihub_bm_classify(uint16_t caps, const uint16_t *props, size_t n_props);

/** Number of classify[] rows. */
size_t minihub_bm_classify_count(void);

/** classify[] row by index, or NULL when out of range. */
const minihub_bm_map_classify_t *minihub_bm_classify_at(size_t index);

/** Server model id → OR'd CAP bits and the matching client model id; false when unknown. */
bool minihub_bm_model_cap(uint16_t model_id, uint16_t *caps_out, uint16_t *client_out);

/** Number of models[] rows. */
size_t minihub_bm_model_count(void);

/** models[] row by index, or NULL when out of range. */
const minihub_bm_map_model_t *minihub_bm_model_at(size_t index);

/** models[] row for a SIG server model id, or NULL when the table does not know it. */
const minihub_bm_map_model_t *minihub_bm_model_by_id(uint16_t model_id);

/**
 * Config name → model id, matching a row's name or its alias.
 * False when the table has no such model; *model_id is then left untouched.
 */
bool minihub_bm_model_id_by_name(const char *name, uint16_t *model_id);

/**
 * Collect params[] names whose cap bit is set or whose property is in @p props, de-duplicated
 * in table order, at most @p max. No catalog intersection here — the caller does that.
 * Returned pointers live in the table and stay valid until the next reload.
 */
size_t minihub_bm_params_for(uint16_t caps, const uint16_t *props, size_t n_props,
                             const char **names, size_t max);

/** params[] entry for a param name, or NULL when the table has no such row. */
const minihub_bm_map_param_t *minihub_bm_param_by_name(const char *name);

/** controls[] entry for a param name, or NULL when the param has no mesh command. */
const minihub_bm_map_control_t *minihub_bm_control_for_param(const char *param);

/** Number of controls[] rows. */
size_t minihub_bm_control_count(void);

/** controls[] row by index, or NULL when out of range. */
const minihub_bm_map_control_t *minihub_bm_control_at(size_t index);

/** sensors[] decode descriptor for a SIG property id, or NULL. Valid until the next reload. */
const minihub_bm_sensor_t *minihub_bm_sensor_by_property(uint16_t property_id);
