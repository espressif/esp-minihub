/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINIHUB_NET_GOT_IP_BIT BIT0

/**
 * Initialize netif/event loop and start Ethernet (if enabled).
 * Wi-Fi driver init, STA connect, and provisioning are deferred to
 * minihub_net_start_wifi() so Zigbee / BLE Mesh can claim internal BT heap
 * before esp_wifi_init().
 */
esp_err_t minihub_net_init(void);

/**
 * Start Wi-Fi STA with Kconfig/NVS credentials, or BLE/SoftAP provisioning.
 * Call after Zigbee is ready when both share the radio (customer RF profile).
 */
esp_err_t minihub_net_start_wifi(void);

/** True if any enabled uplink has a non-zero IPv4 address. */
bool minihub_net_has_ip(void);

/**
 * Copy dotted IPv4 of the uplink that currently has an address
 * (Ethernet first, else Wi-Fi). False if neither has IPv4.
 */
bool minihub_net_get_ip(char *buf, size_t len);

/**
 * Netif with IPv4: Ethernet first, else Wi-Fi, else NULL.
 */
esp_netif_t *minihub_net_get_netif(void);

/**
 * Select the default netif: Ethernet if it has IPv4, else Wi-Fi if it has IPv4,
 * else Ethernet (if built) or Wi-Fi. Called on GOT_IP and link loss.
 */
void minihub_net_refresh_default_netif(void);

/**
 * Log IPv4/gateway/DNS servers on the preferred netif, then resolve hostname
 * via getaddrinfo. Safe to call from store modules (host-exported).
 * hostname may be NULL to only dump DNS config.
 */
void minihub_net_dns_probe(const char *hostname);

/** Netif handles for config UI (NULL if uplink not built/started). */
esp_netif_t *minihub_net_eth_netif(void);
esp_netif_t *minihub_net_wifi_netif(void);

EventGroupHandle_t minihub_net_event_group(void);

#ifdef __cplusplus
}
#endif
