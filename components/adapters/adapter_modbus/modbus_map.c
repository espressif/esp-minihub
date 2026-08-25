/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "modbus_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "strings.h"

/* Keep FC numbers local so this file builds without esp-modbus. */
enum {
    MB_FUNC_READ_COILS = 1,
    MB_FUNC_READ_DISCRETE_INPUTS = 2,
    MB_FUNC_READ_HOLDING_REGISTER = 3,
    MB_FUNC_READ_INPUT_REGISTER = 4,
    MB_FUNC_WRITE_SINGLE_COIL = 5,
    MB_FUNC_WRITE_REGISTER = 6,
    MB_FUNC_WRITE_MULTIPLE_COILS = 15,
    MB_FUNC_WRITE_MULTIPLE_REGISTERS = 16,
};

static modbus_fc_kind_t parse_fc(const char *s)
{
    if (!s) {
        return MODBUS_FC_HOLDING;
    }
    if (strcasecmp(s, "coil") == 0 || strcasecmp(s, "coils") == 0) {
        return MODBUS_FC_COIL;
    }
    if (strcasecmp(s, "discrete") == 0 || strcasecmp(s, "di") == 0) {
        return MODBUS_FC_DISCRETE;
    }
    if (strcasecmp(s, "input") == 0 || strcasecmp(s, "ir") == 0) {
        return MODBUS_FC_INPUT;
    }
    return MODBUS_FC_HOLDING;
}

static modbus_datatype_t parse_dt(const char *s)
{
    if (!s) {
        return MODBUS_DT_U16;
    }
    if (strcasecmp(s, "s16") == 0 || strcasecmp(s, "int16") == 0) {
        return MODBUS_DT_S16;
    }
    if (strcasecmp(s, "u32") == 0 || strcasecmp(s, "uint32") == 0) {
        return MODBUS_DT_U32;
    }
    if (strcasecmp(s, "s32") == 0 || strcasecmp(s, "int32") == 0) {
        return MODBUS_DT_S32;
    }
    if (strcasecmp(s, "f32") == 0 || strcasecmp(s, "float") == 0) {
        return MODBUS_DT_F32;
    }
    return MODBUS_DT_U16;
}

static modbus_transport_t parse_transport(const char *s)
{
    if (!s || !s[0]) {
        return MODBUS_TRANSPORT_TCP;
    }
    if (strcasecmp(s, "rtu") == 0 || strcasecmp(s, "rs485") == 0 || strcasecmp(s, "serial") == 0) {
        return MODBUS_TRANSPORT_RTU;
    }
    if (strcasecmp(s, "rtuovertcp") == 0 || strcasecmp(s, "rtu_over_tcp") == 0 ||
        strcasecmp(s, "gateway") == 0) {
        return MODBUS_TRANSPORT_RTU_OVER_TCP;
    }
    return MODBUS_TRANSPORT_TCP;
}

static modbus_parity_t parse_parity(const char *s)
{
    if (!s) {
        return MODBUS_PARITY_NONE;
    }
    if (strcasecmp(s, "even") == 0) {
        return MODBUS_PARITY_EVEN;
    }
    if (strcasecmp(s, "odd") == 0) {
        return MODBUS_PARITY_ODD;
    }
    return MODBUS_PARITY_NONE;
}

esp_err_t modbus_parse_config(const cJSON *cfg, modbus_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->poll_ms = 2000;
    if (!cJSON_IsObject(cfg)) {
        return ESP_OK;
    }

    const cJSON *en = cJSON_GetObjectItem(cfg, "enabled");
    out->enabled = cJSON_IsTrue(en);
    const cJSON *poll = cJSON_GetObjectItem(cfg, "poll_ms");
    if (cJSON_IsNumber(poll) && poll->valueint >= 500) {
        out->poll_ms = (uint32_t)poll->valueint;
    }

    const cJSON *gws = cJSON_GetObjectItem(cfg, "gateways");
    if (!cJSON_IsArray(gws)) {
        return ESP_OK;
    }

    const cJSON *gw = NULL;
    cJSON_ArrayForEach(gw, gws) {
        if (out->gateway_count >= MODBUS_MAX_GATEWAYS) {
            break;
        }
        if (!cJSON_IsObject(gw)) {
            continue;
        }
        modbus_gateway_t *g = &out->gateways[out->gateway_count];
        memset(g, 0, sizeof(*g));
        const cJSON *id = cJSON_GetObjectItem(gw, "id");
        const cJSON *tr = cJSON_GetObjectItem(gw, "transport");
        const cJSON *host = cJSON_GetObjectItem(gw, "host");
        const cJSON *port = cJSON_GetObjectItem(gw, "port");
        const cJSON *tmo = cJSON_GetObjectItem(gw, "timeout_ms");
        const cJSON *uid = cJSON_GetObjectItem(gw, "unit_id");
        g->transport = parse_transport(cJSON_IsString(tr) ? tr->valuestring : NULL);
        g->uart_port = -1;
        g->baudrate = 9600;
        g->parity = MODBUS_PARITY_NONE;
        g->data_bits = 8;
        g->stop_bits = 1;
        g->tx_pin = -1;
        g->rx_pin = -1;
        g->rts_pin = -1;

        if (g->transport == MODBUS_TRANSPORT_RTU) {
            const cJSON *uart = cJSON_GetObjectItem(gw, "uart_port");
            if (cJSON_IsNumber(uart)) {
                g->uart_port = uart->valueint;
            }
            const cJSON *baud = cJSON_GetObjectItem(gw, "baudrate");
            if (cJSON_IsNumber(baud) && baud->valueint > 0) {
                g->baudrate = baud->valueint;
            }
            const cJSON *par = cJSON_GetObjectItem(gw, "parity");
            g->parity = parse_parity(cJSON_IsString(par) ? par->valuestring : NULL);
            const cJSON *db = cJSON_GetObjectItem(gw, "data_bits");
            if (cJSON_IsNumber(db) && db->valueint >= 5 && db->valueint <= 8) {
                g->data_bits = (uint8_t)db->valueint;
            }
            const cJSON *sb = cJSON_GetObjectItem(gw, "stop_bits");
            if (cJSON_IsNumber(sb) && (sb->valueint == 1 || sb->valueint == 2)) {
                g->stop_bits = (uint8_t)sb->valueint;
            }
            const cJSON *tx = cJSON_GetObjectItem(gw, "tx_pin");
            const cJSON *rx = cJSON_GetObjectItem(gw, "rx_pin");
            const cJSON *rts = cJSON_GetObjectItem(gw, "rts_pin");
            if (cJSON_IsNumber(tx)) {
                g->tx_pin = tx->valueint;
            }
            if (cJSON_IsNumber(rx)) {
                g->rx_pin = rx->valueint;
            }
            if (cJSON_IsNumber(rts)) {
                g->rts_pin = rts->valueint;
            }
        } else {
            if (!cJSON_IsString(host) || !host->valuestring[0]) {
                continue;
            }
            strncpy(g->host, host->valuestring, sizeof(g->host) - 1);
            g->port = cJSON_IsNumber(port) ? (uint16_t)port->valueint : 502;
        }

        if (cJSON_IsString(id)) {
            strncpy(g->id, id->valuestring, sizeof(g->id) - 1);
        } else {
            snprintf(g->id, sizeof(g->id), "gw%u", (unsigned)out->gateway_count);
        }
        g->timeout_ms = cJSON_IsNumber(tmo) ? (uint32_t)tmo->valueint : 1000;
        g->default_unit_id = cJSON_IsNumber(uid) ? (uint8_t)uid->valueint : 1;
        int gw_idx = (int)out->gateway_count;
        out->gateway_count++;

        const cJSON *maps = cJSON_GetObjectItem(gw, "maps");
        if (!cJSON_IsArray(maps)) {
            continue;
        }
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, maps) {
            if (out->map_count >= MODBUS_MAX_MAPS || !cJSON_IsObject(m)) {
                continue;
            }
            const cJSON *pid = cJSON_GetObjectItem(m, "point_id");
            if (!cJSON_IsString(pid) || !pid->valuestring[0]) {
                continue;
            }
            modbus_map_t *map = &out->maps[out->map_count];
            memset(map, 0, sizeof(*map));
            strncpy(map->point_id, pid->valuestring, sizeof(map->point_id) - 1);
            const cJSON *type = cJSON_GetObjectItem(m, "type");
            if (cJSON_IsString(type)) {
                strncpy(map->type, type->valuestring, sizeof(map->type) - 1);
            } else {
                strncpy(map->type, "sensor", sizeof(map->type) - 1);
            }
            map->gateway_idx = gw_idx;
            map->unit_id = g->default_unit_id;
            const cJSON *mu = cJSON_GetObjectItem(m, "unit_id");
            if (cJSON_IsNumber(mu)) {
                map->unit_id = (uint8_t)mu->valueint;
            }
            const cJSON *fc = cJSON_GetObjectItem(m, "fc");
            map->fc = parse_fc(cJSON_IsString(fc) ? fc->valuestring : NULL);
            const cJSON *addr = cJSON_GetObjectItem(m, "address");
            map->address = cJSON_IsNumber(addr) ? (uint16_t)addr->valueint : 0;
            const cJSON *cnt = cJSON_GetObjectItem(m, "count");
            map->count = cJSON_IsNumber(cnt) && cnt->valueint > 0 ? (uint16_t)cnt->valueint : 1;
            const cJSON *dt = cJSON_GetObjectItem(m, "datatype");
            map->datatype = parse_dt(cJSON_IsString(dt) ? dt->valuestring : NULL);
            const cJSON *sc = cJSON_GetObjectItem(m, "scale");
            map->scale = cJSON_IsNumber(sc) ? sc->valuedouble : 1.0;
            const cJSON *off = cJSON_GetObjectItem(m, "offset");
            map->offset = cJSON_IsNumber(off) ? off->valuedouble : 0.0;
            map->writable = cJSON_IsTrue(cJSON_GetObjectItem(m, "writable"));
            map->batch_as_props = cJSON_IsTrue(cJSON_GetObjectItem(m, "batch_as_props"));
            out->map_count++;
        }
    }
    return ESP_OK;
}

uint8_t modbus_command_for(modbus_fc_kind_t fc, bool write, uint16_t count)
{
    if (!write) {
        switch (fc) {
        case MODBUS_FC_COIL:
            return MB_FUNC_READ_COILS;
        case MODBUS_FC_DISCRETE:
            return MB_FUNC_READ_DISCRETE_INPUTS;
        case MODBUS_FC_INPUT:
            return MB_FUNC_READ_INPUT_REGISTER;
        case MODBUS_FC_HOLDING:
        default:
            return MB_FUNC_READ_HOLDING_REGISTER;
        }
    }
    switch (fc) {
    case MODBUS_FC_COIL:
        return (count > 1) ? MB_FUNC_WRITE_MULTIPLE_COILS : MB_FUNC_WRITE_SINGLE_COIL;
    case MODBUS_FC_HOLDING:
        return (count > 1 || count == 0) ? MB_FUNC_WRITE_MULTIPLE_REGISTERS : MB_FUNC_WRITE_REGISTER;
    default:
        return 0;
    }
}

uint16_t modbus_reg_count(const modbus_map_t *m)
{
    if (!m) {
        return 1;
    }
    if (m->fc == MODBUS_FC_COIL || m->fc == MODBUS_FC_DISCRETE) {
        return m->count ? m->count : 1;
    }
    switch (m->datatype) {
    case MODBUS_DT_U32:
    case MODBUS_DT_S32:
    case MODBUS_DT_F32:
        return (uint16_t)((m->count ? m->count : 1) * 2);
    default:
        return m->count ? m->count : 1;
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool modbus_format_value(const modbus_map_t *m, const uint8_t *data, size_t data_len,
                         char *out_str, size_t out_len, cJSON **out_props)
{
    if (!m || !data || !out_str || out_len == 0) {
        return false;
    }
    if (out_props) {
        *out_props = NULL;
    }

    if (m->fc == MODBUS_FC_COIL || m->fc == MODBUS_FC_DISCRETE) {
        bool on = data_len > 0 && (data[0] & 0x01);
        if (strcmp(m->type, "switch") == 0 || strcmp(m->type, "binary_sensor") == 0) {
            snprintf(out_str, out_len, "%s", on ? "on" : "off");
        } else {
            snprintf(out_str, out_len, "%u", on ? 1u : 0u);
        }
        return true;
    }

    if (data_len < 2) {
        return false;
    }

    double raw = 0;
    switch (m->datatype) {
    case MODBUS_DT_S16:
        raw = (int16_t)be16(data);
        break;
    case MODBUS_DT_U32:
        if (data_len < 4) {
            return false;
        }
        raw = (double)be32(data);
        break;
    case MODBUS_DT_S32:
        if (data_len < 4) {
            return false;
        }
        raw = (double)(int32_t)be32(data);
        break;
    case MODBUS_DT_F32: {
        if (data_len < 4) {
            return false;
        }
        uint32_t u = be32(data);
        float f;
        memcpy(&f, &u, sizeof(f));
        raw = (double)f;
        break;
    }
    case MODBUS_DT_U16:
    default:
        raw = (double)be16(data);
        break;
    }

    double v = raw * m->scale + m->offset;
    if (fabs(v - (double)(int64_t)v) < 1e-9) {
        snprintf(out_str, out_len, "%lld", (long long)(int64_t)v);
    } else {
        snprintf(out_str, out_len, "%.4f", v);
    }

    if (m->batch_as_props && out_props && m->count > 1) {
        cJSON *props = cJSON_CreateObject();
        if (props) {
            size_t nreg = data_len / 2;
            for (size_t i = 0; i < nreg && i < 32; i++) {
                char key[8];
                snprintf(key, sizeof(key), "r%u", (unsigned)i);
                cJSON_AddNumberToObject(props, key, be16(data + i * 2));
            }
            *out_props = props;
        }
    }
    return true;
}
