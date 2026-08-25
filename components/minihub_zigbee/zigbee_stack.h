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
#include "minihub_zigbee.h"

typedef void (*zigbee_stack_ready_cb_t)(void *ctx);

esp_err_t zigbee_stack_start(void);
esp_err_t zigbee_stack_stop(void);
esp_err_t zigbee_stack_permit_join(int seconds);
bool zigbee_stack_permit_join_active(void);
bool zigbee_stack_running(void);
esp_err_t zigbee_stack_onoff(uint16_t short_addr, uint8_t endpoint, minihub_zigbee_onoff_cmd_t cmd);

/**
 * Register a Zigbee 3.0 install code for a joiner IEEE.
 * @param ieee   64-bit extended address
 * @param ic     install code bytes including CRC (8/10/14/18)
 * @param ic_len length of @p ic
 */
esp_err_t zigbee_stack_add_install_code(uint64_t ieee, const uint8_t *ic, size_t ic_len);

/** Bind + configure reporting / IAS enroll / metering for sensor or metering caps. */
esp_err_t zigbee_stack_subscribe(uint16_t short_addr, uint8_t endpoint, uint64_t ieee, uint8_t caps);

/** Re-run ActiveEP/SimpleDesc interview (e.g. refresh metering caps after firmware upgrade). */
void zigbee_stack_rediscover(uint16_t short_addr, uint64_t ieee);

void zigbee_stack_set_hooks(minihub_zigbee_device_cb_t on_device,
                            minihub_zigbee_attr_cb_t on_attr,
                            zigbee_stack_ready_cb_t on_ready,
                            void *ctx);

void zigbee_stack_notify_device(const minihub_zigbee_device_t *dev);
void zigbee_stack_notify_attr(const minihub_zigbee_attr_evt_t *evt);

/** After device mapped: read quirk-declared ZCL attrs (Basic app/hw version, etc.). */
void zigbee_stack_subscribe_product_params(uint16_t short_addr, uint8_t endpoint, uint64_t ieee);

/** Write a ZCL attribute (for product_params access=rw). */
esp_err_t zigbee_stack_write_attr(uint16_t short_addr, uint8_t endpoint, uint16_t cluster_id,
                                  uint16_t attr_id, uint8_t attr_type, void *value, uint16_t value_size);
