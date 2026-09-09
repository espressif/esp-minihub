/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "miot_cloud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "miot_http.h"
#include "miot_aes128.h"
#include "nvs.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "miot_cloud";

static char s_last_error[160];
static char s_last_verify_url[MIOT_2FA_URL_MAX];

#define MIOT_LOGIN_USER_MAX 128
#define MIOT_LOGIN_PASS_MAX 128

static struct {
    bool active;
    char user[MIOT_LOGIN_USER_MAX];
    char pass[MIOT_LOGIN_PASS_MAX];
    char qs[512];
    char sign[128];
    char sid[32];
    char callback[192];
} s_login_captcha;

bool miot_cloud_has_pending_login_captcha(void)
{
    return s_login_captcha.active;
}

void miot_cloud_clear_pending_login_captcha(void)
{
    memset(&s_login_captcha, 0, sizeof(s_login_captcha));
}

static void passport_ua(char *ua, size_t ua_len)
{
    if (!ua || ua_len == 0) {
        return;
    }
    strncpy(ua, MIOT_PASSPORT_UA, ua_len - 1);
    ua[ua_len - 1] = '\0';
}

static void passport_cookie_jar(miot_cloud_session_t *session, char *jar, size_t jar_len)
{
    snprintf(jar, jar_len, "sdkVersion=%s; deviceId=%s", MIOT_PASSPORT_SDK_VERSION,
             session->device_id);
}

const char *miot_cloud_last_verify_url(void)
{
    return s_last_verify_url[0] ? s_last_verify_url : NULL;
}

static void miot_cloud_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_error, sizeof(s_last_error), fmt, ap);
    va_end(ap);
}

const char *miot_cloud_last_error(void)
{
    return s_last_error[0] ? s_last_error : NULL;
}

void miot_cloud_set_last_error(const char *message)
{
    if (!message) {
        s_last_error[0] = '\0';
        return;
    }
    strncpy(s_last_error, message, sizeof(s_last_error) - 1);
    s_last_error[sizeof(s_last_error) - 1] = '\0';
}

static void log_body_preview(const char *label, const char *body)
{
    if (!body) {
        ESP_LOGW(TAG, "%s: (null body)", label);
        return;
    }
    char preview[129];
    strncpy(preview, body, sizeof(preview) - 1);
    preview[sizeof(preview) - 1] = '\0';
    for (char *p = preview; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = ' ';
        }
    }
    ESP_LOGW(TAG, "%s: %s", label, preview);
}

static const char *xiaomi_json_desc(const cJSON *obj)
{
    const cJSON *desc = cJSON_GetObjectItem(obj, "desc");
    if (cJSON_IsString(desc) && desc->valuestring[0]) {
        return desc->valuestring;
    }
    desc = cJSON_GetObjectItem(obj, "description");
    if (cJSON_IsString(desc) && desc->valuestring[0]) {
        return desc->valuestring;
    }
    return NULL;
}

static bool xiaomi_json_has_error(const cJSON *obj)
{
    const cJSON *code = cJSON_GetObjectItem(obj, "code");
    return cJSON_IsNumber(code) && code->valueint != 0;
}

/** Spec prop/action item codes: 0 and 1 are both success (HA / Mi Home). */
static bool miot_spec_item_code_ok(const cJSON *code)
{
    if (!cJSON_IsNumber(code)) {
        return true; /* missing code → treat as ok (legacy empty result) */
    }
    return code->valueint == 0 || code->valueint == 1;
}

static const char *UA_FMT =
    "Android-7.1.1-1.0.0-ONEPLUS A3010-136-%s APP/xiaomi.smarthome APPV/62830";

static esp_err_t b64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_len)
{
    size_t olen = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_len, &olen, in, in_len);
    if (ret != 0) {
        return ESP_FAIL;
    }
    out[olen] = '\0';
    return ESP_OK;
}

static esp_err_t b64_decode(const char *in, uint8_t *out, size_t out_len, size_t *out_written)
{
    size_t olen = 0;
    int ret = mbedtls_base64_decode(out, out_len, &olen, (const unsigned char *)in, strlen(in));
    if (ret != 0) {
        return ESP_FAIL;
    }
    if (out_written) {
        *out_written = olen;
    }
    return ESP_OK;
}

static void ensure_device_id(miot_cloud_session_t *session)
{
    if (session->device_id[0] != '\0') {
        return;
    }
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 16; i++) {
        session->device_id[i] = chars[esp_random() % (sizeof(chars) - 1)];
    }
    session->device_id[16] = '\0';
}

static void api_base(const char *region, char *out, size_t out_len)
{
    if (!region || region[0] == '\0' || strcmp(region, "cn") == 0) {
        snprintf(out, out_len, "https://api.io.mi.com/app");
    } else {
        snprintf(out, out_len, "https://%s.api.io.mi.com/app", region);
    }
}

esp_err_t miot_cloud_gen_nonce(char *out, size_t out_len)
{
    if (!out || out_len < 32) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t raw[12];
    esp_fill_random(raw, 8);
    time_t t = time(NULL);
    uint32_t mins = (uint32_t)(t / 60);
    raw[8] = (uint8_t)((mins >> 24) & 0xff);
    raw[9] = (uint8_t)((mins >> 16) & 0xff);
    raw[10] = (uint8_t)((mins >> 8) & 0xff);
    raw[11] = (uint8_t)(mins & 0xff);
    return b64_encode(raw, sizeof(raw), out, out_len);
}

static esp_err_t sign_nonce(const char *ssecurity, const char *nonce, char *snonce_out, size_t snonce_len)
{
    uint8_t sec_dec[64];
    uint8_t nonce_dec[32];
    size_t sec_len = 0;
    size_t nonce_len_dec = 0;
    if (b64_decode(ssecurity, sec_dec, sizeof(sec_dec), &sec_len) != ESP_OK) {
        return ESP_FAIL;
    }
    if (b64_decode(nonce, nonce_dec, sizeof(nonce_dec), &nonce_len_dec) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t hash[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) {
        return ESP_FAIL;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, md, 0) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, sec_dec, sec_len);
    mbedtls_md_update(&ctx, nonce_dec, nonce_len_dec);
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    return b64_encode(hash, sizeof(hash), snonce_out, snonce_len);
}

esp_err_t miot_cloud_sign_request(const char *uri, const char *data_json,
                                  const char *ssecurity, miot_cloud_sign_t *sign)
{
    if (!uri || !data_json || !ssecurity || !sign) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = miot_cloud_gen_nonce(sign->nonce, sizeof(sign->nonce));
    if (err != ESP_OK) {
        return err;
    }

    char snonce[64];
    err = sign_nonce(ssecurity, sign->nonce, snonce, sizeof(snonce));
    if (err != ESP_OK) {
        return err;
    }

    char msg[768];
    snprintf(msg, sizeof(msg), "%s&%s&%s&data=%s", uri, snonce, sign->nonce, data_json);

    uint8_t snonce_dec[64];
    size_t snonce_len = 0;
    if (b64_decode(snonce, snonce_dec, sizeof(snonce_dec), &snonce_len) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t hmac_out[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        snonce_dec, snonce_len,
                        (const uint8_t *)msg, strlen(msg),
                        hmac_out) != 0) {
        return ESP_FAIL;
    }
    return b64_encode(hmac_out, sizeof(hmac_out), sign->signature, sizeof(sign->signature));
}

static cJSON *parse_xiaomi_json(const char *raw)
{
    if (!raw) {
        return NULL;
    }
    const char *start = raw;
    if (strncmp(raw, "&&&START&&&", 11) == 0) {
        start = raw + 11;
    }
    return cJSON_Parse(start);
}

static void json_cookie_put(cJSON *obj, const char *key, char *jar, size_t jar_len)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!item) {
        return;
    }
    if (cJSON_IsString(item) && item->valuestring[0]) {
        if (strcmp(key, "passToken") == 0) {
            miot_http_cookie_jar_put_long(jar, jar_len, key, item->valuestring);
            return;
        }
        miot_http_cookie_jar_put(jar, jar_len, key, item->valuestring);
        return;
    }
    char val[512];
    val[0] = '\0';
    if (cJSON_IsNumber(item)) {
        snprintf(val, sizeof(val), "%.0f", item->valuedouble);
    }
    if (val[0]) {
        miot_http_cookie_jar_put(jar, jar_len, key, val);
    }
}

static esp_err_t cloud_http(const char *url, esp_http_client_method_t method,
                            const char *post_body, const char *user_agent,
                            char *cookie_jar, size_t cookie_jar_len,
                            miot_http_buf_t *resp,
                            char *set_cookie_out, size_t set_cookie_len)
{
    esp_err_t err = miot_http_request(url, method, post_body, user_agent, cookie_jar,
                                      cookie_jar_len, resp, set_cookie_out, set_cookie_len);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            miot_cloud_set_error("HTTP request failed");
        } else {
            miot_cloud_set_error("HTTP connect failed: %s", esp_err_to_name(err));
        }
        return err;
    }
    if (resp && (!resp->body || resp->body[0] == '\0')) {
        ESP_LOGW(TAG, "empty body from %s", url);
        miot_cloud_set_error("empty HTTP response");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t extract_cookie_value(const char *set_cookie, const char *name,
                                      char *out, size_t out_len)
{
    if (!set_cookie || !name || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    const char *p = strstr(set_cookie, pattern);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    p += strlen(pattern);
    size_t i = 0;
    while (p[i] && p[i] != ';' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t miot_cloud_session_load(miot_cloud_session_t *session)
{
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(session, 0, sizeof(*session));
    strncpy(session->region, "cn", sizeof(session->region) - 1);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(session->user_id);
    if (nvs_get_str(nvs, "user_id", session->user_id, &len) != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    len = sizeof(session->service_token);
    if (nvs_get_str(nvs, "service_token", session->service_token, &len) != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    len = sizeof(session->ssecurity);
    if (nvs_get_str(nvs, "ssecurity", session->ssecurity, &len) != ESP_OK) {
        nvs_close(nvs);
        return ESP_ERR_NOT_FOUND;
    }
    len = sizeof(session->device_id);
    nvs_get_str(nvs, "device_id", session->device_id, &len);
    len = sizeof(session->region);
    nvs_get_str(nvs, "region", session->region, &len);
    uint8_t pinned = 0;
    if (nvs_get_u8(nvs, "dev_id_pin", &pinned) == ESP_OK) {
        session->device_id_pinned = pinned != 0;
    }

    nvs_close(nvs);
    session->valid = true;
    return ESP_OK;
}

esp_err_t miot_cloud_session_finalize(miot_cloud_session_t *session)
{
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_device_id(session);
    session->device_id_pinned = true;
    return ESP_OK;
}

esp_err_t miot_cloud_session_save(const miot_cloud_session_t *session)
{
    if (!session || !session->valid) {
        return ESP_ERR_INVALID_ARG;
    }
    miot_cloud_session_t copy = *session;
    if (copy.device_id_pinned && copy.device_id[0] == '\0') {
        ensure_device_id(&copy);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(nvs, "user_id", copy.user_id);
    nvs_set_str(nvs, "service_token", copy.service_token);
    nvs_set_str(nvs, "ssecurity", copy.ssecurity);
    nvs_set_str(nvs, "device_id", copy.device_id);
    nvs_set_str(nvs, "region", copy.region);
    nvs_set_u8(nvs, "dev_id_pin", copy.device_id_pinned ? 1 : 0);
    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void miot_cloud_session_clear(miot_cloud_session_t *session)
{
    if (session) {
        memset(session, 0, sizeof(*session));
    }
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/** Drop credentials only (keep pending 2FA / other keys). */
void miot_cloud_session_invalidate(miot_cloud_session_t *session)
{
    if (session) {
        session->valid = false;
        session->service_token[0] = '\0';
        session->ssecurity[0] = '\0';
        session->user_id[0] = '\0';
    }
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, "user_id");
    nvs_erase_key(nvs, "service_token");
    nvs_erase_key(nvs, "ssecurity");
    nvs_commit(nvs);
    nvs_close(nvs);
}

bool miot_cloud_session_valid(const miot_cloud_session_t *session)
{
    return session && session->valid && session->user_id[0] && session->service_token[0] &&
           session->ssecurity[0];
}

static esp_err_t save_login_captcha_pending(const char *username, const char *password,
                                            const char *qs, const char *sign, const char *sid,
                                            const char *callback)
{
    if (!username || !password || !qs || !sign || !sid) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_login_captcha, 0, sizeof(s_login_captcha));
    strncpy(s_login_captcha.user, username, sizeof(s_login_captcha.user) - 1);
    strncpy(s_login_captcha.pass, password, sizeof(s_login_captcha.pass) - 1);
    strncpy(s_login_captcha.qs, qs, sizeof(s_login_captcha.qs) - 1);
    strncpy(s_login_captcha.sign, sign, sizeof(s_login_captcha.sign) - 1);
    strncpy(s_login_captcha.sid, sid, sizeof(s_login_captcha.sid) - 1);
    if (callback && callback[0]) {
        strncpy(s_login_captcha.callback, callback, sizeof(s_login_captcha.callback) - 1);
    }
    s_login_captcha.active = true;
    return ESP_OK;
}

static esp_err_t handle_login_captcha_required(miot_cloud_session_t *session, cJSON *step2,
                                               const char *username, const char *password,
                                               const char *qs, const char *sign, const char *sid,
                                               const char *callback, char *jar)
{
    const cJSON *captcha = cJSON_GetObjectItem(step2, "captchaUrl");
    if (!cJSON_IsString(captcha) || !captcha->valuestring[0]) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "login needs captcha: %s", captcha->valuestring);
    esp_err_t err = miot_cloud_fetch_passport_captcha(session, captcha->valuestring, jar, 2048);
    if (err != ESP_OK) {
        miot_cloud_set_error("fetch login captcha failed");
        return err;
    }
    save_login_captcha_pending(username, password, qs, sign, sid, callback);
    miot_cloud_set_error("需要图形验证码");
    return ESP_ERR_INVALID_STATE;
}

esp_err_t miot_cloud_login_captcha_retry(const char *capt_code, miot_cloud_session_t *session,
                                         char *verify_url, size_t verify_url_len)
{
    if (!capt_code || !capt_code[0] || !s_login_captcha.active || !session) {
        return ESP_ERR_INVALID_STATE;
    }
    return miot_cloud_login(s_login_captcha.user, s_login_captcha.pass, capt_code, session,
                            verify_url, verify_url_len);
}

esp_err_t miot_cloud_login(const char *username, const char *password, const char *capt_code,
                           miot_cloud_session_t *session, char *verify_url, size_t verify_url_len)
{
    if (!username || !password || !session) {
        return ESP_ERR_INVALID_ARG;
    }
    if (verify_url && verify_url_len > 0) {
        verify_url[0] = '\0';
    }
    s_last_verify_url[0] = '\0';
    s_last_error[0] = '\0';

    const bool captcha_retry = (capt_code && capt_code[0] && s_login_captcha.active);
    if (!captcha_retry) {
        miot_cloud_clear_pending_login_captcha();
        miot_cloud_clear_pending_2fa();
    }

    ensure_device_id(session);
    char ua[256];
    passport_ua(ua, sizeof(ua));

    char jar[2048];
    passport_cookie_jar(session, jar, sizeof(jar));

    const char *qs_val = NULL;
    const char *sign_val = NULL;
    const char *sid_val = NULL;
    const char *callback_val = NULL;
    cJSON *step1 = NULL;

    if (captcha_retry) {
        qs_val = s_login_captcha.qs;
        sign_val = s_login_captcha.sign;
        sid_val = s_login_captcha.sid;
        callback_val = s_login_captcha.callback[0] ? s_login_captcha.callback : NULL;
        username = s_login_captcha.user;
        password = s_login_captcha.pass;
        ESP_LOGI(TAG, "login captcha retry user=%s password=%s", username, password);
    } else {
        miot_http_buf_t resp = {0};
        char url[256];
        snprintf(url, sizeof(url),
                 "https://account.xiaomi.com/pass/serviceLogin?sid=xiaomiio&_json=true");

        esp_err_t err = cloud_http(url, HTTP_METHOD_GET, NULL, ua, jar, sizeof(jar), &resp, NULL, 0);
        if (err != ESP_OK || !resp.body) {
            free(resp.body);
            return err != ESP_OK ? err : ESP_FAIL;
        }
        ESP_LOGI(TAG, "serviceLogin ok, cookie jar %u bytes", (unsigned)strlen(jar));

        step1 = parse_xiaomi_json(resp.body);
        if (!step1) {
            log_body_preview("serviceLogin parse failed", resp.body);
            free(resp.body);
            miot_cloud_set_error("invalid serviceLogin JSON");
            return ESP_FAIL;
        }
        free(resp.body);
        resp.body = NULL;

        cJSON *qs = cJSON_GetObjectItem(step1, "qs");
        cJSON *sign = cJSON_GetObjectItem(step1, "_sign");
        cJSON *sid = cJSON_GetObjectItem(step1, "sid");
        cJSON *callback = cJSON_GetObjectItem(step1, "callback");
        if (!cJSON_IsString(qs) || !cJSON_IsString(sign) || !cJSON_IsString(sid)) {
            char *raw = cJSON_PrintUnformatted(step1);
            log_body_preview("serviceLogin missing fields", raw);
            free(raw);
            cJSON_Delete(step1);
            miot_cloud_set_error("serviceLogin missing qs/_sign/sid");
            return ESP_FAIL;
        }
        qs_val = qs->valuestring;
        sign_val = sign->valuestring;
        sid_val = sid->valuestring;
        callback_val = (cJSON_IsString(callback) && callback->valuestring[0]) ? callback->valuestring
                                                                              : NULL;
    }

    unsigned char md5_out[16];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), (const unsigned char *)password,
                   strlen(password), md5_out) != 0) {
        cJSON_Delete(step1);
        return ESP_FAIL;
    }
    char hash_hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(hash_hex + i * 2, 3, "%02X", md5_out[i]);
    }

    char user_enc[128];
    char sign_enc[96];
    char cb_enc[192];
    char capt_enc[32];
    miot_http_urlencode(username, user_enc, sizeof(user_enc));
    miot_http_urlencode(sign_val, sign_enc, sizeof(sign_enc));
    if (callback_val) {
        miot_http_urlencode(callback_val, cb_enc, sizeof(cb_enc));
    } else {
        cb_enc[0] = '\0';
    }
    if (capt_code && capt_code[0]) {
        miot_http_urlencode(capt_code, capt_enc, sizeof(capt_enc));
    } else {
        capt_enc[0] = '\0';
    }

    char p2fa_qs[512] = {0};
    char p2fa_sign[128] = {0};
    strncpy(p2fa_qs, qs_val, sizeof(p2fa_qs) - 1);
    strncpy(p2fa_sign, sign_val, sizeof(p2fa_sign) - 1);

    char body[2048];
    size_t body_len = 0;
    if (callback_val) {
        body_len = snprintf(body, sizeof(body),
                            "sid=%s&qs=%s&callback=%s&_sign=%s&user=%s&hash=%s&_json=true",
                            sid_val, qs_val, cb_enc, sign_enc, user_enc, hash_hex);
    } else {
        body_len = snprintf(body, sizeof(body), "sid=%s&qs=%s&_sign=%s&user=%s&hash=%s&_json=true",
                            sid_val, qs_val, sign_enc, user_enc, hash_hex);
    }
    if (capt_enc[0]) {
        if (body_len + 10 + strlen(capt_enc) < sizeof(body)) {
            snprintf(body + body_len, sizeof(body) - body_len, "&captCode=%s", capt_enc);
        }
    }

    char url[256];
    snprintf(url, sizeof(url), "https://account.xiaomi.com/pass/serviceLoginAuth2");
    ESP_LOGI(TAG, "serviceLoginAuth2 POST body %u bytes, cookie jar %u bytes captcha=%d",
             (unsigned)strlen(body), (unsigned)strlen(jar), capt_enc[0] != '\0');

    miot_http_buf_t resp = {0};
    esp_err_t err = cloud_http(url, HTTP_METHOD_POST, body, ua, jar, sizeof(jar), &resp, NULL, 0);
    ESP_LOGI(TAG, "serviceLoginAuth2 cookie jar %u bytes after HTTP", (unsigned)strlen(jar));
    cJSON_Delete(step1);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *step2 = parse_xiaomi_json(resp.body);
    if (!step2) {
        log_body_preview("serviceLoginAuth2 parse failed", resp.body);
        free(resp.body);
        miot_cloud_set_error("invalid serviceLoginAuth2 JSON");
        return ESP_FAIL;
    }
    free(resp.body);
    resp.body = NULL;

    const cJSON *notification = cJSON_GetObjectItem(step2, "notificationUrl");
    if (cJSON_IsString(notification) && notification->valuestring[0]) {
        miot_cloud_clear_pending_login_captcha();
        char abs_url[MIOT_2FA_URL_MAX];
        abs_url[0] = '\0';
        if (notification->valuestring[0] == '/') {
            snprintf(abs_url, sizeof(abs_url), "https://account.xiaomi.com%s",
                     notification->valuestring);
        } else if (strncmp(notification->valuestring, "http", 4) != 0) {
            snprintf(abs_url, sizeof(abs_url), "https://account.xiaomi.com/%s",
                     notification->valuestring);
        } else {
            strncpy(abs_url, notification->valuestring, sizeof(abs_url) - 1);
            abs_url[sizeof(abs_url) - 1] = '\0';
        }
        ESP_LOGI(TAG, "2FA notificationUrl len %u", (unsigned)strlen(abs_url));
        json_cookie_put(step2, "passToken", jar, sizeof(jar));
        json_cookie_put(step2, "userId", jar, sizeof(jar));
        json_cookie_put(step2, "cUserId", jar, sizeof(jar));
        const cJSON *pt = cJSON_GetObjectItem(step2, "passToken");
        ESP_LOGI(TAG, "2FA step2 has passToken=%d userId=%d passToken_len=%u",
                 cJSON_GetObjectItem(step2, "passToken") != NULL,
                 cJSON_GetObjectItem(step2, "userId") != NULL,
                 (cJSON_IsString(pt) && pt->valuestring) ? (unsigned)strlen(pt->valuestring) : 0);
        ESP_LOGI(TAG, "2FA pending cookie jar %u bytes deviceId=%s", (unsigned)strlen(jar),
                 session->device_id);
        strncpy(s_last_verify_url, abs_url, sizeof(s_last_verify_url) - 1);
        s_last_verify_url[sizeof(s_last_verify_url) - 1] = '\0';
        miot_cloud_save_pending_2fa(abs_url, jar);
        miot_cloud_save_pending_login_params(p2fa_qs, p2fa_sign);
        if (verify_url && verify_url_len > 0) {
            strncpy(verify_url, abs_url, verify_url_len - 1);
            verify_url[verify_url_len - 1] = '\0';
        }
        ESP_LOGI(TAG, "2FA verify_url ready len=%u", (unsigned)strlen(s_last_verify_url));
        cJSON_Delete(step2);
        return ESP_ERR_INVALID_STATE;
    }

    if (xiaomi_json_has_error(step2)) {
        const cJSON *code = cJSON_GetObjectItem(step2, "code");
        const char *desc = xiaomi_json_desc(step2);
        const cJSON *captcha_item = cJSON_GetObjectItem(step2, "captchaUrl");
        ESP_LOGW(TAG, "login rejected: code=%d desc=%s captchaUrl=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1, desc ? desc : "?",
                 (cJSON_IsString(captcha_item) && captcha_item->valuestring)
                     ? captcha_item->valuestring
                     : "null");
        err = handle_login_captcha_required(session, step2, username, password, qs_val, sign_val,
                                            sid_val, callback_val, jar);
        if (err == ESP_ERR_INVALID_STATE) {
            cJSON_Delete(step2);
            return err;
        }
        miot_cloud_set_error("%s", desc ? desc : "login rejected by Xiaomi cloud");
        cJSON_Delete(step2);
        return ESP_FAIL;
    }

    miot_cloud_clear_pending_login_captcha();

    const cJSON *location = cJSON_GetObjectItem(step2, "location");
    const cJSON *user_id = cJSON_GetObjectItem(step2, "userId");
    const cJSON *ssecurity = cJSON_GetObjectItem(step2, "ssecurity");
    if (!cJSON_IsString(location) || !location->valuestring[0] ||
        !cJSON_IsString(user_id) || !user_id->valuestring[0] ||
        !cJSON_IsString(ssecurity) || !ssecurity->valuestring[0]) {
        char *raw = cJSON_PrintUnformatted(step2);
        log_body_preview("login incomplete response", raw);
        free(raw);
        cJSON_Delete(step2);
        miot_cloud_set_error("login response missing location/userId/ssecurity");
        return ESP_FAIL;
    }

    strncpy(session->user_id, user_id->valuestring, sizeof(session->user_id) - 1);
    strncpy(session->ssecurity, ssecurity->valuestring, sizeof(session->ssecurity) - 1);

    char set_cookie[512];
    err = cloud_http(location->valuestring, HTTP_METHOD_GET, NULL, ua, jar, sizeof(jar), &resp,
                     set_cookie, sizeof(set_cookie));
    cJSON_Delete(step2);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }

    char service_token[MIOT_CLOUD_TOKEN_MAX];
    if (extract_cookie_value(jar, "serviceToken", service_token, sizeof(service_token)) != ESP_OK &&
        extract_cookie_value(set_cookie, "serviceToken", service_token, sizeof(service_token)) != ESP_OK) {
        free(resp.body);
        ESP_LOGE(TAG, "serviceToken not found in login redirect (cookie=%s)", set_cookie);
        miot_cloud_set_error("serviceToken missing in login redirect");
        return ESP_FAIL;
    }
    free(resp.body);

    strncpy(session->service_token, service_token, sizeof(session->service_token) - 1);
    if (session->region[0] == '\0') {
        strncpy(session->region, "cn", sizeof(session->region) - 1);
    }
    session->device_id_pinned = true;
    session->valid = true;
    return miot_cloud_session_save(session);
}

/* RC4 drop-1024 (Xiaomi ENCRYPT-RC4). Ciphertext in place. */
static void rc4_drop1024(const uint8_t *key, size_t key_len, uint8_t *buf, size_t buf_len)
{
    uint8_t s[256];
    for (int i = 0; i < 256; i++) {
        s[i] = (uint8_t)i;
    }
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + s[i] + key[i % key_len]);
        uint8_t tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
    }
    uint8_t i = 0;
    j = 0;
    for (int n = 0; n < 1024; n++) {
        i++;
        j = (uint8_t)(j + s[i]);
        uint8_t tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
    }
    for (size_t n = 0; n < buf_len; n++) {
        i++;
        j = (uint8_t)(j + s[i]);
        uint8_t tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
        buf[n] ^= s[(uint8_t)(s[i] + s[j])];
    }
}

static esp_err_t rc4_b64_encrypt(const uint8_t *key, size_t key_len,
                                 const char *plain, char *out_b64, size_t out_len)
{
    size_t plain_len = strlen(plain);
    uint8_t *buf = (uint8_t *)malloc(plain_len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, plain, plain_len);
    rc4_drop1024(key, key_len, buf, plain_len);
    esp_err_t err = b64_encode(buf, plain_len, out_b64, out_len);
    free(buf);
    return err;
}

static esp_err_t rc4_b64_decrypt(const uint8_t *key, size_t key_len,
                                 const char *cipher_b64, char **out_plain)
{
    size_t cap = strlen(cipher_b64) + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t olen = 0;
    if (b64_decode(cipher_b64, buf, cap, &olen) != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }
    rc4_drop1024(key, key_len, buf, olen);
    char *plain = (char *)realloc(buf, olen + 1);
    if (!plain) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    plain[olen] = '\0';
    *out_plain = plain;
    return ESP_OK;
}

/** SHA1(method & path & k=v... & signed_nonce) → base64 (Xiaomi enc signature). */
static esp_err_t enc_sha1_b64(const char *msg, char *out, size_t out_len)
{
    uint8_t digest[20];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md || mbedtls_md(md, (const unsigned char *)msg, strlen(msg), digest) != 0) {
        return ESP_FAIL;
    }
    return b64_encode(digest, sizeof(digest), out, out_len);
}

esp_err_t miot_cloud_request_rc4(const miot_cloud_session_t *session, const char *path,
                                 const char *data_json, cJSON **out_resp)
{
    if (!miot_cloud_session_valid(session) || !path || !data_json || !out_resp) {
        return ESP_ERR_INVALID_ARG;
    }

    char nonce[64];
    esp_err_t err = miot_cloud_gen_nonce(nonce, sizeof(nonce));
    if (err != ESP_OK) {
        return err;
    }

    char snonce[64];
    err = sign_nonce(session->ssecurity, nonce, snonce, sizeof(snonce));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t snonce_dec[64];
    size_t snonce_len = 0;
    if (b64_decode(snonce, snonce_dec, sizeof(snonce_dec), &snonce_len) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Path form for enc signature: "/v2/..." (strip "/app" prefix if present). */
    const char *sign_path = path;
    if (strncmp(path, "/app/", 5) == 0) {
        sign_path = path + 4;
    }

    char rc4_hash[64];
    char msg[1024];
    int n = snprintf(msg, sizeof(msg), "POST&%s&data=%s&%s", sign_path, data_json, snonce);
    if (n < 0 || n >= (int)sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    err = enc_sha1_b64(msg, rc4_hash, sizeof(rc4_hash));
    if (err != ESP_OK) {
        return err;
    }

    char data_enc[512];
    char hash_enc[128];
    err = rc4_b64_encrypt(snonce_dec, snonce_len, data_json, data_enc, sizeof(data_enc));
    if (err != ESP_OK) {
        return err;
    }
    err = rc4_b64_encrypt(snonce_dec, snonce_len, rc4_hash, hash_enc, sizeof(hash_enc));
    if (err != ESP_OK) {
        return err;
    }

    n = snprintf(msg, sizeof(msg), "POST&%s&data=%s&rc4_hash__=%s&%s",
                 sign_path, data_enc, hash_enc, snonce);
    if (n < 0 || n >= (int)sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    char signature[64];
    err = enc_sha1_b64(msg, signature, sizeof(signature));
    if (err != ESP_OK) {
        return err;
    }

    char data_q[768];
    char hash_q[192];
    char sig_q[128];
    char nonce_q[96];
    char ssec_q[192];
    miot_http_urlencode(data_enc, data_q, sizeof(data_q));
    miot_http_urlencode(hash_enc, hash_q, sizeof(hash_q));
    miot_http_urlencode(signature, sig_q, sizeof(sig_q));
    miot_http_urlencode(nonce, nonce_q, sizeof(nonce_q));
    miot_http_urlencode(session->ssecurity, ssec_q, sizeof(ssec_q));

    char post[2048];
    n = snprintf(post, sizeof(post),
                 "data=%s&rc4_hash__=%s&signature=%s&ssecurity=%s&_nonce=%s",
                 data_q, hash_q, sig_q, ssec_q, nonce_q);
    if (n < 0 || n >= (int)sizeof(post)) {
        return ESP_ERR_NO_MEM;
    }

    char base[96];
    api_base(session->region, base, sizeof(base));
    char url[160];
    snprintf(url, sizeof(url), "%s%s", base, path);

    miot_cloud_session_t sess = *session;
    char cookie[640];
    if (sess.device_id_pinned && sess.device_id[0]) {
        snprintf(cookie, sizeof(cookie),
                 "userId=%s; serviceToken=%s; locale=zh_CN; PassportDeviceId=%s",
                 sess.user_id, sess.service_token, sess.device_id);
    } else {
        snprintf(cookie, sizeof(cookie),
                 "userId=%s; serviceToken=%s; locale=zh_CN",
                 sess.user_id, sess.service_token);
    }

    char ua_id[MIOT_CLOUD_DEVICE_ID_MAX];
    if (sess.device_id[0]) {
        strncpy(ua_id, sess.device_id, sizeof(ua_id) - 1);
        ua_id[sizeof(ua_id) - 1] = '\0';
    } else {
        strncpy(ua_id, "0000000000000000", sizeof(ua_id) - 1);
    }
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, ua_id);

    miot_http_buf_t resp = {0};
    err = miot_http_post_cloud_rc4(url, post, ua, cookie, &resp);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        if (!miot_cloud_last_error()) {
            miot_cloud_set_error("cloud RC4 API HTTP error");
        }
        return err;
    }

    char *plain = NULL;
    err = rc4_b64_decrypt(snonce_dec, snonce_len, resp.body, &plain);
    free(resp.body);
    if (err != ESP_OK || !plain) {
        free(plain);
        miot_cloud_set_error("cloud RC4 decrypt failed");
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(plain);
    free(plain);
    if (!root) {
        return ESP_FAIL;
    }

    const cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        const cJSON *msg_j = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "cloud RC4 API %s failed: code=%d msg=%s", path, code->valueint,
                 cJSON_IsString(msg_j) ? msg_j->valuestring : "?");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_resp = root;
    return ESP_OK;
}

esp_err_t miot_cloud_blt_get_beaconkey(const miot_cloud_session_t *session, const char *did,
                                       char *out_key, size_t out_len)
{
    if (!session || !did || !did[0] || !out_key || out_len < 25) {
        return ESP_ERR_INVALID_ARG;
    }

    char data[160];
    int n = snprintf(data, sizeof(data), "{\"did\":\"%s\",\"pdid\":1}", did);
    if (n < 0 || n >= (int)sizeof(data)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request_rc4(session, "/v2/device/blt_get_beaconkey", data, &resp);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    const cJSON *key = result ? cJSON_GetObjectItem(result, "beaconkey") : NULL;
    if (!cJSON_IsString(key) || !key->valuestring[0]) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }

    strncpy(out_key, key->valuestring, out_len - 1);
    out_key[out_len - 1] = '\0';
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t miot_cloud_request(const miot_cloud_session_t *session, const char *path,
                             const char *data_json, cJSON **out_resp)
{
    if (!miot_cloud_session_valid(session) || !path || !data_json || !out_resp) {
        return ESP_ERR_INVALID_ARG;
    }

    miot_cloud_sign_t sign = {0};
    esp_err_t err = miot_cloud_sign_request(path, data_json, session->ssecurity, &sign);
    if (err != ESP_OK) {
        return err;
    }

    char base[96];
    api_base(session->region, base, sizeof(base));

    char url[160];
    snprintf(url, sizeof(url), "%s%s", base, path);

    char sig_enc[192];
    char nonce_enc[96];
    char data_enc[768];
    miot_http_urlencode(sign.signature, sig_enc, sizeof(sig_enc));
    miot_http_urlencode(sign.nonce, nonce_enc, sizeof(nonce_enc));
    miot_http_urlencode(data_json, data_enc, sizeof(data_enc));

    char post[1536];
    int plen = snprintf(post, sizeof(post), "signature=%s&_nonce=%s&data=%s",
                        sig_enc, nonce_enc, data_enc);
    if (plen < 0 || plen >= (int)sizeof(post)) {
        return ESP_ERR_NO_MEM;
    }

    miot_cloud_session_t sess = *session;

    char cookie[640];
    if (sess.device_id_pinned && sess.device_id[0]) {
        snprintf(cookie, sizeof(cookie),
                 "userId=%s; serviceToken=%s; locale=zh_CN; PassportDeviceId=%s",
                 sess.user_id, sess.service_token, sess.device_id);
    } else {
        snprintf(cookie, sizeof(cookie),
                 "userId=%s; serviceToken=%s; locale=zh_CN",
                 sess.user_id, sess.service_token);
    }

    char ua_id[MIOT_CLOUD_DEVICE_ID_MAX];
    if (sess.device_id[0]) {
        strncpy(ua_id, sess.device_id, sizeof(ua_id) - 1);
        ua_id[sizeof(ua_id) - 1] = '\0';
    } else {
        strncpy(ua_id, "0000000000000000", sizeof(ua_id) - 1);
    }

    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, ua_id);

    miot_http_buf_t resp = {0};
    err = miot_http_post_cloud(url, post, ua, cookie, &resp);
    if (err != ESP_OK || !resp.body) {
        bool token_expired = false;
        if (resp.body) {
            log_body_preview("cloud API error body", resp.body);
            cJSON *err_json = cJSON_Parse(resp.body);
            if (err_json) {
                const cJSON *msg = cJSON_GetObjectItem(err_json, "message");
                if (cJSON_IsString(msg) && msg->valuestring[0]) {
                    if (strcmp(msg->valuestring, "SERVICETOKEN_EXPIRED") == 0 ||
                        strcmp(msg->valuestring, "auth error") == 0) {
                        token_expired = true;
                        miot_cloud_set_error("登录已过期，请重新登录米家");
                    } else {
                        miot_cloud_set_error("cloud API: %s", msg->valuestring);
                    }
                }
                cJSON_Delete(err_json);
            }
        }
        free(resp.body);
        if (token_expired) {
            return ESP_ERR_INVALID_STATE;
        }
        if (err == ESP_FAIL && !miot_cloud_last_error()) {
            miot_cloud_set_error("cloud API HTTP error");
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(resp.body);
    free(resp.body);
    if (!root) {
        return ESP_FAIL;
    }

    const cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "cloud API %s failed: code=%d msg=%s", path, code->valueint,
                 cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_resp = root;
    return ESP_OK;
}

esp_err_t miot_cloud_device_list(const miot_cloud_session_t *session, cJSON **out_list)
{
    const char *data = "{\"getVirtualModel\":false,\"getHuamiDevices\":0}";
    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/home/device_list", data, &resp);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    cJSON *list = result ? cJSON_GetObjectItem(result, "list") : NULL;
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    *out_list = cJSON_Duplicate(list, true);
    cJSON_Delete(resp);
    return *out_list ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miot_cloud_homeroom_gethome(const miot_cloud_session_t *session, cJSON **out_result)
{
    if (!out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = NULL;
    const char *data =
        "{\"limit\":150,\"fetch_share\":true,\"fetch_share_dev\":true,\"plat_form\":0,\"app_ver\":9}";
    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/v2/homeroom/gethome", data, &resp);
    if (err != ESP_OK) {
        return err;
    }
    cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    *out_result = cJSON_Duplicate(result, true);
    cJSON_Delete(resp);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miot_cloud_prop_get(const miot_cloud_session_t *session,
                              const char *did, int siid, int piid, cJSON **out_value)
{
    /* Prefer /v2 + datasource so Mesh/gateway devices match Mi Home (app shows
     * live state). datasource=2 reads from device via cloud/gateway; 1=cache.
     * BLE (blt.*) magnets often have no live gateway path — fall back to cache
     * like HA xiaomi_miot / RPC prop_get_multi. */
    if (!session || !did || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = NULL;

    const int datasources[] = {2, 1};
    const size_t n_ds = (strncmp(did, "blt.", 4) == 0) ? 2 : 1;
    esp_err_t last_err = ESP_FAIL;

    for (size_t i = 0; i < n_ds; i++) {
        char data[192];
        snprintf(data, sizeof(data),
                 "{\"datasource\":%d,\"params\":[{\"did\":\"%s\",\"siid\":%d,\"piid\":%d}]}",
                 datasources[i], did, siid, piid);

        cJSON *resp = NULL;
        esp_err_t err = miot_cloud_request(session, "/v2/miotspec/prop/get", data, &resp);
        if (err != ESP_OK) {
            last_err = err;
            continue;
        }

        const cJSON *result = cJSON_GetObjectItem(resp, "result");
        if (!cJSON_IsArray(result) || cJSON_GetArraySize(result) == 0) {
            cJSON_Delete(resp);
            last_err = ESP_FAIL;
            continue;
        }

        const cJSON *item = cJSON_GetArrayItem(result, 0);
        const cJSON *code = cJSON_GetObjectItem(item, "code");
        if (!miot_spec_item_code_ok(code)) {
            cJSON_Delete(resp);
            last_err = ESP_FAIL;
            continue;
        }

        const cJSON *value = cJSON_GetObjectItem(item, "value");
        *out_value = value ? cJSON_Duplicate(value, true) : cJSON_CreateNull();
        cJSON_Delete(resp);
        return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
    }

    return last_err;
}

esp_err_t miot_cloud_prop_get_multi(const miot_cloud_session_t *session, const char *did,
                                    const cJSON *pairs, cJSON **out_result)
{
    if (!session || !did || !cJSON_IsArray(pairs) || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_result = NULL;

    cJSON *root = cJSON_CreateObject();
    cJSON *params = cJSON_AddArrayToObject(root, "params");
    if (!root || !params || !cJSON_AddNumberToObject(root, "datasource", 1)) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    const cJSON *pair = NULL;
    cJSON_ArrayForEach(pair, pairs) {
        const cJSON *siid = cJSON_GetObjectItem(pair, "siid");
        const cJSON *piid = cJSON_GetObjectItem(pair, "piid");
        if (!cJSON_IsNumber(siid) || !cJSON_IsNumber(piid)) {
            continue;
        }
        const cJSON *pdid = cJSON_GetObjectItem(pair, "did");
        const char *use_did = (cJSON_IsString(pdid) && pdid->valuestring[0]) ? pdid->valuestring : did;
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item, "did", use_did) ||
            !cJSON_AddNumberToObject(item, "siid", siid->valuedouble) ||
            !cJSON_AddNumberToObject(item, "piid", piid->valuedouble) ||
            !cJSON_AddItemToArray(params, item)) {
            cJSON_Delete(item);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    if (cJSON_GetArraySize(params) == 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char *data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/v2/miotspec/prop/get", data, &resp);
    free(data);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    *out_result = cJSON_Duplicate(result, true);
    cJSON_Delete(resp);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miot_cloud_prop_set(const miot_cloud_session_t *session,
                              const char *did, int siid, int piid, cJSON *value)
{
    char *value_str = cJSON_PrintUnformatted(value);
    if (!value_str) {
        return ESP_ERR_NO_MEM;
    }

    char data[384];
    snprintf(data, sizeof(data),
             "{\"params\":[{\"did\":\"%s\",\"siid\":%d,\"piid\":%d,\"value\":%s}]}",
             did, siid, piid, value_str);
    free(value_str);

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/miotspec/prop/set", data, &resp);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        const cJSON *item = cJSON_GetArrayItem(result, 0);
        const cJSON *code = cJSON_GetObjectItem(item, "code");
        if (!miot_spec_item_code_ok(code)) {
            ESP_LOGW(TAG, "prop/set did=%s siid=%d piid=%d code=%d",
                     did, siid, piid, code->valueint);
            snprintf(s_last_error, sizeof(s_last_error),
                     "prop/set code=%d siid=%d piid=%d", code->valueint, siid, piid);
            cJSON_Delete(resp);
            return ESP_FAIL;
        }
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t miot_cloud_action(const miot_cloud_session_t *session,
                            const char *did, int siid, int aiid,
                            const cJSON *in_args)
{
    if (!did) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in_args && !cJSON_IsArray(in_args)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *in_str = NULL;
    char in_empty[] = "[]";
    bool in_owned = false;
    if (in_args) {
        in_str = cJSON_PrintUnformatted(in_args);
        in_owned = true;
    } else {
        in_str = in_empty;
    }
    if (!in_str) {
        return ESP_ERR_NO_MEM;
    }

    /* Official Mi Home / HA: params is a single object (not array); in = plain values.
     * Array form → cloud code -8 "data type not valid". */
    char data[1536];
    int dlen = snprintf(data, sizeof(data),
                        "{\"params\":{\"did\":\"%s\",\"siid\":%d,\"aiid\":%d,\"in\":",
                        did, siid, aiid);
    if (dlen < 0 || dlen >= (int)sizeof(data)) {
        if (in_owned) {
            free(in_str);
        }
        return ESP_ERR_NO_MEM;
    }
    size_t in_len = strlen(in_str);
    if ((size_t)dlen + in_len + 2 >= sizeof(data)) {
        if (in_owned) {
            free(in_str);
        }
        return ESP_ERR_NO_MEM;
    }
    memcpy(data + dlen, in_str, in_len);
    dlen += (int)in_len;
    data[dlen++] = '}';
    data[dlen++] = '}';
    data[dlen] = '\0';
    if (in_owned) {
        free(in_str);
    }

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/v2/miotspec/action", data, &resp);
    if (err != ESP_OK) {
        return err;
    }

    /* v2 may return result as object; legacy returns array of one. */
    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    const cJSON *code = NULL;
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        code = cJSON_GetObjectItem(cJSON_GetArrayItem(result, 0), "code");
    } else if (cJSON_IsObject(result)) {
        code = cJSON_GetObjectItem(result, "code");
    }
    if (cJSON_IsNumber(code) && !miot_spec_item_code_ok(code)) {
        ESP_LOGW(TAG, "action did=%s siid=%d aiid=%d code=%d",
                 did, siid, aiid, code->valueint);
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    cJSON_Delete(resp);
    return ESP_OK;
}

esp_err_t miot_cloud_get_user_device_data(const miot_cloud_session_t *session,
                                          const char *did, const char *key, const char *type,
                                          int limit, cJSON **out_result)
{
    if (!miot_cloud_session_valid(session) || !did || !did[0] || !key || !key[0] ||
        !type || !type[0] || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 100) {
        limit = 100;
    }

    time_t now = time(NULL);
    if (now < (time_t)1700000000) {
        now = (time_t)1700000000 + 86400 * 365;
    }
    long time_end = (long)now;
    long time_start = time_end - (90L * 24L * 3600L);

    char data[384];
    int n = snprintf(data, sizeof(data),
                     "{\"uid\":\"%s\",\"did\":\"%s\",\"key\":\"%s\",\"type\":\"%s\","
                     "\"time_start\":%ld,\"time_end\":%ld,\"limit\":%d}",
                     session->user_id, did, key, type, time_start, time_end, limit);
    if (n < 0 || n >= (int)sizeof(data)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/user/get_user_device_data", data, &resp);
    if (err != ESP_OK) {
        return err;
    }
    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    *out_result = cJSON_Duplicate(result, true);
    cJSON_Delete(resp);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miot_cloud_get_user_device_log(const miot_cloud_session_t *session,
                                         const char *did, int limit, cJSON **out_result)
{
    if (!miot_cloud_session_valid(session) || !did || !did[0] || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 50) {
        limit = 50;
    }

    time_t now = time(NULL);
    if (now < (time_t)1700000000) {
        now = (time_t)1700000000 + 86400 * 365;
    }
    long time_end = (long)now;
    long time_start = time_end - (90L * 24L * 3600L);

    char data[256];
    int n = snprintf(data, sizeof(data),
                     "{\"did\":\"%s\",\"time_start\":%ld,\"time_end\":%ld,\"limit\":%d}",
                     did, time_start, time_end, limit);
    if (n < 0 || n >= (int)sizeof(data)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *resp = NULL;
    esp_err_t err = miot_cloud_request(session, "/v2/user/get_user_device_log", data, &resp);
    if (err != ESP_OK) {
        return err;
    }
    const cJSON *result = cJSON_GetObjectItem(resp, "result");
    if (!result) {
        cJSON_Delete(resp);
        return ESP_FAIL;
    }
    *out_result = cJSON_Duplicate(result, true);
    cJSON_Delete(resp);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

static const char *url_sign_path(const char *url)
{
    const char *path = url;
    const char *p = strstr(url, "://");
    if (p) {
        p = strchr(p + 3, '/');
        path = p ? p : "/";
    }
    if (strncmp(path, "/app/", 5) == 0) {
        return path + 4;
    }
    return path;
}

static void camera_abs_url(const char *region, const char *host, const char *path,
                           char *out, size_t out_len)
{
    if (!region || !region[0] || strcmp(region, "cn") == 0) {
        snprintf(out, out_len, "https://%s/%s", host, path);
    } else {
        snprintf(out, out_len, "https://%s.%s/%s", region, host, path);
    }
}

static void camera_cookie(const miot_cloud_session_t *session, char *cookie, size_t cookie_len)
{
    if (session->device_id_pinned && session->device_id[0]) {
        snprintf(cookie, cookie_len,
                 "userId=%s; serviceToken=%s; yetAnotherServiceToken=%s; locale=zh_CN; PassportDeviceId=%s",
                 session->user_id, session->service_token, session->service_token, session->device_id);
    } else {
        snprintf(cookie, cookie_len,
                 "userId=%s; serviceToken=%s; yetAnotherServiceToken=%s; locale=zh_CN",
                 session->user_id, session->service_token, session->service_token);
    }
}

static void fill_cloud_ua(const miot_cloud_session_t *session, char *ua, size_t ua_len)
{
    char ua_id[MIOT_CLOUD_DEVICE_ID_MAX];
    if (session->device_id[0]) {
        strncpy(ua_id, session->device_id, sizeof(ua_id) - 1);
        ua_id[sizeof(ua_id) - 1] = '\0';
    } else {
        strncpy(ua_id, "0000000000000000", sizeof(ua_id) - 1);
        ua_id[sizeof(ua_id) - 1] = '\0';
    }
    snprintf(ua, ua_len, UA_FMT, ua_id);
}

static void region_upper(const char *region, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (!region || !region[0] || strcmp(region, "cn") == 0) {
        strncpy(out, "CN", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    size_t i = 0;
    for (; region[i] && i + 1 < out_len; i++) {
        char c = region[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static bool json_as_str(const cJSON *item, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        strncpy(out, item->valuestring, out_len - 1);
        out[out_len - 1] = '\0';
        return true;
    }
    if (cJSON_IsNumber(item)) {
        snprintf(out, out_len, "%d", item->valueint);
        return out[0] != '\0';
    }
    return false;
}

static bool is_jpeg_buf(const uint8_t *p, size_t n)
{
    return p && n >= 2 && p[0] == 0xFF && p[1] == 0xD8;
}

static esp_err_t write_bytes_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t rc4_build_query(const char *method, const char *sign_path, const char *data_json,
                                 const miot_cloud_session_t *session, char *query, size_t query_len,
                                 uint8_t *snonce_dec, size_t snonce_cap, size_t *snonce_len)
{
    char nonce[64];
    esp_err_t err = miot_cloud_gen_nonce(nonce, sizeof(nonce));
    if (err != ESP_OK) {
        return err;
    }
    char snonce[64];
    err = sign_nonce(session->ssecurity, nonce, snonce, sizeof(snonce));
    if (err != ESP_OK) {
        return err;
    }
    size_t slen = 0;
    if (b64_decode(snonce, snonce_dec, snonce_cap, &slen) != ESP_OK) {
        return ESP_FAIL;
    }
    *snonce_len = slen;

    char rc4_hash[64];
    char msg[2048];
    int n = snprintf(msg, sizeof(msg), "%s&%s&data=%s&%s", method, sign_path, data_json, snonce);
    if (n < 0 || n >= (int)sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    err = enc_sha1_b64(msg, rc4_hash, sizeof(rc4_hash));
    if (err != ESP_OK) {
        return err;
    }

    char data_enc[768];
    char hash_enc[128];
    err = rc4_b64_encrypt(snonce_dec, slen, data_json, data_enc, sizeof(data_enc));
    if (err != ESP_OK) {
        return err;
    }
    err = rc4_b64_encrypt(snonce_dec, slen, rc4_hash, hash_enc, sizeof(hash_enc));
    if (err != ESP_OK) {
        return err;
    }
    n = snprintf(msg, sizeof(msg), "%s&%s&data=%s&rc4_hash__=%s&%s",
                 method, sign_path, data_enc, hash_enc, snonce);
    if (n < 0 || n >= (int)sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    char signature[64];
    err = enc_sha1_b64(msg, signature, sizeof(signature));
    if (err != ESP_OK) {
        return err;
    }

    char data_q[1024];
    char hash_q[192];
    char sig_q[128];
    char nonce_q[96];
    char ssec_q[192];
    char tok_q[384];
    miot_http_urlencode(data_enc, data_q, sizeof(data_q));
    miot_http_urlencode(hash_enc, hash_q, sizeof(hash_q));
    miot_http_urlencode(signature, sig_q, sizeof(sig_q));
    miot_http_urlencode(nonce, nonce_q, sizeof(nonce_q));
    miot_http_urlencode(session->ssecurity, ssec_q, sizeof(ssec_q));
    miot_http_urlencode(session->service_token, tok_q, sizeof(tok_q));

    n = snprintf(query, query_len,
                 "data=%s&rc4_hash__=%s&signature=%s&ssecurity=%s&_nonce=%s&yetAnotherServiceToken=%s",
                 data_q, hash_q, sig_q, ssec_q, nonce_q, tok_q);
    if (n < 0 || n >= (int)query_len) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t rc4_get_json(const miot_cloud_session_t *session, const char *abs_url,
                              const char *data_json, cJSON **out_resp)
{
    uint8_t snonce_dec[64];
    size_t snonce_len = 0;
    char query[3072];
    const char *sign_path = url_sign_path(abs_url);
    esp_err_t err = rc4_build_query("GET", sign_path, data_json, session, query, sizeof(query),
                                    snonce_dec, sizeof(snonce_dec), &snonce_len);
    if (err != ESP_OK) {
        return err;
    }

    char url[3584];
    int n = snprintf(url, sizeof(url), "%s?%s", abs_url, query);
    if (n < 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_NO_MEM;
    }

    char cookie[1024];
    char ua[160];
    camera_cookie(session, cookie, sizeof(cookie));
    fill_cloud_ua(session, ua, sizeof(ua));

    miot_http_buf_t resp = {0};
    resp.max_len = 48 * 1024;
    err = miot_http_get_cloud_rc4(url, ua, cookie, &resp);
    if (err != ESP_OK || !resp.body) {
        free(resp.body);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        miot_cloud_set_error("camera eventlist HTTP error");
        return err;
    }

    char *plain = NULL;
    if (resp.body[0] == '{') {
        plain = resp.body;
        resp.body = NULL;
    } else {
        err = rc4_b64_decrypt(snonce_dec, snonce_len, resp.body, &plain);
        free(resp.body);
        if (err != ESP_OK || !plain) {
            free(plain);
            miot_cloud_set_error("camera eventlist decrypt failed");
            return err != ESP_OK ? err : ESP_FAIL;
        }
    }

    cJSON *root = cJSON_Parse(plain);
    free(plain);
    if (!root) {
        miot_cloud_set_error("camera eventlist JSON parse failed");
        return ESP_FAIL;
    }
    const cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        const cJSON *msg_j = cJSON_GetObjectItem(root, "message");
        ESP_LOGW(TAG, "camera eventlist failed: code=%d msg=%s", code->valueint,
                 cJSON_IsString(msg_j) ? msg_j->valuestring : "?");
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    *out_resp = root;
    return ESP_OK;
}

static esp_err_t rc4_get_raw(const miot_cloud_session_t *session, const char *abs_url,
                             const char *data_json, uint8_t **out_buf, size_t *out_len)
{
    uint8_t snonce_dec[64];
    size_t snonce_len = 0;
    char query[3072];
    const char *sign_path = url_sign_path(abs_url);
    esp_err_t err = rc4_build_query("GET", sign_path, data_json, session, query, sizeof(query),
                                    snonce_dec, sizeof(snonce_dec), &snonce_len);
    if (err != ESP_OK) {
        return err;
    }
    (void)snonce_len;

    char url[3584];
    int n = snprintf(url, sizeof(url), "%s?%s", abs_url, query);
    if (n < 0 || n >= (int)sizeof(url)) {
        return ESP_ERR_NO_MEM;
    }

    char cookie[1024];
    char ua[160];
    camera_cookie(session, cookie, sizeof(cookie));
    fill_cloud_ua(session, ua, sizeof(ua));

    miot_http_buf_t resp = {0};
    resp.max_len = 256 * 1024;
    err = miot_http_get_cloud(url, ua, cookie, &resp);
    if (err != ESP_OK || !resp.body || resp.len == 0) {
        free(resp.body);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        miot_cloud_set_error(miot_http_last_get_error() ? miot_http_last_get_error()
                                                        : "camera img HTTP error");
        return err;
    }
    *out_buf = (uint8_t *)resp.body;
    *out_len = resp.len;
    return ESP_OK;
}

esp_err_t miot_cloud_camera_preview_jpeg(const miot_cloud_session_t *session,
                                         const char *did, const char *model,
                                         const char *out_path,
                                         miot_camera_preview_info_t *info)
{
    if (!miot_cloud_session_valid(session) || !did || !did[0] || !out_path || !out_path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info) {
        memset(info, 0, sizeof(*info));
    }

    unsigned now = (unsigned)time(NULL);
    if (now < 1700000000u) {
        now = 1700000000u;
    }
    unsigned begin = (now > 7u * 86400u) ? (now - 7u * 86400u) : 0;
    char region_up[8];
    region_upper(session->region, region_up, sizeof(region_up));
    const char *mod = (model && model[0]) ? model : "";
    const char *doorbell = (strstr(mod, "doorbell") || strstr(mod, ".lock.")) ? "true" : "false";

    char data_json[384];
    int n = snprintf(data_json, sizeof(data_json),
                     "{\"did\":\"%s\",\"model\":\"%s\",\"doorBell\":%s,\"eventType\":\"Default\","
                     "\"needMerge\":true,\"sortType\":\"DESC\",\"region\":\"%s\",\"language\":\"zh_CN\","
                     "\"beginTime\":%u000,\"endTime\":%u999,\"limit\":2}",
                     did, mod, doorbell, region_up, begin, now);
    if (n < 0 || n >= (int)sizeof(data_json)) {
        return ESP_ERR_NO_MEM;
    }

    char abs_url[160];
    camera_abs_url(session->region, "business.smartcamera.api.io.mi.com",
                   "common/app/get/eventlist", abs_url, sizeof(abs_url));

    cJSON *root = NULL;
    esp_err_t err = rc4_get_json(session, abs_url, data_json, &root);
    if (err != ESP_OK) {
        return err;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    const cJSON *units = cJSON_GetObjectItem(data, "thirdPartPlayUnits");
    if (!cJSON_IsArray(units) || cJSON_GetArraySize(units) <= 0) {
        cJSON_Delete(root);
        miot_cloud_set_error("camera events empty");
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON *fst = cJSON_GetArrayItem(units, 0);
    char file_id[128];
    char sto_id[128];
    if (!json_as_str(cJSON_GetObjectItem(fst, "fileId"), file_id, sizeof(file_id)) ||
        !json_as_str(cJSON_GetObjectItem(fst, "imgStoreId"), sto_id, sizeof(sto_id))) {
        cJSON_Delete(root);
        miot_cloud_set_error("camera event missing fileId/imgStoreId");
        return ESP_ERR_NOT_FOUND;
    }
    if (info) {
        json_as_str(cJSON_GetObjectItem(fst, "eventType"), info->event_type, sizeof(info->event_type));
        json_as_str(cJSON_GetObjectItem(fst, "createTime"), info->create_time, sizeof(info->create_time));
    }
    cJSON_Delete(root);

    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));
    char iv_b64[32];
    if (b64_encode(iv, sizeof(iv), iv_b64, sizeof(iv_b64)) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *img = cJSON_CreateObject();
    if (!img) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(img, "did", did);
    cJSON_AddStringToObject(img, "fileId", file_id);
    cJSON_AddStringToObject(img, "stoId", sto_id);
    cJSON_AddStringToObject(img, "segmentIv", iv_b64);
    char *img_json = cJSON_PrintUnformatted(img);
    cJSON_Delete(img);
    if (!img_json) {
        return ESP_ERR_NO_MEM;
    }

    const char *img_host = "processor.smartcamera.api.io.mi.com";
    if (!session->region[0] || strcmp(session->region, "cn") == 0) {
        /* CN GSLB 302s processor → alibj.app.processor; start there. */
        img_host = "alibj.app.processor.smartcamera.api.io.mi.com";
    }
    camera_abs_url(session->region, img_host, "miot/camera/app/v1/img", abs_url, sizeof(abs_url));
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    err = rc4_get_raw(session, abs_url, img_json, &raw, &raw_len);
    free(img_json);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *jpeg = raw;
    size_t jpeg_len = raw_len;
    if (!is_jpeg_buf(raw, raw_len)) {
        uint8_t key[32];
        size_t key_len = 0;
        if (b64_decode(session->ssecurity, key, sizeof(key), &key_len) != ESP_OK || key_len < 16) {
            free(raw);
            miot_cloud_set_error("camera AES key invalid");
            return ESP_FAIL;
        }
        if ((raw_len % 16) != 0) {
            free(raw);
            miot_cloud_set_error("camera img not AES block aligned");
            return ESP_FAIL;
        }
        err = miot_aes128_cbc_decrypt(key, key_len, iv, raw, raw_len, &jpeg_len);
        if (err != ESP_OK || !is_jpeg_buf(raw, jpeg_len)) {
            free(raw);
            miot_cloud_set_error("camera AES decrypt failed");
            return err != ESP_OK ? err : ESP_FAIL;
        }
        jpeg = raw;
    }

    err = write_bytes_file(out_path, jpeg, jpeg_len);
    free(raw);
    if (err != ESP_OK) {
        miot_cloud_set_error("camera preview write failed");
        return err;
    }
    if (info) {
        info->jpeg_bytes = jpeg_len;
    }
    ESP_LOGI(TAG, "camera preview %u bytes -> %s", (unsigned)jpeg_len, out_path);
    return ESP_OK;
}
