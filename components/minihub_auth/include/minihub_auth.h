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
/** Idle timeout: no page navigation for this long → session ends. */
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
 * Read-only: never extends the session. Background polling must not keep a
 * console signed in — see minihub_auth_touch() for the navigation-based reset.
 * Absolute lifetime from login is never extended either way.
 */
esp_err_t minihub_auth_check(httpd_req_t *req);

/**
 * Slide the idle timer and re-issue the session cookie. Call only where a
 * browser actually loads a page (document navigation), so an auto-refreshing
 * tab still times out while a manual reload or a new page visit resets it.
 */
void minihub_auth_touch(httpd_req_t *req);

esp_err_t minihub_auth_check_token(const char *token);

/** First-time password (username must be admin). Fails if already set. */
esp_err_t minihub_auth_setup_password(const char *username, const char *password);

/**
 * Change the web password while signed in. username must be admin, the old
 * password must match, and the new password must be 6-64 chars.
 * Returns ESP_ERR_NOT_SUPPORTED if login auth is off, ESP_ERR_INVALID_STATE
 * if the password is unset or the old password is wrong, and
 * ESP_ERR_INVALID_ARG if the username or new password is invalid.
 */
esp_err_t minihub_auth_change_password(const char *username,
                                       const char *old_password,
                                       const char *new_password);

/**
 * Verify admin + password. On success fills ``sess_hex`` (64 hex chars + NUL)
 * and remembers the session in RAM.
 */
esp_err_t minihub_auth_login(const char *username, const char *password, char sess_hex[MINIHUB_AUTH_SESS_HEX_LEN + 1]);

void minihub_auth_logout(void);

void minihub_auth_set_session_cookie(httpd_req_t *req, const char *sess_hex);
void minihub_auth_clear_session_cookie(httpd_req_t *req);
