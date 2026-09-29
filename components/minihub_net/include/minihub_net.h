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

/**
 * @brief MiniHub Host network uplink (Ethernet / Wi-Fi STA)
 *
 * Store modules may call exported minihub_net_* helpers; do not use esp_wifi_* directly.
 */

/**
 * @brief Event bit set when STA or Ethernet obtains IPv4
 */
#define MINIHUB_NET_GOT_IP_BIT BIT0

/**
 * @brief Initialize netif/event loop and start Ethernet (if enabled)
 *
 * Wi-Fi STA connect is deferred to minihub_net_start_wifi() (wifi adapter start).
 *
 * @return
 *     - ESP_OK: succeed
 */
esp_err_t minihub_net_init(void);

/**
 * @brief Start Wi-Fi STA with NVS credentials (or provisioning)
 *
 * Invoked by the wifi Host adapter after minihub_adapter_start_all() mutex.
 *
 * @return
 *     - ESP_OK: succeed
 */
esp_err_t minihub_net_start_wifi(void);

/**
 * @brief True if any enabled uplink has a non-zero IPv4 address
 */
bool minihub_net_has_ip(void);

/**
 * @brief Copy dotted IPv4 of the uplink that currently has an address
 *
 * Ethernet first, else Wi-Fi. False if neither has IPv4.
 *
 * @param buf Output buffer
 * @param len Buffer size
 *
 * @return true if IP was written
 */
bool minihub_net_get_ip(char *buf, size_t len);

/**
 * @brief Netif with IPv4: Ethernet first, else Wi-Fi, else NULL
 *
 * @return esp_netif_t pointer, or NULL
 */
esp_netif_t *minihub_net_get_netif(void);

/**
 * @brief Select the default netif from IPv4 uplinks
 *
 * Ethernet if it has IPv4, else Wi-Fi if it has IPv4, else Ethernet (if built)
 * or Wi-Fi. Called on GOT_IP and link loss.
 */
void minihub_net_refresh_default_netif(void);

/**
 * @brief Log IPv4/gateway/DNS on the preferred netif, then optionally resolve
 *
 * Safe to call from store modules (host-exported).
 *
 * @param hostname Host to resolve; NULL dumps DNS only
 */
void minihub_net_dns_probe(const char *hostname);

/**
 * @brief Ethernet netif, or NULL if not built/started
 */
esp_netif_t *minihub_net_eth_netif(void);

/**
 * @brief Wi-Fi STA netif, or NULL if not built/started
 */
esp_netif_t *minihub_net_wifi_netif(void);

/**
 * @brief FreeRTOS event group; MINIHUB_NET_GOT_IP_BIT on IPv4
 */
EventGroupHandle_t minihub_net_event_group(void);

/**
 * @brief Copy the STA SSID the hub will join. Never returns the password.
 *
 * @param ssid          Output buffer
 * @param ssid_len      Buffer size
 * @param has_password  Set when a password is stored; may be NULL
 */
esp_err_t minihub_net_get_wifi_sta(char *ssid, size_t ssid_len, bool *has_password);

/**
 * @brief Save STA credentials and reconnect.
 *
 * An empty password keeps the stored one unless @p open_network is true.
 * The password is not logged.
 */
esp_err_t minihub_net_set_wifi_sta(const char *ssid, const char *password, bool open_network);

#ifdef __cplusplus
}
#endif
