/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zigbee_stack.h"

esp_err_t zigbee_stack_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zigbee_stack_stop(void)
{
    return ESP_OK;
}

esp_err_t zigbee_stack_permit_join(int seconds)
{
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
}

bool zigbee_stack_permit_join_active(void)
{
    return false;
}

esp_err_t zigbee_stack_add_install_code(uint64_t ieee, const uint8_t *ic, size_t ic_len)
{
    (void)ieee;
    (void)ic;
    (void)ic_len;
    return ESP_ERR_NOT_SUPPORTED;
}

bool zigbee_stack_running(void)
{
    return false;
}

esp_err_t zigbee_stack_onoff(uint16_t short_addr, uint8_t endpoint, minihub_zigbee_onoff_cmd_t cmd)
{
    (void)short_addr;
    (void)endpoint;
    (void)cmd;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zigbee_stack_subscribe(uint16_t short_addr, uint8_t endpoint, uint64_t ieee, uint8_t caps)
{
    (void)short_addr;
    (void)endpoint;
    (void)ieee;
    (void)caps;
    return ESP_ERR_NOT_SUPPORTED;
}

void zigbee_stack_rediscover(uint16_t short_addr, uint64_t ieee)
{
    (void)short_addr;
    (void)ieee;
}

void zigbee_stack_subscribe_product_params(uint16_t short_addr, uint8_t endpoint, uint64_t ieee)
{
    (void)short_addr;
    (void)endpoint;
    (void)ieee;
}

esp_err_t zigbee_stack_write_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                  uint16_t attr_id, uint8_t attr_type, void *value, uint16_t value_size)
{
    (void)short_addr;
    (void)endpoint;
    (void)cluster_id;
    (void)attr_id;
    (void)attr_type;
    (void)value;
    (void)value_size;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t zigbee_stack_move_to_level(uint16_t short_addr, uint8_t endpoint, uint8_t level,
                                     uint16_t transition_time)
{
    (void)short_addr;
    (void)endpoint;
    (void)level;
    (void)transition_time;
    return ESP_ERR_NOT_SUPPORTED;
}

void zigbee_stack_set_hooks(minihub_zigbee_device_cb_t on_device, minihub_zigbee_attr_cb_t on_attr,
                            zigbee_stack_ready_cb_t on_ready, void *ctx)
{
    (void)on_device;
    (void)on_attr;
    (void)on_ready;
    (void)ctx;
}

void zigbee_stack_notify_device(const minihub_zigbee_device_t *dev)
{
    (void)dev;
}

void zigbee_stack_notify_attr(const minihub_zigbee_attr_evt_t *evt)
{
    (void)evt;
}
