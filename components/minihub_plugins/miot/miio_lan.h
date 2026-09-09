/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/** miIO LAN UDP port */
#define MIIO_LAN_PORT 54321

/** Max devices returned by one UDP broadcast scan */
#define MIIO_DISCOVER_MAX 24

/** Parse 32-char hex token into 16 bytes */
esp_err_t miio_token_from_hex(const char *hex, uint8_t token[16]);

/** One device seen on LAN via plaintext miio.info broadcast */
typedef struct {
    char ip[48];
    char model[64];
    char mac[24];
} miio_discovered_t;

/**
 * UDP broadcast discovery (binary miIO hello, same as python-miio/miiocli). Does not require token.
 * @param timeout_ms  Total listen window (typical 2000–3000)
 */
esp_err_t miio_lan_discover(miio_discovered_t *out, size_t max_out, size_t *found,
                            uint32_t timeout_ms);

/**
 * Match a device by token (+ optional model/mac) against discovery results.
 * Probes candidates with encrypted miio.info until one accepts the token.
 */
esp_err_t miio_lan_bind_ip(const uint8_t token[16], const char *model, const char *mac,
                           const miio_discovered_t *list, size_t list_count,
                           char *out_ip, size_t ip_len, uint32_t *miio_id);

/**
 * Verify token against a known IP (encrypted miio.info).
 */
esp_err_t miio_lan_probe(const char *ip, const uint8_t token[16], uint32_t *miio_id);

/**
 * MIoT-spec get_properties over LAN.
 * @param miio_id  device id from miIO header; use 0xFFFFFFFF until first reply updates it
 */
esp_err_t miio_lan_get_property(const char *ip, const uint8_t token[16], uint32_t *miio_id,
                                int siid, int piid, cJSON **out_value);

esp_err_t miio_lan_set_property(const char *ip, const uint8_t token[16], uint32_t *miio_id,
                              int siid, int piid, const cJSON *value);
