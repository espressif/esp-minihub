/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool valid;
    double lat;
    double lon;
    char city[64];
    char region[64];
    char country[64];
    char public_ip[48];
} minihub_geo_t;

esp_err_t minihub_web_get_geo(minihub_geo_t *out);

/** Read cached geolocation only (never blocks on HTTP). */
esp_err_t minihub_web_peek_geo(minihub_geo_t *out);

/** True when STA netif exists and has a non-zero IPv4 address (safe for HTTP/DNS). */
bool minihub_web_sta_has_ip(void);

/** Copy STA IPv4 dotted string into buf (empty if unavailable). Returns true when written. */
bool minihub_web_get_sta_ip(char *buf, size_t len);

/** Re-fetch IP geolocation after network is up (async; safe from Wi-Fi event handlers). */
esp_err_t minihub_web_refresh_geo_async(void);
