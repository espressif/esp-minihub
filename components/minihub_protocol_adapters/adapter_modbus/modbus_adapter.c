/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "modbus_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_MODBUS

#include "driver/uart.h"
#include "mbcontroller.h"

/* FC numbers (mb_proto.h is not a public esp-modbus include; newer esp-modbus
 * exposes MB_FUNC_* in mb_port_types.h). Use a local prefix so this file builds
 * against both old and new esp-modbus without redeclaration. */
enum {
    MINIHUB_MB_FC_WRITE_SINGLE_COIL = 5,
    MINIHUB_MB_FC_WRITE_REGISTER = 6,
    MINIHUB_MB_FC_WRITE_MULTIPLE_REGISTERS = 16,
};

#ifndef CONFIG_MINIHUB_MODBUS_UART_NUM
#define CONFIG_MINIHUB_MODBUS_UART_NUM 1
#endif
#ifndef CONFIG_MINIHUB_MODBUS_UART_TXD
#define CONFIG_MINIHUB_MODBUS_UART_TXD (-1)
#endif
#ifndef CONFIG_MINIHUB_MODBUS_UART_RXD
#define CONFIG_MINIHUB_MODBUS_UART_RXD (-1)
#endif
#ifndef CONFIG_MINIHUB_MODBUS_UART_RTS
#define CONFIG_MINIHUB_MODBUS_UART_RTS (-1)
#endif

static const char *TAG = "adapter_modbus";

typedef struct {
    void *master;
    char *ip_table[MODBUS_MAX_UNITS_PER_GW + 1];
    char ip_rows[MODBUS_MAX_UNITS_PER_GW][96];
    size_t ip_count;
    bool started;
} modbus_gw_runtime_t;

EXT_RAM_BSS_ATTR static modbus_config_t s_cfg;
EXT_RAM_BSS_ATTR static modbus_gw_runtime_t s_rt[MODBUS_MAX_GATEWAYS];
static uint32_t s_last_poll_ms;
static bool s_points_registered;

static esp_netif_t *pick_netif(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (n) {
        return n;
    }
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static void free_gateway_runtime(modbus_gw_runtime_t *rt)
{
    if (!rt) {
        return;
    }
    if (rt->master) {
        (void)mbc_master_stop(rt->master);
        (void)mbc_master_delete(rt->master);
        rt->master = NULL;
    }
    memset(rt->ip_table, 0, sizeof(rt->ip_table));
    rt->ip_count = 0;
    rt->started = false;
}

static esp_err_t build_ip_table(int gw_idx, modbus_gw_runtime_t *rt)
{
    const modbus_gateway_t *g = &s_cfg.gateways[gw_idx];
    uint8_t units[MODBUS_MAX_UNITS_PER_GW];
    size_t n = 0;
    for (size_t i = 0; i < s_cfg.map_count; i++) {
        if (s_cfg.maps[i].gateway_idx != gw_idx) {
            continue;
        }
        uint8_t u = s_cfg.maps[i].unit_id;
        bool seen = false;
        for (size_t j = 0; j < n; j++) {
            if (units[j] == u) {
                seen = true;
                break;
            }
        }
        if (!seen && n < MODBUS_MAX_UNITS_PER_GW) {
            units[n++] = u;
        }
    }
    if (n == 0) {
        units[n++] = g->default_unit_id ? g->default_unit_id : 1;
    }
    for (size_t i = 0; i < n; i++) {
        snprintf(rt->ip_rows[i], sizeof(rt->ip_rows[i]), "%02u;%s;%u",
                 (unsigned)units[i], g->host, (unsigned)g->port);
        rt->ip_table[i] = rt->ip_rows[i];
    }
    rt->ip_table[n] = NULL;
    rt->ip_count = n;
    return ESP_OK;
}

static uart_parity_t uart_parity_from_cfg(modbus_parity_t p)
{
    switch (p) {
    case MODBUS_PARITY_EVEN:
        return UART_PARITY_EVEN;
    case MODBUS_PARITY_ODD:
        return UART_PARITY_ODD;
    case MODBUS_PARITY_NONE:
    default:
        return UART_PARITY_DISABLE;
    }
}

static uart_word_length_t uart_data_bits_from_cfg(uint8_t bits)
{
    switch (bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
    default:
        return UART_DATA_8_BITS;
    }
}

static esp_err_t start_gateway_tcp(int gw_idx, bool rtu_over_tcp)
{
    modbus_gw_runtime_t *rt = &s_rt[gw_idx];
    const modbus_gateway_t *g = &s_cfg.gateways[gw_idx];
    ESP_RETURN_ON_ERROR(build_ip_table(gw_idx, rt), TAG, "ip table");

    esp_netif_t *netif = pick_netif();
    if (!netif) {
        ESP_LOGW(TAG, "no netif yet for gateway %s", g->id);
        return ESP_ERR_INVALID_STATE;
    }

    mb_communication_info_t tcp_cfg = {
        .tcp_opts = {
            .mode = MB_TCP,
            .port = g->port,
            .uid = 0,
            .response_tout_ms = g->timeout_ms ? g->timeout_ms : 1000,
            .addr_type = MB_IPV4,
            .ip_addr_table = (void *)rt->ip_table,
            .ip_netif_ptr = (void *)netif,
            .dns_name = NULL,
            .start_disconnected = true,
        },
    };

    esp_err_t err = mbc_master_create_tcp(&tcp_cfg, &rt->master);
    if (err != ESP_OK || !rt->master) {
        ESP_LOGE(TAG, "create_tcp %s failed: %s", g->id, esp_err_to_name(err));
        rt->master = NULL;
        return err != ESP_OK ? err : ESP_FAIL;
    }
    err = mbc_master_start(rt->master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start %s failed: %s", g->id, esp_err_to_name(err));
        free_gateway_runtime(rt);
        return err;
    }
    rt->started = true;
    ESP_LOGI(TAG, "gateway %s %s master up (%s:%u, %u unit(s))",
             g->id, rtu_over_tcp ? "rtuovertcp" : "tcp",
             g->host, (unsigned)g->port, (unsigned)rt->ip_count);
    return ESP_OK;
}

static esp_err_t start_gateway_rtu(int gw_idx)
{
    modbus_gw_runtime_t *rt = &s_rt[gw_idx];
    const modbus_gateway_t *g = &s_cfg.gateways[gw_idx];

    int uart_num = g->uart_port >= 0 ? g->uart_port : CONFIG_MINIHUB_MODBUS_UART_NUM;
    int tx = g->tx_pin >= 0 ? g->tx_pin : CONFIG_MINIHUB_MODBUS_UART_TXD;
    int rx = g->rx_pin >= 0 ? g->rx_pin : CONFIG_MINIHUB_MODBUS_UART_RXD;
    int rts = g->rts_pin >= 0 ? g->rts_pin : CONFIG_MINIHUB_MODBUS_UART_RTS;

    if (tx < 0 || rx < 0) {
        ESP_LOGE(TAG, "gateway %s RTU needs tx_pin/rx_pin (or Kconfig defaults)", g->id);
        return ESP_ERR_INVALID_ARG;
    }

    mb_communication_info_t ser_cfg = {
        .ser_opts = {
            .mode = MB_RTU,
            .port = (uart_port_t)uart_num,
            .uid = 0,
            .response_tout_ms = g->timeout_ms ? g->timeout_ms : 1000,
            .baudrate = (uint32_t)(g->baudrate > 0 ? g->baudrate : 9600),
            .data_bits = uart_data_bits_from_cfg(g->data_bits),
            .stop_bits = (g->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
            .parity = uart_parity_from_cfg(g->parity),
        },
    };

    esp_err_t err = mbc_master_create_serial(&ser_cfg, &rt->master);
    if (err != ESP_OK || !rt->master) {
        ESP_LOGE(TAG, "create_serial %s failed: %s", g->id, esp_err_to_name(err));
        rt->master = NULL;
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = uart_set_pin((uart_port_t)uart_num, tx, rx,
                       rts >= 0 ? rts : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin %s failed: %s", g->id, esp_err_to_name(err));
        free_gateway_runtime(rt);
        return err;
    }

    if (rts >= 0) {
        err = uart_set_mode((uart_port_t)uart_num, UART_MODE_RS485_HALF_DUPLEX);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "uart_set_mode RS485 %s failed: %s", g->id, esp_err_to_name(err));
            free_gateway_runtime(rt);
            return err;
        }
    }

    err = mbc_master_start(rt->master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start RTU %s failed: %s", g->id, esp_err_to_name(err));
        free_gateway_runtime(rt);
        return err;
    }
    rt->started = true;
    ESP_LOGI(TAG, "gateway %s RTU master up (uart%d %d baud, tx=%d rx=%d rts=%d)",
             g->id, uart_num, g->baudrate, tx, rx, rts);
    return ESP_OK;
}

static esp_err_t start_gateway(int gw_idx)
{
    modbus_gw_runtime_t *rt = &s_rt[gw_idx];
    const modbus_gateway_t *g = &s_cfg.gateways[gw_idx];
    free_gateway_runtime(rt);

    switch (g->transport) {
    case MODBUS_TRANSPORT_RTU:
        return start_gateway_rtu(gw_idx);
    case MODBUS_TRANSPORT_RTU_OVER_TCP:
        return start_gateway_tcp(gw_idx, true);
    case MODBUS_TRANSPORT_TCP:
    default:
        return start_gateway_tcp(gw_idx, false);
    }
}

static void register_points(void)
{
    for (size_t i = 0; i < s_cfg.map_count; i++) {
        const modbus_map_t *m = &s_cfg.maps[i];
        minihub_point_t pt = {0};
        strncpy(pt.point_id, m->point_id, sizeof(pt.point_id) - 1);
        strncpy(pt.type, m->type, sizeof(pt.type) - 1);
        strncpy(pt.adapter, "modbus", sizeof(pt.adapter) - 1);
        strncpy(pt.value, "unknown", sizeof(pt.value) - 1);
        pt.online = false;
        (void)minihub_point_register(&pt);
    }
    s_points_registered = true;
}

static void unregister_points(void)
{
    for (size_t i = 0; i < s_cfg.map_count; i++) {
        (void)minihub_point_unregister(s_cfg.maps[i].point_id);
    }
    s_points_registered = false;
}

static size_t response_bytes_for_read(const modbus_map_t *m)
{
    uint16_t n = modbus_reg_count(m);
    if (m->fc == MODBUS_FC_COIL || m->fc == MODBUS_FC_DISCRETE) {
        return (size_t)((n + 7) / 8);
    }
    return (size_t)n * 2;
}

static esp_err_t read_map(modbus_map_t *m)
{
    if (m->gateway_idx < 0 || (size_t)m->gateway_idx >= s_cfg.gateway_count) {
        return ESP_ERR_INVALID_ARG;
    }
    modbus_gw_runtime_t *rt = &s_rt[m->gateway_idx];
    if (!rt->started || !rt->master) {
        esp_err_t e = start_gateway(m->gateway_idx);
        if (e != ESP_OK) {
            return e;
        }
    }

    uint8_t cmd = modbus_command_for(m->fc, false, m->count);
    uint16_t reg_size = modbus_reg_count(m);
    uint8_t buf[256];
    size_t need = response_bytes_for_read(m);
    if (need > sizeof(buf)) {
        need = sizeof(buf);
    }
    memset(buf, 0, sizeof(buf));

    mb_param_request_t req = {
        .slave_addr = m->unit_id,
        .command = cmd,
        .reg_start = m->address,
        .reg_size = reg_size,
    };
    esp_err_t err = mbc_master_send_request(rt->master, &req, buf);
    if (err != ESP_OK) {
        if (++m->fail_streak >= 3) {
            minihub_point_set_online(m->point_id, false);
        }
        return err;
    }
    m->fail_streak = 0;
    char value[48];
    cJSON *props = NULL;
    if (!modbus_format_value(m, buf, need, value, sizeof(value), &props)) {
        minihub_point_set_online(m->point_id, true);
        return ESP_OK;
    }
    minihub_point_set(m->point_id, value, props);
    if (props) {
        cJSON_Delete(props);
    }
    minihub_point_set_online(m->point_id, true);
    return ESP_OK;
}

static esp_err_t write_map(modbus_map_t *m, const char *value_str)
{
    if (!m->writable) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (m->gateway_idx < 0 || (size_t)m->gateway_idx >= s_cfg.gateway_count) {
        return ESP_ERR_INVALID_ARG;
    }
    modbus_gw_runtime_t *rt = &s_rt[m->gateway_idx];
    if (!rt->started || !rt->master) {
        ESP_RETURN_ON_ERROR(start_gateway(m->gateway_idx), TAG, "start gw");
    }

    uint8_t data[8] = {0};
    uint16_t reg_size = 1;
    uint8_t cmd;

    if (m->fc == MODBUS_FC_COIL) {
        bool on = value_str && (strcmp(value_str, "on") == 0 || strcmp(value_str, "1") == 0 ||
                                strcasecmp(value_str, "true") == 0);
        cmd = MINIHUB_MB_FC_WRITE_SINGLE_COIL;
        uint16_t coil = on ? 0xFF00 : 0x0000;
        data[0] = (uint8_t)(coil >> 8);
        data[1] = (uint8_t)(coil & 0xFF);
        reg_size = 1;
    } else if (m->fc == MODBUS_FC_HOLDING) {
        double v = value_str ? atof(value_str) : 0;
        double raw = (m->scale != 0.0) ? ((v - m->offset) / m->scale) : v;
        reg_size = modbus_reg_count(m);
        cmd = modbus_command_for(m->fc, true, reg_size);
        if (m->datatype == MODBUS_DT_F32) {
            float f = (float)raw;
            uint32_t u;
            memcpy(&u, &f, sizeof(u));
            data[0] = (uint8_t)(u >> 24);
            data[1] = (uint8_t)(u >> 16);
            data[2] = (uint8_t)(u >> 8);
            data[3] = (uint8_t)u;
        } else if (m->datatype == MODBUS_DT_U32 || m->datatype == MODBUS_DT_S32) {
            uint32_t u = (uint32_t)(int32_t)raw;
            data[0] = (uint8_t)(u >> 24);
            data[1] = (uint8_t)(u >> 16);
            data[2] = (uint8_t)(u >> 8);
            data[3] = (uint8_t)u;
        } else {
            uint16_t u = (uint16_t)(int16_t)raw;
            data[0] = (uint8_t)(u >> 8);
            data[1] = (uint8_t)u;
            if (cmd == MINIHUB_MB_FC_WRITE_REGISTER) {
                /* single register write uses 16-bit value in host endian for some stacks;
                 * esp-modbus expects register image as bytes in PDU order. */
            }
        }
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    mb_param_request_t req = {
        .slave_addr = m->unit_id,
        .command = cmd,
        .reg_start = m->address,
        .reg_size = reg_size,
    };
    return mbc_master_send_request(rt->master, &req, data);
}

static modbus_map_t *find_map(const char *point_id)
{
    for (size_t i = 0; i < s_cfg.map_count; i++) {
        if (strcmp(s_cfg.maps[i].point_id, point_id) == 0) {
            return &s_cfg.maps[i];
        }
    }
    return NULL;
}

static esp_err_t modbus_adapter_init(const cJSON *config)
{
    ESP_RETURN_ON_ERROR(minihub_adapter_check_core_api(1, 1), TAG, "core api");
    for (size_t i = 0; i < MODBUS_MAX_GATEWAYS; i++) {
        free_gateway_runtime(&s_rt[i]);
    }
    return modbus_parse_config(config, &s_cfg);
}

static esp_err_t modbus_adapter_start(void)
{
    if (!s_cfg.enabled || s_cfg.map_count == 0) {
        ESP_LOGI(TAG, "disabled or empty maps (enabled=%d maps=%u)",
                 (int)s_cfg.enabled, (unsigned)s_cfg.map_count);
        return ESP_OK;
    }
    register_points();
    /* Defer TCP connect to first poll (netif may not be up yet). */
    return ESP_OK;
}

static esp_err_t modbus_adapter_stop(void)
{
    for (size_t i = 0; i < MODBUS_MAX_GATEWAYS; i++) {
        free_gateway_runtime(&s_rt[i]);
    }
    if (s_points_registered) {
        unregister_points();
    }
    return ESP_OK;
}

static esp_err_t modbus_adapter_poll(void)
{
    if (!s_cfg.enabled || s_cfg.map_count == 0) {
        return ESP_OK;
    }
    uint32_t now = (uint32_t)(esp_log_timestamp());
    if (s_last_poll_ms != 0 && (now - s_last_poll_ms) < s_cfg.poll_ms) {
        return ESP_OK;
    }
    s_last_poll_ms = now;

    /* Limit work per poll tick. */
    size_t budget = 8;
    static size_t s_cursor;
    for (size_t n = 0; n < s_cfg.map_count && budget > 0; n++) {
        size_t i = (s_cursor + n) % s_cfg.map_count;
        (void)read_map(&s_cfg.maps[i]);
        budget--;
    }
    s_cursor = (s_cursor + 8) % (s_cfg.map_count ? s_cfg.map_count : 1);
    return ESP_OK;
}

static esp_err_t modbus_adapter_handle_command(const minihub_command_t *cmd)
{
    if (!cmd || !s_cfg.enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    modbus_map_t *m = find_map(cmd->point_id);
    if (!m) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strcmp(cmd->cmd, "turn_on") == 0) {
        esp_err_t err = write_map(m, "on");
        if (err == ESP_OK) {
            minihub_point_set(m->point_id, "on", NULL);
        }
        return err;
    }
    if (strcmp(cmd->cmd, "turn_off") == 0) {
        esp_err_t err = write_map(m, "off");
        if (err == ESP_OK) {
            minihub_point_set(m->point_id, "off", NULL);
        }
        return err;
    }
    if (strcmp(cmd->cmd, "set") == 0 || strcmp(cmd->cmd, "set_value") == 0) {
        const char *v = NULL;
        if (cmd->params && cJSON_IsString(cJSON_GetObjectItem(cmd->params, "value"))) {
            v = cJSON_GetObjectItem(cmd->params, "value")->valuestring;
        } else if (cmd->params && cJSON_IsNumber(cJSON_GetObjectItem(cmd->params, "value"))) {
            static char num[32];
            snprintf(num, sizeof(num), "%g", cJSON_GetObjectItem(cmd->params, "value")->valuedouble);
            v = num;
        }
        if (!v) {
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = write_map(m, v);
        if (err == ESP_OK) {
            char formatted[48];
            strncpy(formatted, v, sizeof(formatted) - 1);
            minihub_point_set(m->point_id, formatted, NULL);
        }
        return err;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const minihub_adapter_t modbus_adapter = {
    .name = "modbus",
    .version = "0.1.0",
    .init = modbus_adapter_init,
    .start = modbus_adapter_start,
    .stop = modbus_adapter_stop,
    .deinit = modbus_adapter_stop,
    .poll = modbus_adapter_poll,
    .handle_command = modbus_adapter_handle_command,
};

#else /* !CONFIG_MINIHUB_MODBUS */

static esp_err_t modbus_stub(const cJSON *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t modbus_stub_void(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t modbus_stub_cmd(const minihub_command_t *cmd)
{
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}

const minihub_adapter_t modbus_adapter = {
    .name = "modbus",
    .version = "0.0.0",
    .init = modbus_stub,
    .start = modbus_stub_void,
    .stop = modbus_stub_void,
    .deinit = modbus_stub_void,
    .poll = modbus_stub_void,
    .handle_command = modbus_stub_cmd,
};

#endif /* CONFIG_MINIHUB_MODBUS */
