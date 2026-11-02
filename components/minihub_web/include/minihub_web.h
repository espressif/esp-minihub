/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "minihub_geo.h"
#include "minihub_map.h"

/**
 * @brief Attach MiniHub REST/SPA routes to an already-running httpd.
 * Does not start or stop the server.
 *
 * Host `minihub_web_start()` calls this with defaults: index `/`, favicon and
 * WebSocket on. Another firmware can pass a different URI or skip favicon/WS
 * when the host httpd already owns those.
 *
 * String pointers must remain valid for the life of the httpd.
 */
typedef struct {
    const char *index_uri;   /*!< Console URI; NULL means "/". */
    const char *index_path;  /*!< Extra index.html after preinstall; NULL = preinstall only. */
    bool register_favicon;   /*!< True if the host httpd does not already serve `/favicon.ico`; false otherwise. */
    bool start_websocket;    /*!< True if the socket budget is not tight; false otherwise. */
} minihub_web_attach_config_t;

esp_err_t minihub_web_start(uint16_t port);
esp_err_t minihub_web_attach(void *httpd, const minihub_web_attach_config_t *cfg);
esp_err_t minihub_web_ws_start(void *server);
/**
 * @brief Running httpd handle, or NULL if not started.
 */
void *minihub_web_httpd(void);
