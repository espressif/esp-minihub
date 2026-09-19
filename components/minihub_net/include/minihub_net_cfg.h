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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Persistent Ethernet / Wi-Fi interface configuration (NVS)
 *
 * Used by REST POST /api/net/config. Modules should use minihub_net_* or REST, not esp_wifi_*.
 */

/**
 * @brief Network interface selector
 */
typedef enum {
    MINIHUB_NET_IF_ETH = 0,  /**< Ethernet uplink */
    MINIHUB_NET_IF_WIFI = 1, /**< Wi-Fi STA uplink */
} minihub_net_iface_t;

/**
 * @brief Runtime interface status snapshot
 */
typedef struct {
    bool present;
    bool up;
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gw[16];
    char dns_main[16];
    char dns_backup[16];
} minihub_net_iface_status_t;

/**
 * @brief Interface configuration to save or apply
 */
typedef struct {
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gw[16];
    char dns_main[16];
    char dns_backup[16];
} minihub_net_iface_config_t;

/**
 * @brief Read link/DHCP/address status for an interface
 *
 * @param iface Target interface
 * @param out   Output status
 */
esp_err_t minihub_net_get_iface_status(minihub_net_iface_t iface, minihub_net_iface_status_t *out);

/**
 * @brief Save interface config to NVS and apply immediately
 *
 * @param iface Target interface
 * @param cfg   Configuration
 */
esp_err_t minihub_net_set_iface_config(minihub_net_iface_t iface, const minihub_net_iface_config_t *cfg);

/**
 * @brief Re-apply NVS-saved configs after uplinks start
 */
esp_err_t minihub_net_apply_saved_configs(void);

#ifdef __cplusplus
}
#endif
