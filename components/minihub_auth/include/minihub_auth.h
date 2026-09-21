/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define MINIHUB_WEB_USERNAME "admin"
#define MINIHUB_AUTH_SESS_HEX_LEN 64
/** Idle timeout: no authenticated request for this long → session ends. */
#define MINIHUB_AUTH_SESS_SEC 300
/** Absolute cap from login (even if the SPA keeps polling). */
#define MINIHUB_AUTH_SESS_ABS_SEC (8 * 3600)

esp_err_t minihub_auth_init(void);

/** CONFIG_MINIHUB_WEB_LOGIN_AUTH: browser must use admin + NVS password. */
bool minihub_auth_web_login_enabled(void);

/** Bearer token from Kconfig/NVS, or web login is on and a password is stored. */
bool minihub_auth_enabled(void);

/** SHA-256 of the web password is present in NVS. */
bool minihub_auth_password_set(void);

/**
 * Allow the request: valid ``mh_sess`` cookie, or ``Authorization: Bearer``
 * when a Hub API token is configured.
 *
 * On success, slides the **idle** timer (so an open console that polls stays
 * signed in). Absolute lifetime from login is never extended.
 */
esp_err_t minihub_auth_check(httpd_req_t *req);

esp_err_t minihub_auth_check_token(const char *token);

/** First-time password (username must be admin). Fails if already set. */
esp_err_t minihub_auth_setup_password(const char *username, const char *password);

/**
 * Verify admin + password. On success fills ``sess_hex`` (64 hex chars + NUL)
 * and remembers the session in RAM.
 */
esp_err_t minihub_auth_login(const char *username, const char *password, char sess_hex[MINIHUB_AUTH_SESS_HEX_LEN + 1]);

void minihub_auth_logout(void);

void minihub_auth_set_session_cookie(httpd_req_t *req, const char *sess_hex);
void minihub_auth_clear_session_cookie(httpd_req_t *req);
