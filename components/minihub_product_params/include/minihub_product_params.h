/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#define MINIHUB_PP_PROP_MAX 32
#define MINIHUB_PP_LABEL_MAX 48
#define MINIHUB_PP_MAX_QUIRKS 8
#define MINIHUB_PP_MAX_PARAMS 16

typedef enum {
    MINIHUB_PP_TRANSPORT_ZIGBEE = 1,
    MINIHUB_PP_TRANSPORT_BLE_MESH = 2,
} minihub_pp_transport_t;

typedef struct {
    char prop[MINIHUB_PP_PROP_MAX];
    char label[MINIHUB_PP_LABEL_MAX];
    char unit[16];
    bool writable; /* access == "rw" */
    uint16_t cluster;
    uint16_t attr_id;
    uint8_t zcl_type;
    float multiply;
    float divide;
    float offset;
} minihub_pp_zb_param_t;

typedef struct {
    char prop[MINIHUB_PP_PROP_MAX];
    char label[MINIHUB_PP_LABEL_MAX];
    char unit[16];
    bool writable;
    uint16_t model_id;
    uint16_t company_id;
    uint32_t opcode;
    char field[24];
    float multiply;
    float divide;
    float offset;
} minihub_pp_bm_param_t;

esp_err_t minihub_product_params_init(void);
esp_err_t minihub_product_params_reload(void);

/** Zigbee: list quirk params for device ieee (0 if no match). */
size_t minihub_pp_zigbee_list_params(uint64_t ieee, const char *manufacturer, const char *model,
                                     minihub_pp_zb_param_t *out, size_t cap);

/** Apply a ZCL attr value into point props if matched. Returns true if applied. */
bool minihub_pp_zigbee_apply(const char *point_id, uint64_t ieee, const char *manufacturer,
                             const char *model, uint16_t cluster, uint16_t attr_id, uint8_t zcl_type,
                             const void *value, size_t value_len);

size_t minihub_pp_ble_mesh_list_params(const char *uuid_hex, minihub_pp_bm_param_t *out, size_t cap);

bool minihub_pp_ble_mesh_apply(const char *point_id, const char *uuid_hex, uint16_t model_id,
                               uint16_t company_id, uint32_t opcode, const char *field, double raw);

/**
 * set_prop: validates access, merges props for local echo; for zigbee fills *out_zb if provided.
 * Returns ESP_ERR_NOT_ALLOWED if read-only, ESP_ERR_NOT_FOUND if unknown prop.
 */
esp_err_t minihub_pp_set_prop(const char *point_id, const char *prop, const cJSON *value,
                              minihub_pp_zb_param_t *out_zb, bool *is_zigbee);

/** JSON array of {prop,label,unit,access} for a point (for Web). Caller deletes. */
cJSON *minihub_pp_meta_for_point(const char *point_id, uint64_t ieee, const char *uuid_hex);
