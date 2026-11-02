/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "minihub_core.h"

#define MINIHUB_BM_CAP_ONOFF      (1u << 0)
#define MINIHUB_BM_CAP_LEVEL      (1u << 1)
#define MINIHUB_BM_CAP_LIGHTNESS  (1u << 2)
#define MINIHUB_BM_CAP_CTL        (1u << 3)

#define MINIHUB_BM_MAX_DEVICES 16
#define MINIHUB_BM_UUID_HEX_LEN 33 /* 32 hex + NUL */
#define MINIHUB_BM_UUID_FILTER_MAX 8
#define MINIHUB_BM_UUID_FILTER_BYTES_MAX 16
#define MINIHUB_BM_BIND_MODEL_MAX 12
/**
 * @brief SIG models use company_id == MINIHUB_BM_CID_SIG.
 */
#define MINIHUB_BM_CID_SIG 0xFFFF

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    char uuid_hex[MINIHUB_BM_UUID_HEX_LEN];
    uint16_t unicast;
    uint8_t elem_num;
    uint8_t caps;
    uint16_t cid; /* Composition Company ID (Bluetooth SIG) */
    uint16_t pid; /* Product ID */
    uint16_t vid; /* Version ID */
} minihub_ble_mesh_device_t;

/**
 * @brief Device UUID prefix filter (OR'd). accept_all or empty filter_count → any UUID.
 */
typedef struct {
    uint8_t bytes[MINIHUB_BM_UUID_FILTER_BYTES_MAX];
    uint8_t len;
    uint8_t offset;
} minihub_bm_uuid_filter_t;

typedef struct {
    uint16_t model_id;
    uint16_t company_id; /* MINIHUB_BM_CID_SIG for SIG models */
} minihub_bm_bind_model_t;

typedef struct {
    minihub_bm_uuid_filter_t filters[MINIHUB_BM_UUID_FILTER_MAX];
    size_t filter_count;
    bool accept_all_uuid;
    minihub_bm_bind_model_t bind_models[MINIHUB_BM_BIND_MODEL_MAX];
    size_t bind_model_count;
} minihub_bm_provision_policy_t;

typedef enum {
    MINIHUB_BM_ATTR_ONOFF = 1,
    MINIHUB_BM_ATTR_LEVEL = 2,
    MINIHUB_BM_ATTR_LIGHTNESS = 3,
    MINIHUB_BM_ATTR_CTL = 4,
} minihub_ble_mesh_attr_kind_t;

typedef struct {
    uint16_t unicast;
    minihub_ble_mesh_attr_kind_t kind;
    union {
        bool onoff;
        int16_t level;
        uint16_t lightness;
        struct {
            uint16_t lightness;
            uint16_t temperature; /* CTL Kelvin proxy 800–20000 */
            int16_t delta_uv;
        } ctl;
    } u;
} minihub_ble_mesh_attr_evt_t;

typedef enum {
    MINIHUB_BM_ONOFF_OFF = 0,
    MINIHUB_BM_ONOFF_ON = 1,
    MINIHUB_BM_ONOFF_TOGGLE = 2,
} minihub_ble_mesh_onoff_cmd_t;
