/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

typedef struct {
    char *body;
    size_t len;
    size_t cap;
    /** If >0, refuse to grow past this many payload bytes (JPEG cap). */
    size_t max_len;
} miot_http_buf_t;

/** Optional response headers captured during perform(). */
typedef struct {
    char ext_pragma[512];
    char location[4096];
    int http_status;
} miot_http_extra_t;

/** Fill HTTPS client config: custom CA for account.xiaomi.com, crt bundle elsewhere. */
void miot_http_client_config_init(esp_http_client_config_t *cfg, const char *url);

/** Percent-encode a form field value (RFC 3986 unreserved + form specials). */
size_t miot_http_urlencode(const char *src, char *dst, size_t dst_len);

typedef esp_err_t (*miot_http_worker_fn_t)(void *ctx);
typedef void (*miot_http_worker_done_cb_t)(esp_err_t result, void *user_ctx);

/** Run HTTPS work on a dedicated task (avoids httpd stack overflow). Blocks until complete. */
esp_err_t miot_http_run_worker(miot_http_worker_fn_t fn, void *ctx);

/** Queue HTTPS work; callback runs on the worker task when finished (does not block caller). */
esp_err_t miot_http_run_worker_async(miot_http_worker_fn_t fn, void *ctx, miot_http_worker_done_cb_t cb,
                                     void *user_ctx);

/** Merge Set-Cookie name=value into a semicolon-separated cookie jar. */
void miot_http_cookie_jar_merge(char *jar, size_t jar_len, const char *set_cookie);

/** Set/replace a cookie in the jar (supports long passToken / identity_session). */
void miot_http_cookie_jar_put(char *jar, size_t jar_len, const char *name, const char *value);

/** Like miot_http_cookie_jar_put but allows values up to 1024 bytes (passToken). */
void miot_http_cookie_jar_put_long(char *jar, size_t jar_len, const char *name, const char *value);

/**
 * HTTPS request with optional cookie jar (send + merge Set-Cookie).
 * Pass cookie_jar=NULL to skip cookies.
 */
/** POST to api.io.mi.com (sets x-xiaomi-protocal-flag-cli). cookie_hdr is full Cookie value. */
esp_err_t miot_http_post_cloud(const char *url, const char *post_body,
                               const char *user_agent, const char *cookie_hdr,
                               miot_http_buf_t *resp);

/**
 * Same as miot_http_post_cloud, plus MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4
 * and Accept-Encoding: identity (required for RC4-encrypted API bodies).
 */
esp_err_t miot_http_post_cloud_rc4(const char *url, const char *post_body,
                                   const char *user_agent, const char *cookie_hdr,
                                   miot_http_buf_t *resp);

/**
 * GET with MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4 and Accept-Encoding: identity.
 * `url` is the full URL including signed query. Honors resp->max_len.
 * Redirect hops drop the RC4 header (processor → app.processor ticket URLs).
 */
esp_err_t miot_http_get_cloud_rc4(const char *url, const char *user_agent,
                                  const char *cookie_hdr, miot_http_buf_t *resp);

/**
 * GET a signed camera/CDN URL with cookies only (no ENCRYPT-RC4).
 * Matches HA/ffmpeg: query already carries RC4 params; body is AES JPEG.
 */
esp_err_t miot_http_get_cloud(const char *url, const char *user_agent,
                              const char *cookie_hdr, miot_http_buf_t *resp);

/** Last GET failure detail (redirect Location prefix, HTTP status). */
const char *miot_http_last_get_error(void);

esp_err_t miot_http_request(const char *url, esp_http_client_method_t method,
                            const char *post_body, const char *user_agent,
                            char *cookie_jar, size_t cookie_jar_len,
                            miot_http_buf_t *resp,
                            char *set_cookie_out, size_t set_cookie_len);

/** Same as miot_http_request; optionally captures Location / extension-pragma. */
esp_err_t miot_http_request_ex(const char *url, esp_http_client_method_t method,
                               const char *post_body, const char *user_agent,
                               char *cookie_jar, size_t cookie_jar_len,
                               miot_http_buf_t *resp,
                               char *set_cookie_out, size_t set_cookie_len,
                               miot_http_extra_t *extra, bool no_auto_redirect);

/** Xiaomi identity/2FA requests (Content-Type on GET+POST, optional manual redirects). */
esp_err_t miot_http_request_identity(const char *url, esp_http_client_method_t method,
                                     const char *post_body, const char *user_agent,
                                     char *cookie_jar, size_t cookie_jar_len,
                                     miot_http_buf_t *resp, miot_http_extra_t *extra,
                                     bool no_auto_redirect);
