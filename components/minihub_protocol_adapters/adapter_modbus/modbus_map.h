/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "minihub_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_MAX_GATEWAYS 8
#define MODBUS_MAX_MAPS     64
#define MODBUS_MAX_UNITS_PER_GW 16

typedef enum {
    MODBUS_FC_COIL = 0,
    MODBUS_FC_DISCRETE,
    MODBUS_FC_HOLDING,
    MODBUS_FC_INPUT,
} modbus_fc_kind_t;

typedef enum {
    MODBUS_DT_U16 = 0,
    MODBUS_DT_S16,
    MODBUS_DT_U32,
    MODBUS_DT_S32,
    MODBUS_DT_F32,
} modbus_datatype_t;

/** How MiniHub reaches the Modbus peer(s). */
typedef enum {
    MODBUS_TRANSPORT_TCP = 0,       /**< Native Modbus TCP (MBAP) */
    MODBUS_TRANSPORT_RTU,           /**< Local UART / RS-485 RTU */
    MODBUS_TRANSPORT_RTU_OVER_TCP,  /**< Ethernet→RS-485 gateway via Modbus TCP+UID */
} modbus_transport_t;

typedef enum {
    MODBUS_PARITY_NONE = 0,
    MODBUS_PARITY_EVEN,
    MODBUS_PARITY_ODD,
} modbus_parity_t;

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char type[MINIHUB_TYPE_MAX];
    int gateway_idx;
    uint8_t unit_id;
    modbus_fc_kind_t fc;
    uint16_t address;
    uint16_t count;
    modbus_datatype_t datatype;
    double scale;
    double offset;
    bool writable;
    bool batch_as_props;
    uint8_t fail_streak;
} modbus_map_t;

typedef struct {
    char id[32];
    modbus_transport_t transport;
    char host[64];
    uint16_t port;
    uint32_t timeout_ms;
    uint8_t default_unit_id;
    /* RTU / RS-485 (ignored for tcp / rtuovertcp) */
    int uart_port;
    int baudrate;
    modbus_parity_t parity;
    uint8_t data_bits;
    uint8_t stop_bits;
    int tx_pin;
    int rx_pin;
    int rts_pin; /* DE/~RE; -1 = unused */
} modbus_gateway_t;

typedef struct {
    bool enabled;
    uint32_t poll_ms;
    modbus_gateway_t gateways[MODBUS_MAX_GATEWAYS];
    size_t gateway_count;
    modbus_map_t maps[MODBUS_MAX_MAPS];
    size_t map_count;
} modbus_config_t;

esp_err_t modbus_parse_config(const cJSON *cfg, modbus_config_t *out);

/** Map fc+write+count → Modbus function code (1–16). */
uint8_t modbus_command_for(modbus_fc_kind_t fc, bool write, uint16_t count);

/** Registers (or coil words) needed for datatype+count. */
uint16_t modbus_reg_count(const modbus_map_t *m);

/** Decode big-endian register payload into out_str. */
bool modbus_format_value(const modbus_map_t *m, const uint8_t *data, size_t data_len,
                         char *out_str, size_t out_len, cJSON **out_props);

#ifdef __cplusplus
}
#endif
