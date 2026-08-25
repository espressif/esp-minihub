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

typedef enum {
    MINIHUB_NET_IF_ETH = 0,
    MINIHUB_NET_IF_WIFI = 1,
} minihub_net_iface_t;

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

typedef struct {
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gw[16];
    char dns_main[16];
    char dns_backup[16];
} minihub_net_iface_config_t;

esp_err_t minihub_net_get_iface_status(minihub_net_iface_t iface, minihub_net_iface_status_t *out);

/** Save to NVS and apply immediately. */
esp_err_t minihub_net_set_iface_config(minihub_net_iface_t iface, const minihub_net_iface_config_t *cfg);

/** Apply any NVS-saved configs (call after uplinks started). */
esp_err_t minihub_net_apply_saved_configs(void);

#ifdef __cplusplus
}
#endif
