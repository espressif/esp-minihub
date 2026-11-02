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

#define MINIHUB_ZB_CAP_ONOFF       (1u << 0)
#define MINIHUB_ZB_CAP_TEMP        (1u << 1)
#define MINIHUB_ZB_CAP_HUMIDITY    (1u << 2)
#define MINIHUB_ZB_CAP_IAS_ZONE    (1u << 3)
#define MINIHUB_ZB_CAP_ELECTRICAL  (1u << 4) /* Electrical Measurement 0x0b04 */
#define MINIHUB_ZB_CAP_ENERGY      (1u << 5) /* Simple Metering 0x0702 */
#define MINIHUB_ZB_MAX_DEVICES     16

/**
 * @brief Sensors that are not on/off actuators (temp/humidity/contact).
 */
#define MINIHUB_ZB_CAP_SENSOR_MASK \
    (MINIHUB_ZB_CAP_TEMP | MINIHUB_ZB_CAP_HUMIDITY | MINIHUB_ZB_CAP_IAS_ZONE)

/**
 * @brief Plug metering clusters (stay on switch points; not in SENSOR_MASK).
 */
#define MINIHUB_ZB_CAP_METERING_MASK \
    (MINIHUB_ZB_CAP_ELECTRICAL | MINIHUB_ZB_CAP_ENERGY)

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    uint64_t ieee;
    uint16_t short_addr;
    uint8_t endpoint;
    uint8_t caps;
} minihub_zigbee_device_t;

typedef enum {
    MINIHUB_ZB_ATTR_ONOFF = 1,
    MINIHUB_ZB_ATTR_TEMP_C,
    MINIHUB_ZB_ATTR_HUMIDITY_PCT,
    MINIHUB_ZB_ATTR_CONTACT,
    MINIHUB_ZB_ATTR_POWER_W,
    MINIHUB_ZB_ATTR_VOLTAGE_V,
    MINIHUB_ZB_ATTR_CURRENT_A,
    MINIHUB_ZB_ATTR_ENERGY_WH,
} minihub_zigbee_attr_kind_t;

typedef struct {
    uint16_t short_addr;
    uint8_t endpoint;
    minihub_zigbee_attr_kind_t kind;
    union {
        bool onoff;
        float temp_c;
        float humidity_pct;
        float power_w;
        float voltage_v;
        float current_a;
        float energy_wh;
        struct {
            bool open;       /* Alarm1: open=true, closed=false */
            bool tamper;     /* ZoneStatus Tamper bit */
            bool battery_low; /* ZoneStatus Battery bit */
            uint16_t zone_status;
        } contact;
    } u;
} minihub_zigbee_attr_evt_t;

typedef enum {
    MINIHUB_ZB_ONOFF_OFF = 0,
    MINIHUB_ZB_ONOFF_ON = 1,
    MINIHUB_ZB_ONOFF_TOGGLE = 2,
} minihub_zigbee_onoff_cmd_t;
