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
 * @brief MiniHub Host network uplink (Ethernet / Wi-Fi STA).
 *
 * Store modules may call exported minihub_net_* helpers; do not use esp_wifi_* directly.
 */

/**
 * @brief Event bit set when STA or Ethernet obtains IPv4.
 */
#define MINIHUB_NET_GOT_IP_BIT BIT0

/**
 * @brief Initialize netif/event loop and start Ethernet (if enabled).
 *
 * Wi-Fi STA connect is deferred to minihub_net_start_wifi() (wifi adapter start).
 *
 * @return
 *     - ESP_OK: Succeed.
 */
esp_err_t minihub_net_init(void);

/**
 * @brief Start Wi-Fi STA with NVS credentials (or provisioning).
 *
 * Invoked by the wifi Host adapter after minihub_adapter_start_all() mutex.
 *
 * @return
 *     - ESP_OK: Succeed.
 */
esp_err_t minihub_net_start_wifi(void);

/**
 * @brief True if any enabled uplink has a non-zero IPv4 address; false otherwise.
 */
bool minihub_net_has_ip(void);

/**
 * @brief Copy dotted IPv4 of the uplink that currently has an address.
 *
 * Ethernet first, else Wi-Fi. False if neither has IPv4.
 *
 * @param buf Output buffer.
 * @param len Buffer size.
 *
 * @return True if the address was written; false otherwise.
 */
bool minihub_net_get_ip(char *buf, size_t len);

/**
 * @brief Netif with IPv4: Ethernet first, else Wi-Fi, else NULL.
 *
 * @return esp_netif_t pointer, or NULL.
 */
esp_netif_t *minihub_net_get_netif(void);

/**
 * @brief Select the default netif from IPv4 uplinks.
 *
 * Ethernet if it has IPv4, else Wi-Fi if it has IPv4, else Ethernet (if built)
 * or Wi-Fi. Called on GOT_IP and link loss.
 */
void minihub_net_refresh_default_netif(void);

/**
 * @brief Log IPv4/gateway/DNS on the preferred netif, then optionally resolve.
 *
 * Safe to call from store modules (host-exported).
 *
 * @param hostname Host to resolve; NULL dumps DNS only.
 */
void minihub_net_dns_probe(const char *hostname);

/**
 * @brief Ethernet netif, or NULL if not built/started.
 */
esp_netif_t *minihub_net_eth_netif(void);

/**
 * @brief Wi-Fi STA netif, or NULL if not built/started.
 */
esp_netif_t *minihub_net_wifi_netif(void);

/**
 * @brief FreeRTOS event group; MINIHUB_NET_GOT_IP_BIT on IPv4.
 */
EventGroupHandle_t minihub_net_event_group(void);

/**
 * @brief Copy the STA SSID the hub will join. Never returns the password.
 *
 * @param ssid          Output buffer.
 * @param ssid_len      Buffer size.
 * @param has_password  Set when a password is stored; may be NULL.
 */
esp_err_t minihub_net_get_wifi_sta(char *ssid, size_t ssid_len, bool *has_password);

/**
 * @brief Copy STA SSID and password for the on-device UI (LCD).
 *
 * Prefer this over minihub_net_get_wifi_sta() when the local screen must show
 * the saved passphrase. Do not expose the password over the network.
 * Either buffer may be NULL to skip that field.
 */
esp_err_t minihub_net_get_wifi_credentials(char *ssid, size_t ssid_len,
                                           char *password, size_t password_len);

/**
 * @brief Save STA credentials and reconnect.
 *
 * An empty password keeps the stored one unless @p open_network is true.
 * The password is not logged.
 */
esp_err_t minihub_net_set_wifi_sta(const char *ssid, const char *password, bool open_network);

/**
 * @brief STA MAC, or base MAC when STA is unavailable.
 *
 * Lowercase colon-separated, for example `aa:bb:cc:dd:ee:ff`.
 * `out_len` must be at least 18.
 */
esp_err_t minihub_net_format_mac(char *out, size_t out_len);

/**
 * @brief Copy `url` and append `mac` when it is not already in the query.
 *
 * If the MAC cannot be read, `out` is a plain copy of `url`.
 */
esp_err_t minihub_net_copy_url_with_mac(const char *url, char *out, size_t out_len);

/**
 * @brief Append `chip=` and `board=` (when known) onto an in-place URL buffer.
 *
 * `chip` is lowercase `CONFIG_IDF_TARGET`. `board` is `CONFIG_MINIHUB_BOARD`
 * when that Kconfig is non-empty. Truncates quietly if `url_len` is too small.
 */
void minihub_net_append_chip_board_query(char *url, size_t url_len);

/**
 * @brief Heap-allocate `url` with mac, chip, board, and optional `cur_version`.
 *
 * Same fields the Store expects on `/api/catalog`, `/api/firmware`, and
 * `/files/…`. Caller frees the result with `free()`. Returns NULL on OOM or
 * when `url` is NULL.
 */
char *minihub_net_url_with_device_queries(const char *url, const char *cur_version);

#ifdef __cplusplus
}
#endif
