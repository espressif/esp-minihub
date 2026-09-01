/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Defaults for native OpenThread radio (ESP32-S31 / IEEE 802.15.4).
 */

#pragma once

#include "esp_openthread_types.h"
#include "sdkconfig.h"

#if CONFIG_OPENTHREAD_RADIO_NATIVE
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG() \
    {                                         \
        .radio_mode = RADIO_MODE_NATIVE,      \
    }
#elif CONFIG_OPENTHREAD_RADIO_SPINEL_UART
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                  \
    {                                                          \
        .radio_mode = RADIO_MODE_UART_RCP,                     \
        .radio_uart_config = {                                 \
            .port = 1,                                         \
            .uart_config =                                     \
                {                                              \
                    .baud_rate = 460800,                       \
                    .data_bits = UART_DATA_8_BITS,             \
                    .parity = UART_PARITY_DISABLE,             \
                    .stop_bits = UART_STOP_BITS_1,             \
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,     \
                    .rx_flow_ctrl_thresh = 0,                  \
                    .source_clk = UART_SCLK_DEFAULT,           \
                },                                             \
            .rx_pin = 4,                                       \
            .tx_pin = 5,                                       \
        },                                                     \
    }
#else
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG() \
    {                                         \
        .radio_mode = RADIO_MODE_NATIVE,      \
    }
#endif

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()               \
    {                                                      \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE, \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG() \
    {                                        \
        .storage_partition_name = "nvs",     \
        .netif_queue_size = 10,              \
        .task_queue_size = 10,               \
    }
