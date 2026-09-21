/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_auth.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_auth";
static char s_token[65];
static uint8_t s_pw_sha[32];
static bool s_pw_set;
static char s_sess_hex[MINIHUB_AUTH_SESS_HEX_LEN + 1];
static int64_t s_sess_idle_until_us;
static int64_t s_sess_abs_until_us;
static char s_cookie_hdr[160];

static bool web_login_on(void)
{
#if CONFIG_MINIHUB_WEB_LOGIN_AUTH
    return true;
#else
    return false;
#endif
}

bool minihub_auth_web_login_enabled(void)
{
    return web_login_on();
}

static void sess_touch_idle(httpd_req_t *req)
{
    s_sess_idle_until_us = esp_timer_get_time() + (int64_t)MINIHUB_AUTH_SESS_SEC * 1000000LL;
    if (req) {
        minihub_auth_set_session_cookie(req, s_sess_hex);
    }
}

static void sess_start(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();
    s_sess_idle_until_us = now + (int64_t)MINIHUB_AUTH_SESS_SEC * 1000000LL;
    s_sess_abs_until_us = now + (int64_t)MINIHUB_AUTH_SESS_ABS_SEC * 1000000LL;
    if (req) {
        minihub_auth_set_session_cookie(req, s_sess_hex);
    }
}

static bool sess_alive(void)
{
    int64_t now = esp_timer_get_time();
    return s_sess_hex[0] && now <= s_sess_idle_until_us && now <= s_sess_abs_until_us;
}

static void sess_clear(httpd_req_t *req)
{
    s_sess_hex[0] = '\0';
    s_sess_idle_until_us = 0;
    s_sess_abs_until_us = 0;
    if (req) {
        minihub_auth_clear_session_cookie(req);
    }
}

static void hash_password(const char *password, uint8_t out[32])
{
    /* IDF 6 / mbedtls 4: mbedtls/sha256.h is no longer a public header. */
    (void)mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                     (const unsigned char *)password, strlen(password), out);
}

static bool ct_eq32(const uint8_t *a, const uint8_t *b)
{
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

static void bytes_to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[in[i] & 0xf];
    }
    out[n * 2] = '\0';
}

static bool cookie_mh_sess(httpd_req_t *req, char out[MINIHUB_AUTH_SESS_HEX_LEN + 1])
{
    size_t n = httpd_req_get_hdr_value_len(req, "Cookie");
    if (n == 0 || n > 512) {
        return false;
    }
    char *buf = malloc(n + 1);
    if (!buf) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, n + 1) != ESP_OK) {
        free(buf);
        return false;
    }
    const char *p = strstr(buf, "mh_sess=");
    bool ok = false;
    if (p) {
        p += 8;
        size_t i = 0;
        while (p[i] && p[i] != ';' && i < MINIHUB_AUTH_SESS_HEX_LEN) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        ok = (i == MINIHUB_AUTH_SESS_HEX_LEN);
    }
    free(buf);
    return ok;
}

static bool bearer_ok(httpd_req_t *req)
{
    if (s_token[0] == '\0') {
        return false;
    }
    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    const char prefix[] = "Bearer ";
    if (strncmp(auth_hdr, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    return strcmp(auth_hdr + sizeof(prefix) - 1, s_token) == 0;
}

esp_err_t minihub_auth_init(void)
{
    s_token[0] = '\0';
    s_sess_hex[0] = '\0';
    s_sess_idle_until_us = 0;
    s_sess_abs_until_us = 0;
    s_pw_set = false;
    memset(s_pw_sha, 0, sizeof(s_pw_sha));

#if CONFIG_MINIHUB_WEB_LOGIN_AUTH
    if (strlen(CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN) > 0) {
        strncpy(s_token, CONFIG_MINIHUB_WEB_LOGIN_AUTH_TOKEN, sizeof(s_token) - 1);
        ESP_LOGI(TAG, "login auth token loaded from Kconfig");
    } else {
        nvs_handle_t tok;
        if (nvs_open("minihub", NVS_READONLY, &tok) == ESP_OK) {
            size_t len = sizeof(s_token);
            if (nvs_get_str(tok, "api_token", s_token, &len) == ESP_OK) {
                ESP_LOGI(TAG, "login auth token loaded from NVS");
            }
            nvs_close(tok);
        }
    }
#endif

    nvs_handle_t nvs;
    if (nvs_open("minihub", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_pw_sha);
        if (nvs_get_blob(nvs, "web_pw", s_pw_sha, &len) == ESP_OK && len == sizeof(s_pw_sha)) {
            s_pw_set = true;
            ESP_LOGI(TAG, "web password hash loaded from NVS");
        }
        nvs_close(nvs);
    }
    return ESP_OK;
}

bool minihub_auth_password_set(void)
{
    return s_pw_set;
}

bool minihub_auth_enabled(void)
{
    if (web_login_on() && s_pw_set) {
        return true;
    }
    return s_token[0] != '\0';
}

esp_err_t minihub_auth_check(httpd_req_t *req)
{
    if (web_login_on()) {
        char sess[MINIHUB_AUTH_SESS_HEX_LEN + 1];
        if (s_sess_hex[0] && cookie_mh_sess(req, sess) && strcmp(sess, s_sess_hex) == 0) {
            if (!sess_alive()) {
                sess_clear(req);
            } else {
                /* Any authenticated use slides idle (GET polls count as presence). */
                sess_touch_idle(req);
                return ESP_OK;
            }
        }
        if (bearer_ok(req)) {
            return ESP_OK;
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (s_token[0] == '\0') {
        return ESP_OK;
    }
    return bearer_ok(req) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t minihub_auth_check_token(const char *token)
{
    if (!token) {
        return ESP_ERR_INVALID_STATE;
    }
    if (web_login_on() && s_sess_hex[0] && strcmp(token, s_sess_hex) == 0) {
        if (sess_alive()) {
            sess_touch_idle(NULL);
            return ESP_OK;
        }
        sess_clear(NULL);
    }
    if (s_token[0] && strcmp(token, s_token) == 0) {
        return ESP_OK;
    }
    if (!web_login_on() && s_token[0] == '\0') {
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t minihub_auth_setup_password(const char *username, const char *password)
{
    if (!web_login_on()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_pw_set) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!username || strcmp(username, MINIHUB_WEB_USERNAME) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!password || strlen(password) < 6 || strlen(password) > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t hash[32];
    hash_password(password, hash);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("minihub", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "web_pw", hash, sizeof(hash));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(s_pw_sha, hash, sizeof(hash));
    s_pw_set = true;
    ESP_LOGI(TAG, "web password saved to NVS");
    return ESP_OK;
}

esp_err_t minihub_auth_login(const char *username, const char *password,
                             char sess_hex[MINIHUB_AUTH_SESS_HEX_LEN + 1])
{
    if (!web_login_on()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_pw_set) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!username || strcmp(username, MINIHUB_WEB_USERNAME) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!password) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hash[32];
    hash_password(password, hash);
    if (!ct_eq32(hash, s_pw_sha)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    bytes_to_hex(rnd, sizeof(rnd), s_sess_hex);
    sess_start(NULL);
    if (sess_hex) {
        memcpy(sess_hex, s_sess_hex, MINIHUB_AUTH_SESS_HEX_LEN + 1);
    }
    return ESP_OK;
}

void minihub_auth_logout(void)
{
    sess_clear(NULL);
}

void minihub_auth_set_session_cookie(httpd_req_t *req, const char *sess_hex)
{
    snprintf(s_cookie_hdr, sizeof(s_cookie_hdr),
             "mh_sess=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
             sess_hex, MINIHUB_AUTH_SESS_SEC);
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
}

void minihub_auth_clear_session_cookie(httpd_req_t *req)
{
    snprintf(s_cookie_hdr, sizeof(s_cookie_hdr), "mh_sess=; Path=/; HttpOnly; Max-Age=0");
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
}
