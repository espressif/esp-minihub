/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Test-only recording seams for the Hub APIs the BLE Mesh adapter calls.
 * The adapter under test is the real one; these stand in for Core, the mesh
 * stack and product_params so a test can assert what the adapter sent and
 * what it wrote to the point. */

#pragma once

#include "minihub_core.h"
#include "minihub_ble_mesh.h"
#include "minihub_product_params.h"

#define HUB_MESH_CMD_MAX 16
#define HUB_DEV_MAX      8
#define HUB_POINT_MAX    8
#define HUB_PP_MAX       16

typedef enum {
    HUB_MESH_NONE = 0,
    HUB_MESH_ONOFF,
    HUB_MESH_LEVEL,
    HUB_MESH_LIGHTNESS,
    HUB_MESH_CTL,
    HUB_MESH_HSL,
} hub_mesh_kind_t;

typedef struct {
    hub_mesh_kind_t kind;
    uint16_t unicast;
    int onoff;
    int16_t level;
    uint16_t lightness;
    uint16_t temperature;
    int16_t delta_uv;
    uint16_t hue;
    uint16_t saturation;
} hub_mesh_cmd_t;

typedef struct {
    bool used;
    minihub_point_t point; /* props is owned by the stub */
} hub_point_t;

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    uint16_t model_id;
    char field[24];
    double raw;
} hub_pp_apply_t;

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char prop[MINIHUB_PP_PROP_MAX];
} hub_pp_set_t;

extern hub_mesh_cmd_t hub_mesh_cmds[HUB_MESH_CMD_MAX];
extern size_t hub_mesh_cmd_count;
extern esp_err_t hub_mesh_result; /* what every mesh writer returns */

extern minihub_ble_mesh_device_t hub_devs[HUB_DEV_MAX];
extern size_t hub_dev_count;

extern hub_point_t hub_points[HUB_POINT_MAX];
extern size_t hub_point_register_count;
extern size_t hub_point_set_count;

extern minihub_bm_provision_policy_t hub_policy;
extern size_t hub_policy_count;

extern minihub_ble_mesh_device_cb_t hub_on_device;
extern minihub_ble_mesh_attr_cb_t hub_on_attr;
extern void *hub_cb_ctx;

extern hub_pp_apply_t hub_pp_applies[HUB_PP_MAX];
extern size_t hub_pp_apply_count;
extern hub_pp_set_t hub_pp_sets[HUB_PP_MAX];
extern size_t hub_pp_set_count;

extern bool hub_running;
extern char hub_ota_point[MINIHUB_POINT_ID_MAX];
extern size_t hub_ota_count;

/** Drop every recorded call and free the stored point props. */
void hub_stub_reset(void);

/** Stored point, or NULL. */
hub_point_t *hub_point_find(const char *point_id);

/** props.params.<param>.value of a stored point, or NULL. */
const cJSON *hub_point_param(const char *point_id, const char *param);

/** Last recorded mesh command, or NULL. */
const hub_mesh_cmd_t *hub_last_mesh_cmd(void);
