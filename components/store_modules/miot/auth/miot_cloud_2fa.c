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
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "miot_http.h"
#include "nvs.h"
#include "mbedtls/base64.h"

static const char *TAG = "miot_2fa";

static const char *UA_FMT =
    "Android-7.1.1-1.0.0-ONEPLUS A3010-136-%s APP/xiaomi.smarthome APPV/62830";

#define PENDING_JAR_MAX 2048
#define PENDING_IDENTITY_SESSION_MAX 1024
#define FLAG_PHONE 4
#define FLAG_EMAIL 8

static char *s_captcha_b64;
static char s_captcha_mime[24] = "image/jpeg";
static char s_last_icode_sent[16];

/**
 * In-RAM pending 2FA (authStart URL + cookie jar).
 * Login returns verify_url from RAM even when NVS write fails (full partition /
 * ignore of nvs_set_str). Send/verify must not depend solely on NVS or the UI
 * shows 2FA while identity/list returns ESP_ERR_NOT_FOUND.
 */
static char s_pending_url[MIOT_2FA_URL_MAX];
static char s_pending_jar[PENDING_JAR_MAX];
static bool s_pending_ram_valid;
static char s_pending_ick[256];
static int s_pending_flag = -1;

static esp_err_t extract_cookie_value(const char *jar, const char *name, char *out, size_t out_len);

static void pending_ram_clear(void)
{
    s_pending_url[0] = '\0';
    s_pending_jar[0] = '\0';
    s_pending_ick[0] = '\0';
    s_pending_flag = -1;
    s_pending_ram_valid = false;
}

static void pending_ram_set(const char *url, const char *jar)
{
    if (!url || !url[0]) {
        pending_ram_clear();
        return;
    }
    strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    if (jar && jar[0]) {
        strncpy(s_pending_jar, jar, sizeof(s_pending_jar) - 1);
        s_pending_jar[sizeof(s_pending_jar) - 1] = '\0';
    } else {
        s_pending_jar[0] = '\0';
    }
    s_pending_ram_valid = true;
}

static void pending_ram_set_jar(const char *jar)
{
    if (!jar || !jar[0]) {
        return;
    }
    strncpy(s_pending_jar, jar, sizeof(s_pending_jar) - 1);
    s_pending_jar[sizeof(s_pending_jar) - 1] = '\0';
    /* Keep jar even if URL not yet mirrored — captcha retry needs it. */
    s_pending_ram_valid = true;
}

static bool jar_has_identity_session(const char *jar)
{
    char tmp[48];
    return jar && jar[0] &&
           extract_cookie_value(jar, "identity_session", tmp, sizeof(tmp)) == ESP_OK &&
           tmp[0] != '\0';
}

static void captcha_b64_clear(void)
{
    free(s_captcha_b64);
    s_captcha_b64 = NULL;
}

static esp_err_t captcha_b64_store(const uint8_t *data, size_t len)
{
    captcha_b64_clear();
    if (len >= 4 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        strncpy(s_captcha_mime, "image/png", sizeof(s_captcha_mime) - 1);
    } else if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        strncpy(s_captcha_mime, "image/jpeg", sizeof(s_captcha_mime) - 1);
    } else {
        strncpy(s_captcha_mime, "image/jpeg", sizeof(s_captcha_mime) - 1);
    }
    s_captcha_mime[sizeof(s_captcha_mime) - 1] = '\0';
    size_t need = 4 * ((len + 2) / 3) + 1;
    s_captcha_b64 = malloc(need);
    if (!s_captcha_b64) {
        return ESP_ERR_NO_MEM;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)s_captcha_b64, need, &olen, data, len) != 0) {
        captcha_b64_clear();
        return ESP_FAIL;
    }
    s_captcha_b64[olen] = '\0';
    return ESP_OK;
}

const char *miot_cloud_last_2fa_captcha_b64(void)
{
    return s_captcha_b64;
}

const char *miot_cloud_last_2fa_captcha_mime(void)
{
    return s_captcha_mime;
}

const char *miot_cloud_last_2fa_icode_sent(void)
{
    return s_last_icode_sent[0] ? s_last_icode_sent : NULL;
}

esp_err_t miot_cloud_last_2fa_captcha_save_file(const char *path)
{
    if (!path || !path[0] || !s_captcha_b64 || !s_captcha_b64[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t b64_len = strlen(s_captcha_b64);
    const size_t max_dec = b64_len * 3 / 4 + 8;
    uint8_t *buf = (uint8_t *)malloc(max_dec);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    size_t olen = 0;
    if (mbedtls_base64_decode(buf, max_dec, &olen, (const unsigned char *)s_captcha_b64, b64_len) != 0) {
        free(buf);
        return ESP_FAIL;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        return ESP_FAIL;
    }
    const size_t written = fwrite(buf, 1, olen, f);
    fclose(f);
    free(buf);
    return written == olen ? ESP_OK : ESP_FAIL;
}

/** NVS key names must be <= 15 chars (NVS_KEY_NAME_MAX_SIZE - 1). */
#define NVS_P2FA_URL   "pending_2fa"
#define NVS_P2FA_JAR   "pending_2fa_jar"
#define NVS_P2FA_FLAG  "p2fa_flag"
#define NVS_P2FA_DEVID "p2fa_devid"
#define NVS_P2FA_ICK   "p2fa_ick"
#define NVS_P2FA_QS    "p2fa_qs"
#define NVS_P2FA_SIGN  "p2fa_sign"

static void abs_notification_url(const char *url_in, char *url_out, size_t out_len)
{
    if (!url_in || url_in[0] == '\0') {
        url_out[0] = '\0';
        return;
    }
    if (strncmp(url_in, "http", 4) == 0) {
        strncpy(url_out, url_in, out_len - 1);
        url_out[out_len - 1] = '\0';
        return;
    }
    if (url_in[0] == '/') {
        snprintf(url_out, out_len, "https://account.xiaomi.com%s", url_in);
    } else {
        snprintf(url_out, out_len, "https://account.xiaomi.com/%s", url_in);
    }
}

static esp_err_t extract_query_param(const char *url, const char *key, char *out, size_t out_len)
{
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(url, pattern);
    if (!p) {
        return ESP_ERR_NOT_FOUND;
    }
    p += strlen(pattern);
    size_t i = 0;
    while (p[i] && p[i] != '&' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t extract_cookie_value(const char *jar, const char *name, char *out, size_t out_len)
{
    if (!jar || !name || !name[0] || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t name_len = strlen(name);
    const char *p = jar;
    while (p && *p) {
        while (*p == ' ') {
            p++;
        }
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t i = 0;
            while (p[i] && p[i] != ';' && i + 1 < out_len) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
            return i > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
        }
        const char *semi = strchr(p, ';');
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

static bool icode_char_ok(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/** Trim and fold full-width alnum to ASCII; drop other characters. */
static size_t icode_sanitize(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return 0;
    }

    size_t w = 0;
    for (size_t r = 0; buf[r] != '\0' && r < buf_len - 1; r++) {
        unsigned char c = (unsigned char)buf[r];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        /* UTF-8 full-width ASCII variant: EF BC xx */
        if (c == 0xEF && (unsigned char)buf[r + 1] == 0xBC && buf[r + 2]) {
            unsigned char fb = (unsigned char)buf[r + 2];
            if (fb >= 0x90 && fb <= 0x99) {
                buf[w++] = (char)('0' + (fb - 0x90)); /* ０-９ */
            } else if (fb >= 0xA1 && fb <= 0xBA) {
                buf[w++] = (char)('A' + (fb - 0xA1)); /* Ａ-Ｚ */
            } else if (fb >= 0xB1 && fb <= 0xCA) {
                buf[w++] = (char)('a' + (fb - 0xB1)); /* ａ-ｚ */
            }
            r += 2;
            continue;
        }
        if (icode_char_ok((char)c)) {
            buf[w++] = (char)c;
        }
    }
    buf[w] = '\0';
    return w;
}

static bool icode_chars_valid(const char *icode)
{
    if (!icode || !icode[0]) {
        return false;
    }
    size_t len = strlen(icode);
    if (len < 3 || len > 8) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!icode_char_ok(icode[i])) {
            return false;
        }
    }
    return true;
}

static void ensure_device_id(miot_cloud_session_t *session)
{
    if (session->device_id[0] != '\0') {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(session->device_id);
        if (nvs_get_str(nvs, NVS_P2FA_DEVID, session->device_id, &len) == ESP_OK &&
            session->device_id[0] != '\0') {
            nvs_close(nvs);
            ESP_LOGD(TAG, "device_id restored from NVS");
            return;
        }
        char jar[PENDING_JAR_MAX];
        len = sizeof(jar);
        if (nvs_get_str(nvs, NVS_P2FA_JAR, jar, &len) == ESP_OK &&
            extract_cookie_value(jar, "deviceId", session->device_id,
                                 sizeof(session->device_id)) == ESP_OK &&
            session->device_id[0] != '\0') {
            nvs_close(nvs);
            ESP_LOGD(TAG, "device_id restored from pending jar");
            return;
        }
        nvs_close(nvs);
    }
    static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 16; i++) {
        session->device_id[i] = chars[esp_random() % (sizeof(chars) - 1)];
    }
    session->device_id[16] = '\0';
}

static void log_jar_hint(const char *label, const char *jar)
{
    char tmp[48];
    bool id_sess = extract_cookie_value(jar, "identity_session", tmp, sizeof(tmp)) == ESP_OK;
    bool ick = extract_cookie_value(jar, "ick", tmp, sizeof(tmp)) == ESP_OK;
    ESP_LOGI(TAG, "%s jar %u bytes identity_session=%d ick=%d",
             label, (unsigned)strlen(jar), id_sess, ick);
}

static esp_err_t nvs_get_pending(char *url, size_t url_len, char *jar, size_t jar_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READONLY, &nvs);
    bool want_url = url && url_len > 0;
    bool got_url = !want_url;
    bool got_jar = !(jar && jar_len > 0);

    if (err == ESP_OK) {
        if (want_url) {
            url[0] = '\0';
            size_t need = 0;
            esp_err_t uerr = nvs_get_str(nvs, NVS_P2FA_URL, NULL, &need);
            if (uerr == ESP_OK) {
                if (need > url_len) {
                    ESP_LOGE(TAG, "pending_2fa URL too long (%u > %u), login again",
                             (unsigned)need, (unsigned)url_len);
                    nvs_close(nvs);
                    return ESP_ERR_INVALID_SIZE;
                }
                size_t len = url_len;
                if (nvs_get_str(nvs, NVS_P2FA_URL, url, &len) == ESP_OK && url[0]) {
                    got_url = true;
                }
            }
        }
        if (jar && jar_len > 0) {
            jar[0] = '\0';
            size_t len = jar_len;
            if (nvs_get_str(nvs, NVS_P2FA_JAR, jar, &len) == ESP_OK && jar[0]) {
                got_jar = true;
            }
        } else {
            got_jar = true;
        }
        nvs_close(nvs);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "pending NVS open failed: %s", esp_err_to_name(err));
    }

    /* Prefer RAM jar when it has identity_session and NVS jar does not (NVS full). */
    if (jar && jar_len > 0 && s_pending_jar[0]) {
        bool ram_ok = jar_has_identity_session(s_pending_jar);
        bool nvs_ok = got_jar && jar_has_identity_session(jar);
        if (ram_ok && !nvs_ok) {
            strncpy(jar, s_pending_jar, jar_len - 1);
            jar[jar_len - 1] = '\0';
            got_jar = true;
            ESP_LOGW(TAG, "pending jar: prefer RAM identity_session, len=%u",
                     (unsigned)strlen(jar));
        } else if (!got_jar) {
            strncpy(jar, s_pending_jar, jar_len - 1);
            jar[jar_len - 1] = '\0';
            got_jar = true;
            ESP_LOGW(TAG, "pending 2FA jar from RAM (NVS miss), len=%u",
                     (unsigned)strlen(s_pending_jar));
        }
    }

    if (want_url && !got_url) {
        const char *vu = miot_cloud_last_verify_url();
        const char *src = NULL;
        if (s_pending_ram_valid && s_pending_url[0]) {
            src = s_pending_url;
        } else if (vu && vu[0]) {
            src = vu;
        }
        if (!src) {
            ESP_LOGE(TAG, "pending_2fa URL missing (NVS+RAM+verify_url)");
            return ESP_ERR_NOT_FOUND;
        }
        if (strlen(src) + 1 > url_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        strncpy(url, src, url_len - 1);
        url[url_len - 1] = '\0';
        got_url = true;
        if (!(s_pending_ram_valid && s_pending_url[0])) {
            pending_ram_set(url, (jar && jar[0]) ? jar : s_pending_jar);
        }
        ESP_LOGW(TAG, "pending 2FA URL from %s, len=%u",
                 (src == vu) ? "verify_url" : "RAM", (unsigned)strlen(url));
    }
    if (want_url && !got_url) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t nvs_set_pending_jar(const char *jar)
{
    if (jar && jar[0]) {
        pending_ram_set_jar(jar);
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return ESP_OK; /* RAM kept */
    }
    if (jar && jar[0]) {
        nvs_erase_key(nvs, NVS_P2FA_JAR);
        err = nvs_set_str(nvs, NVS_P2FA_JAR, jar);
        if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
            /* Drop expired cloud credentials to free pages for 2FA cookies. */
            nvs_erase_key(nvs, "service_token");
            nvs_erase_key(nvs, "ssecurity");
            nvs_erase_key(nvs, NVS_P2FA_JAR);
            err = nvs_set_str(nvs, NVS_P2FA_JAR, jar);
            ESP_LOGW(TAG, "pending jar NVS reclaim retry: %s", esp_err_to_name(err));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "pending jar NVS write failed: %s (RAM kept)", esp_err_to_name(err));
        }
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

static void nvs_clear_pending(void)
{
    pending_ram_clear();
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_erase_key(nvs, NVS_P2FA_URL);
    nvs_erase_key(nvs, NVS_P2FA_JAR);
    nvs_erase_key(nvs, NVS_P2FA_FLAG);
    nvs_erase_key(nvs, NVS_P2FA_DEVID);
    nvs_erase_key(nvs, NVS_P2FA_ICK);
    nvs_erase_key(nvs, NVS_P2FA_QS);
    nvs_erase_key(nvs, NVS_P2FA_SIGN);
    /* legacy keys (names exceeded NVS 15-char limit) */
    nvs_erase_key(nvs, "pending_2fa_flag");
    nvs_erase_key(nvs, "pending_device_id");
    nvs_erase_key(nvs, "pending_pass_token");
    nvs_erase_key(nvs, "pending_identity_session");
    nvs_commit(nvs);
    nvs_close(nvs);
}

void miot_cloud_clear_pending_2fa(void)
{
    nvs_clear_pending();
}

bool miot_cloud_has_pending_2fa(void)
{
    if (s_pending_ram_valid && s_pending_url[0]) {
        return true;
    }
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t need = 0;
    esp_err_t err = nvs_get_str(nvs, NVS_P2FA_URL, NULL, &need);
    nvs_close(nvs);
    return err == ESP_OK && need > 1;
}

static esp_err_t nvs_get_pending_flag(int *flag)
{
    if (s_pending_flag == FLAG_PHONE || s_pending_flag == FLAG_EMAIL) {
        if (flag) {
            *flag = s_pending_flag;
        }
        return ESP_OK;
    }
    nvs_handle_t nvs;
    if (nvs_open("miot_cloud", NVS_READONLY, &nvs) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t v = FLAG_PHONE;
    esp_err_t err = nvs_get_u8(nvs, NVS_P2FA_FLAG, &v);
    nvs_close(nvs);
    if (err == ESP_OK) {
        s_pending_flag = (int)v;
        if (flag) {
            *flag = (int)v;
        }
    }
    return err;
}

static esp_err_t nvs_set_pending_flag(int flag)
{
    s_pending_flag = flag;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return ESP_OK; /* RAM kept */
    }
    err = nvs_set_u8(nvs, NVS_P2FA_FLAG, (uint8_t)flag);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t nvs_set_captcha_ick(const char *ick)
{
    if (!ick || !ick[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_pending_ick, ick, sizeof(s_pending_ick) - 1);
    s_pending_ick[sizeof(s_pending_ick) - 1] = '\0';
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    nvs_erase_key(nvs, NVS_P2FA_ICK);
    err = nvs_set_str(nvs, NVS_P2FA_ICK, ick);
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        nvs_erase_key(nvs, "service_token");
        nvs_erase_key(nvs, "ssecurity");
        nvs_erase_key(nvs, NVS_P2FA_ICK);
        err = nvs_set_str(nvs, NVS_P2FA_ICK, ick);
    }
    if (err == ESP_OK) {
        nvs_commit(nvs);
    } else {
        ESP_LOGW(TAG, "captcha ick NVS write failed: %s (RAM kept)", esp_err_to_name(err));
    }
    nvs_close(nvs);
    return ESP_OK;
}

static esp_err_t nvs_get_captcha_ick(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (s_pending_ick[0]) {
        strncpy(out, s_pending_ick, out_len - 1);
        out[out_len - 1] = '\0';
        return ESP_OK;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = out_len;
    err = nvs_get_str(nvs, NVS_P2FA_ICK, out, &len);
    nvs_close(nvs);
    if (err == ESP_OK && out[0]) {
        strncpy(s_pending_ick, out, sizeof(s_pending_ick) - 1);
        s_pending_ick[sizeof(s_pending_ick) - 1] = '\0';
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

/** Replace authStart path with identity/list (AlexxIT / XiaomiGateway3 flow). */
static void notification_to_list_url(const char *pending, char *list_url, size_t list_len)
{
    if (!pending || !list_url || list_len == 0) {
        return;
    }
    strncpy(list_url, pending, list_len - 1);
    list_url[list_len - 1] = '\0';

    const char *from[] = {
        "/fe/service/identity/authStart",
        "/identity/authStart",
        "identity/authStart",
        NULL,
    };
    for (int i = 0; from[i]; i++) {
        char *pos = strstr(list_url, from[i]);
        if (pos) {
            const char *to = (from[i][0] == '/') ? "/identity/list" : "identity/list";
            size_t tail = strlen(pos + strlen(from[i])) + 1;
            memmove(pos + strlen(to), pos + strlen(from[i]), tail);
            memcpy(pos, to, strlen(to));
            return;
        }
    }
}

static esp_err_t make_identity_session_jar(const char *jar, char *out, size_t out_len)
{
    if (!out || out_len < 32) {
        return ESP_ERR_INVALID_ARG;
    }

    char id_sess[PENDING_IDENTITY_SESSION_MAX];
    if (jar && extract_cookie_value(jar, "identity_session", id_sess, sizeof(id_sess)) == ESP_OK &&
        id_sess[0]) {
        snprintf(out, out_len, "identity_session=%s", id_sess);
        return ESP_OK;
    }

    if (extract_cookie_value(s_pending_jar, "identity_session", id_sess, sizeof(id_sess)) == ESP_OK &&
        id_sess[0]) {
        snprintf(out, out_len, "identity_session=%s", id_sess);
        return ESP_OK;
    }

    char stored_jar[PENDING_JAR_MAX];
    if (nvs_get_pending(NULL, 0, stored_jar, sizeof(stored_jar)) != ESP_OK ||
        extract_cookie_value(stored_jar, "identity_session", id_sess, sizeof(id_sess)) != ESP_OK ||
        !id_sess[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(out, out_len, "identity_session=%s", id_sess);
    return ESP_OK;
}

/** identity_session + optional ick (图形验证码 cookie). */
static esp_err_t make_2fa_send_jar(const char *jar, char *out, size_t out_len, bool with_ick)
{
    esp_err_t err = make_identity_session_jar(jar, out, out_len);
    if (err != ESP_OK) {
        return err;
    }
    if (!with_ick) {
        return ESP_OK;
    }
    char ick[256];
    if (nvs_get_captcha_ick(ick, sizeof(ick)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t used = strlen(out);
    if (used + 8 + strlen(ick) >= out_len) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(out + used, out_len - used, "; ick=%s", ick);
    return ESP_OK;
}

static void json_captcha_path(const cJSON *j, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!j) {
        return;
    }
    const cJSON *item = cJSON_GetObjectItem(j, "captchaUrl");
    if (!cJSON_IsString(item) || !item->valuestring[0]) {
        return;
    }
    strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
}

static esp_err_t fetch_send_captcha(miot_cloud_session_t *session, const char *captcha_path,
                                    char *jar, size_t jar_len)
{
    if (!captcha_path || captcha_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char url[384];
    if (strncmp(captcha_path, "http", 4) == 0) {
        strncpy(url, captcha_path, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else if (captcha_path[0] == '/') {
        snprintf(url, sizeof(url), "https://account.xiaomi.com%s", captcha_path);
    } else {
        snprintf(url, sizeof(url), "https://account.xiaomi.com/%s", captcha_path);
    }

    char send_jar[PENDING_JAR_MAX];
    esp_err_t err = make_identity_session_jar(jar, send_jar, sizeof(send_jar));
    if (err != ESP_OK) {
        return err;
    }

    ensure_device_id(session);
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, session->device_id);

    miot_http_buf_t resp = {0};
    err = miot_http_request_identity(url, HTTP_METHOD_GET, NULL, ua, send_jar, sizeof(send_jar),
                                     &resp, NULL, false);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }
    if (!resp.body || resp.len == 0) {
        free(resp.body);
        return ESP_FAIL;
    }

    size_t img_len = resp.len;
    err = captcha_b64_store((const uint8_t *)resp.body, img_len);
    free(resp.body);
    if (err != ESP_OK) {
        return err;
    }

    char ick[256];
    if (extract_cookie_value(send_jar, "ick", ick, sizeof(ick)) == ESP_OK && ick[0]) {
        miot_http_cookie_jar_put(jar, jar_len, "ick", ick);
        nvs_set_captcha_ick(ick);
        ESP_LOGI(TAG, "captcha ick saved (%u bytes)", (unsigned)strlen(ick));
    }
    nvs_set_pending_jar(jar);
    ESP_LOGI(TAG, "captcha image %u bytes (base64 %u)", (unsigned)img_len,
             (unsigned)strlen(s_captcha_b64));
    return ESP_OK;
}

static esp_err_t nvs_get_pending_login_params(char *qs, size_t qs_len, char *sign, size_t sign_len)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (qs && qs_len > 0) {
        qs[0] = '\0';
        size_t len = qs_len;
        nvs_get_str(nvs, NVS_P2FA_QS, qs, &len);
    }
    if (sign && sign_len > 0) {
        sign[0] = '\0';
        size_t len = sign_len;
        nvs_get_str(nvs, NVS_P2FA_SIGN, sign, &len);
    }
    nvs_close(nvs);
    return (sign && sign[0]) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t miot_cloud_save_pending_login_params(const char *qs, const char *sign)
{
    if (!sign || !sign[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (qs && qs[0]) {
        nvs_set_str(nvs, NVS_P2FA_QS, qs);
    } else {
        nvs_erase_key(nvs, NVS_P2FA_QS);
    }
    err = nvs_set_str(nvs, NVS_P2FA_SIGN, sign);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

/** Append serviceLogin qs/_sign when result/check URL was truncated or incomplete. */
static void url_ensure_login_params(char *url, size_t url_len)
{
    if (!url || !url[0] || strstr(url, "_sign=")) {
        return;
    }
    char qs[512];
    char sign[128];
    qs[0] = '\0';
    sign[0] = '\0';
    if (nvs_get_pending_login_params(qs, sizeof(qs), sign, sizeof(sign)) != ESP_OK) {
        return;
    }
    char qs_enc[640];
    char sign_enc[192];
    miot_http_urlencode(qs, qs_enc, sizeof(qs_enc));
    miot_http_urlencode(sign, sign_enc, sizeof(sign_enc));
    char suffix[896];
    snprintf(suffix, sizeof(suffix), "%s_sign=%s&qs=%s", strchr(url, '?') ? "&" : "?", sign_enc,
             qs_enc);
    size_t cur = strlen(url);
    if (cur + strlen(suffix) + 1 <= url_len) {
        strncat(url, suffix, url_len - cur - 1);
        ESP_LOGI(TAG, "2FA finish: appended qs/_sign to url (len=%u)", (unsigned)strlen(url));
    } else {
        ESP_LOGW(TAG, "2FA finish: url too short for qs/_sign append");
    }
}

static const char *auth_key_for_flag(int flag)
{
    return flag == FLAG_PHONE ? "Phone" : "Email";
}

static void jar_copy_cookie(const char *src_jar, char *dst_jar, size_t dst_len, const char *name)
{
    char val[PENDING_IDENTITY_SESSION_MAX];
    if (extract_cookie_value(src_jar, name, val, sizeof(val)) != ESP_OK || !val[0]) {
        return;
    }
    if (strcmp(name, "identity_session") == 0 || strcmp(name, "passToken") == 0) {
        miot_http_cookie_jar_put_long(dst_jar, dst_len, name, val);
    } else {
        miot_http_cookie_jar_put(dst_jar, dst_len, name, val);
    }
}

static void jar_merge_identity_cookies(const char *src_jar, char *dst_jar, size_t dst_len)
{
    static const char *names[] = {"identity_session", "userId", "passToken", "cUserId", "ick", NULL};
    for (int i = 0; names[i]; i++) {
        jar_copy_cookie(src_jar, dst_jar, dst_len, names[i]);
    }
}

static cJSON *parse_xiaomi_json(const char *raw);
static void resolve_redirect_url(const char *base, const char *loc, char *out, size_t out_len);
static void ext_pragma_apply_session(const char *ext, miot_cloud_session_t *session);

static bool body_find_sts_url(const char *body, char *out, size_t out_len)
{
    if (!body || !out || out_len == 0) {
        return false;
    }
    const char *p = strstr(body, "https://sts.api.io.mi.com/sts");
    if (!p) {
        return false;
    }
    size_t i = 0;
    while (p[i] && p[i] != '"' && p[i] != '\'' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t http_get_one_hop(const char *url, const char *ua, char *jar, size_t jar_len,
                                  miot_http_buf_t *resp, miot_http_extra_t *extra)
{
    if (strstr(url, "sts.api.io.mi.com")) {
        return miot_http_request_ex(url, HTTP_METHOD_GET, NULL, ua, jar, jar_len, resp, NULL, 0,
                                    extra, true);
    }
    return miot_http_request_identity(url, HTTP_METHOD_GET, NULL, ua, jar, jar_len, resp, extra,
                                    true);
}

/**
 * AlexxIT login_verify + _get_credentials + token_extractor Auth2/end/STS chain.
 * Manual redirect hops: capture extension-pragma (ssecurity) and serviceToken cookies.
 */
static esp_err_t fetch_credentials_from_location(const char *location, const char *ua, char *jar,
                                               size_t jar_len, miot_cloud_session_t *session)
{
    char url[MIOT_2FA_URL_MAX];
    strncpy(url, location, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';

    if (strstr(url, "identity/result/check")) {
        url_ensure_login_params(url, sizeof(url));
    }

    ESP_LOGI(TAG, "2FA credentials: url len=%u", (unsigned)strlen(url));

    bool sign_retried = false;

    for (int hop = 0; hop < 10; hop++) {
        miot_http_buf_t resp = {0};
        miot_http_extra_t extra = {0};
        esp_err_t err = http_get_one_hop(url, ua, jar, jar_len, &resp, &extra);
        if (err != ESP_OK) {
            free(resp.body);
            return err;
        }

        if (extra.ext_pragma[0]) {
            ext_pragma_apply_session(extra.ext_pragma, session);
        }
        if (session->user_id[0] == '\0') {
            extract_cookie_value(jar, "userId", session->user_id, sizeof(session->user_id));
        }

        char service_token[MIOT_CLOUD_TOKEN_MAX];
        bool have_token =
            extract_cookie_value(jar, "serviceToken", service_token, sizeof(service_token)) == ESP_OK;

        ESP_LOGI(TAG, "2FA hop %d: status=%d ssec=%d token=%d",
                 hop, extra.http_status, session->ssecurity[0] != '\0', have_token);

        if (session->ssecurity[0] && have_token) {
            strncpy(session->service_token, service_token, sizeof(session->service_token) - 1);
            free(resp.body);
            return ESP_OK;
        }

        /* Xiaomi Tips interstitial: repeat same URL */
        if (resp.body && strstr(resp.body, "Xiaomi Account - Tips")) {
            free(resp.body);
            continue;
        }

        /* API error (e.g. missing _sign): append qs/_sign once and retry */
        if (!sign_retried && resp.body && strstr(url, "result/check") && !strstr(url, "_sign=")) {
            cJSON *j = parse_xiaomi_json(resp.body);
            if (j) {
                const cJSON *code = cJSON_GetObjectItem(j, "code");
                int api_code = cJSON_IsNumber(code) ? code->valueint : 0;
                cJSON_Delete(j);
                if (api_code != 0) {
                    url_ensure_login_params(url, sizeof(url));
                    if (strstr(url, "_sign=")) {
                        sign_retried = true;
                        free(resp.body);
                        ESP_LOGI(TAG, "2FA credentials: retry with qs/_sign");
                        continue;
                    }
                }
            }
        }

        char next[MIOT_2FA_URL_MAX];
        next[0] = '\0';

        if (is_redirect_status(extra.http_status) && extra.location[0]) {
            resolve_redirect_url(url, extra.location, next, sizeof(next));
        } else if (resp.body) {
            cJSON *j = parse_xiaomi_json(resp.body);
            if (j) {
                const cJSON *loc = cJSON_GetObjectItem(j, "location");
                if (cJSON_IsString(loc) && loc->valuestring[0]) {
                    resolve_redirect_url(url, loc->valuestring, next, sizeof(next));
                }
                cJSON_Delete(j);
            }
            if (next[0] == '\0') {
                body_find_sts_url(resp.body, next, sizeof(next));
            }
        }

        free(resp.body);

        if (next[0] == '\0') {
            break;
        }

        strncpy(url, next, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    }

    char service_token[MIOT_CLOUD_TOKEN_MAX];
    if (session->ssecurity[0] &&
        extract_cookie_value(jar, "serviceToken", service_token, sizeof(service_token)) == ESP_OK) {
        strncpy(session->service_token, service_token, sizeof(session->service_token) - 1);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "2FA: credentials incomplete ssec=%d token=%d",
             session->ssecurity[0] != '\0',
             extract_cookie_value(jar, "serviceToken", service_token, sizeof(service_token)) == ESP_OK);
    return ESP_FAIL;
}

static void ext_pragma_apply_session(const char *ext, miot_cloud_session_t *session)
{
    if (!ext || !ext[0] || !session) {
        return;
    }
    cJSON *j = cJSON_Parse(ext);
    if (!j) {
        return;
    }
    const cJSON *ss = cJSON_GetObjectItem(j, "ssecurity");
    if (cJSON_IsString(ss) && ss->valuestring[0]) {
        strncpy(session->ssecurity, ss->valuestring, sizeof(session->ssecurity) - 1);
        session->ssecurity[sizeof(session->ssecurity) - 1] = '\0';
    }
    if (session->user_id[0] == '\0') {
        const cJSON *uid = cJSON_GetObjectItem(j, "userId");
        if (cJSON_IsNumber(uid)) {
            snprintf(session->user_id, sizeof(session->user_id), "%d", uid->valueint);
        } else if (cJSON_IsString(uid) && uid->valuestring[0]) {
            strncpy(session->user_id, uid->valuestring, sizeof(session->user_id) - 1);
            session->user_id[sizeof(session->user_id) - 1] = '\0';
        }
    }
    cJSON_Delete(j);
}

static void resolve_redirect_url(const char *base, const char *loc, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!loc || !loc[0]) {
        return;
    }
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        strncpy(out, loc, out_len - 1);
    } else if (loc[0] == '/') {
        const char *host = "https://account.xiaomi.com";
        if (base && strstr(base, "sts.api.io.mi.com")) {
            host = "https://sts.api.io.mi.com";
        }
        snprintf(out, out_len, "%s%s", host, loc);
    } else {
        snprintf(out, out_len, "https://account.xiaomi.com/%s", loc);
    }
    out[out_len - 1] = '\0';
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

static bool xiaomi_json_ok(const char *raw)
{
    cJSON *j = parse_xiaomi_json(raw);
    if (!j) {
        return raw && raw[0] != '\0';
    }
    const cJSON *code = cJSON_GetObjectItem(j, "code");
    bool ok = !cJSON_IsNumber(code) || code->valueint == 0;
    if (!ok) {
        const cJSON *desc = cJSON_GetObjectItem(j, "desc");
        if (!cJSON_IsString(desc)) {
            desc = cJSON_GetObjectItem(j, "description");
        }
        ESP_LOGW(TAG, "Xiaomi API code=%d desc=%s",
                 cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_IsString(desc) ? desc->valuestring : "?");
    }
    cJSON_Delete(j);
    return ok;
}

static int pick_2fa_flag(const cJSON *j)
{
    const cJSON *flag = cJSON_GetObjectItem(j, "flag");
    int list_flag = cJSON_IsNumber(flag) ? flag->valueint : FLAG_PHONE;

    bool has_phone = list_flag == FLAG_PHONE;
    bool has_email = list_flag == FLAG_EMAIL;
    const cJSON *options = cJSON_GetObjectItem(j, "options");
    if (cJSON_IsArray(options)) {
        has_phone = false;
        has_email = false;
        for (int i = 0; i < cJSON_GetArraySize(options); i++) {
            const cJSON *opt = cJSON_GetArrayItem(options, i);
            if (!cJSON_IsNumber(opt)) {
                continue;
            }
            if (opt->valueint == FLAG_PHONE) {
                has_phone = true;
            }
            if (opt->valueint == FLAG_EMAIL) {
                has_email = true;
            }
        }
    }

    if (has_phone) {
        return FLAG_PHONE;
    }
    if (has_email) {
        ESP_LOGW(TAG, "identity/list: phone 2FA unavailable, email only");
        miot_cloud_set_last_error("该账号仅支持邮箱验证，MiniHub 仅支持手机短信验证");
        return -1;
    }
    return list_flag;
}

static esp_err_t identity_list_fetch(miot_cloud_session_t *session, char *jar, size_t jar_len,
                                     int *out_flag)
{
    char pending[MIOT_2FA_URL_MAX];
    esp_err_t err = nvs_get_pending(pending, sizeof(pending), jar, jar_len);
    if (err != ESP_OK) {
        return err;
    }
    if (jar[0] == '\0') {
        ESP_LOGE(TAG, "2FA cookie jar missing, login again");
        return ESP_ERR_INVALID_STATE;
    }

    char list_url[MIOT_2FA_URL_MAX];
    notification_to_list_url(pending, list_url, sizeof(list_url));
    if (strcmp(list_url, pending) == 0) {
        ESP_LOGE(TAG, "pending URL is not authStart, login again");
        return ESP_ERR_INVALID_STATE;
    }

    ensure_device_id(session);
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, session->device_id);

    ESP_LOGI(TAG, "identity/list GET url_len=%u jar_len=%u", (unsigned)strlen(list_url),
             (unsigned)strlen(jar));
    miot_http_buf_t resp = {0};
    err = miot_http_request_identity(list_url, HTTP_METHOD_GET, NULL, ua, jar, jar_len, &resp, NULL,
                                     false);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }
    log_jar_hint("after identity/list", jar);

    cJSON *j = parse_xiaomi_json(resp.body);
    if (!j) {
        ESP_LOGE(TAG, "identity/list: invalid JSON");
        free(resp.body);
        return ESP_FAIL;
    }
    const cJSON *code = cJSON_GetObjectItem(j, "code");
    int list_code = cJSON_IsNumber(code) ? code->valueint : -1;
    int list_flag = pick_2fa_flag(j);
    if (list_code != 2) {
        ESP_LOGW(TAG, "identity/list unexpected code=%d", list_code);
    }
    cJSON_Delete(j);
    free(resp.body);

    if (list_flag != FLAG_PHONE && list_flag != FLAG_EMAIL) {
        ESP_LOGE(TAG, "identity/list: no phone 2FA");
        return ESP_FAIL;
    }

    char id_check[48];
    if (extract_cookie_value(jar, "identity_session", id_check, sizeof(id_check)) != ESP_OK ||
        !id_check[0]) {
        ESP_LOGE(TAG, "identity/list: identity_session cookie missing");
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_set_pending_jar(jar);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identity/list: save jar failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_pending_flag(list_flag);
    if (out_flag) {
        *out_flag = list_flag;
    }
    ESP_LOGI(TAG, "identity/list flag=%d (%s)", list_flag, auth_key_for_flag(list_flag));
    return ESP_OK;
}

static esp_err_t identity_verify_prepare(miot_cloud_session_t *session, int flag, const char *jar)
{
    char session_jar[PENDING_JAR_MAX];
    esp_err_t err = make_identity_session_jar(jar, session_jar, sizeof(session_jar));
    if (err != ESP_OK) {
        return err;
    }

    char url[256];
    snprintf(url, sizeof(url),
             "https://account.xiaomi.com/identity/auth/verify%s?_flag=%d&_json=true",
             auth_key_for_flag(flag), flag);

    ensure_device_id(session);
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, session->device_id);

    miot_http_buf_t resp = {0};
    err = miot_http_request_identity(url, HTTP_METHOD_GET, NULL, ua, session_jar, sizeof(session_jar),
                                     &resp, NULL, false);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }

    cJSON *j = parse_xiaomi_json(resp.body);
    bool ok = false;
    if (j) {
        const cJSON *code = cJSON_GetObjectItem(j, "code");
        int api_code = cJSON_IsNumber(code) ? code->valueint : -1;
        ok = api_code == 0;
        if (!ok) {
            const cJSON *desc = cJSON_GetObjectItem(j, "desc");
            ESP_LOGW(TAG, "verify prepare code=%d desc=%s", api_code,
                     cJSON_IsString(desc) ? desc->valuestring : "?");
        }
        cJSON_Delete(j);
    }
    free(resp.body);
    log_jar_hint("after verify prepare", session_jar);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_ticket_request(miot_cloud_session_t *session, int flag, char *jar,
                                     size_t jar_len, const char *icode_in)
{
    char icode_buf[32];
    const char *icode = icode_in;
    bool use_ick = icode_in && icode_in[0];

    if (use_ick) {
        strncpy(icode_buf, icode_in, sizeof(icode_buf) - 1);
        icode_buf[sizeof(icode_buf) - 1] = '\0';
        size_t n = icode_sanitize(icode_buf, sizeof(icode_buf));
        icode = icode_buf;
        if (!icode_chars_valid(icode)) {
            ESP_LOGW(TAG, "icode rejected sanitized=\"%s\" len=%u raw_len=%u", icode_buf,
                     (unsigned)n, (unsigned)strlen(icode_in));
            miot_cloud_set_last_error("图形验证码应为3-8位字母或数字");
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(s_last_icode_sent, icode_buf, sizeof(s_last_icode_sent) - 1);
        s_last_icode_sent[sizeof(s_last_icode_sent) - 1] = '\0';
        ESP_LOGI(TAG, "icode submit icode=\"%s\" raw_len=%u", s_last_icode_sent,
                 (unsigned)strlen(icode_in));
    } else {
        s_last_icode_sent[0] = '\0';
    }

    char send_jar[PENDING_JAR_MAX];
    esp_err_t err = make_2fa_send_jar(jar, send_jar, sizeof(send_jar), use_ick);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            miot_cloud_set_last_error("图形验证码已过期，请重新获取");
        }
        return err;
    }

    ensure_device_id(session);
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, session->device_id);

    char url[128];
    snprintf(url, sizeof(url), "https://account.xiaomi.com/identity/auth/send%sTicket",
             auth_key_for_flag(flag));

    char body[320];
    if (use_ick) {
        char ick_check[256];
        if (nvs_get_captcha_ick(ick_check, sizeof(ick_check)) != ESP_OK) {
            miot_cloud_set_last_error("图形验证码已过期，请重新获取");
            return ESP_ERR_INVALID_STATE;
        }
        char icode_enc[64];
        miot_http_urlencode(icode, icode_enc, sizeof(icode_enc));
        snprintf(body, sizeof(body), "retry=0&icode=%s&_json=true", icode_enc);
        ESP_LOGI(TAG, "send%sTicket icode_len=%u ick_len=%u", auth_key_for_flag(flag),
                 (unsigned)strlen(icode), (unsigned)strlen(ick_check));
    } else {
        snprintf(body, sizeof(body), "retry=0&icode=&_json=true");
        ESP_LOGI(TAG, "send%sTicket (identity_session only)", auth_key_for_flag(flag));
    }

    miot_http_buf_t resp = {0};
    err = miot_http_request_identity(url, HTTP_METHOD_POST, body, ua, send_jar, sizeof(send_jar),
                                     &resp, NULL, false);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }

    cJSON *j = parse_xiaomi_json(resp.body);
    if (!j) {
        free(resp.body);
        return ESP_FAIL;
    }

    const cJSON *code = cJSON_GetObjectItem(j, "code");
    int api_code = cJSON_IsNumber(code) ? code->valueint : -1;
    if (api_code == 0) {
        jar_copy_cookie(send_jar, jar, jar_len, "identity_session");
        jar_copy_cookie(send_jar, jar, jar_len, "ick");
        cJSON_Delete(j);
        free(resp.body);
        captcha_b64_clear();
        return ESP_OK;
    }

    char captcha_path[256];
    json_captcha_path(j, captcha_path, sizeof(captcha_path));
    const cJSON *desc = cJSON_GetObjectItem(j, "desc");
    if (!cJSON_IsString(desc)) {
        desc = cJSON_GetObjectItem(j, "description");
    }
    ESP_LOGW(TAG, "sendTicket code=%d desc=%s captcha=%d", api_code,
             cJSON_IsString(desc) ? desc->valuestring : "?", captcha_path[0] != '\0');
    cJSON_Delete(j);
    free(resp.body);

    if (captcha_path[0]) {
        err = fetch_send_captcha(session, captcha_path, jar, jar_len);
        if (err != ESP_OK) {
            miot_cloud_set_last_error("获取图形验证码失败");
            return err;
        }
        miot_cloud_set_last_error(use_ick ? "图形验证码错误，请按新图片重新输入"
                                          : "请输入图形验证码后再次点击发送");
        return ESP_ERR_INVALID_STATE;
    }

    if (cJSON_IsString(desc) && desc->valuestring[0]) {
        miot_cloud_set_last_error(desc->valuestring);
    }
    return ESP_FAIL;
}

esp_err_t miot_cloud_save_pending_2fa(const char *notification_url, const char *cookie_jar)
{
    if (!notification_url) {
        return ESP_ERR_INVALID_ARG;
    }
    char abs[MIOT_2FA_URL_MAX];
    abs_notification_url(notification_url, abs, sizeof(abs));
    if (!abs[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Always keep RAM copy so send/verify works even if NVS is full. */
    pending_ram_set(abs, cookie_jar);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("miot_cloud", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "2FA pending NVS open failed (%s) — using RAM only", esp_err_to_name(err));
        log_jar_hint("2FA pending saved (RAM)", cookie_jar ? cookie_jar : "");
        return ESP_OK;
    }

    /* Free prior blob first — helps when partition is fragmented/full. */
    nvs_erase_key(nvs, NVS_P2FA_URL);
    nvs_erase_key(nvs, NVS_P2FA_JAR);

    esp_err_t url_err = nvs_set_str(nvs, NVS_P2FA_URL, abs);
    esp_err_t jar_err = ESP_OK;
    if (cookie_jar && cookie_jar[0]) {
        jar_err = nvs_set_str(nvs, NVS_P2FA_JAR, cookie_jar);
        char did[MIOT_CLOUD_DEVICE_ID_MAX];
        if (extract_cookie_value(cookie_jar, "deviceId", did, sizeof(did)) == ESP_OK) {
            nvs_set_str(nvs, NVS_P2FA_DEVID, did);
        }
    }
    if (url_err != ESP_OK || jar_err != ESP_OK) {
        ESP_LOGW(TAG, "2FA pending NVS write url=%s jar=%s — RAM fallback active",
                 esp_err_to_name(url_err), esp_err_to_name(jar_err));
    }
    err = nvs_commit(nvs);
    nvs_close(nvs);
    log_jar_hint("2FA pending saved", cookie_jar ? cookie_jar : "");
    /* Log path shape so we can spot Xiaomi URL format changes. */
    {
        const char *p = strstr(abs, "identity/");
        ESP_LOGI(TAG, "2FA pending url_len=%u path_hint=%s nvs_url=%s",
                 (unsigned)strlen(abs), p ? p : "(no identity/)",
                 url_err == ESP_OK ? "ok" : esp_err_to_name(url_err));
    }
    /* RAM is authoritative for this boot; NVS is best-effort persistence. */
    (void)err;
    (void)url_err;
    return ESP_OK;
}

esp_err_t miot_cloud_fetch_passport_captcha(miot_cloud_session_t *session, const char *captcha_path,
                                            char *jar, size_t jar_len)
{
    if (!session || !captcha_path || !captcha_path[0] || !jar || jar_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[384];
    if (strncmp(captcha_path, "http", 4) == 0) {
        strncpy(url, captcha_path, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
    } else if (captcha_path[0] == '/') {
        snprintf(url, sizeof(url), "https://account.xiaomi.com%s", captcha_path);
    } else {
        snprintf(url, sizeof(url), "https://account.xiaomi.com/%s", captcha_path);
    }

    char ua[256];
    strncpy(ua, MIOT_PASSPORT_UA, sizeof(ua) - 1);
    ua[sizeof(ua) - 1] = '\0';

    miot_http_buf_t resp = {0};
    esp_err_t err = miot_http_request(url, HTTP_METHOD_GET, NULL, ua, jar, jar_len, &resp, NULL, 0);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }
    if (!resp.body || resp.len == 0) {
        free(resp.body);
        return ESP_FAIL;
    }

    err = captcha_b64_store((const uint8_t *)resp.body, resp.len);
    free(resp.body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "login captcha stored b64_len=%u", (unsigned)strlen(s_captcha_b64));
    }
    return err;
}

esp_err_t miot_cloud_login_retry(const char *username, const char *password,
                                 miot_cloud_session_t *session)
{
    return miot_cloud_login(username, password, NULL, session, NULL, 0);
}

esp_err_t miot_cloud_send_2fa_email(miot_cloud_session_t *session, const char *icode)
{
    if (!session) {
        return ESP_ERR_INVALID_ARG;
    }

    char jar[PENDING_JAR_MAX];
    int flag = FLAG_PHONE;
    esp_err_t err;
    const char *send_icode = icode;

    if (send_icode && send_icode[0]) {
        char ick[256];
        ick[0] = '\0';
        const bool captcha_retry = (nvs_get_captcha_ick(ick, sizeof(ick)) == ESP_OK);
        if (!captcha_retry) {
            /* Not captcha retry (e.g. user typed SMS digits on /miot send) — use list flow. */
            ESP_LOGW(TAG, "send: icode without captcha state, running identity/list");
            send_icode = NULL;
        } else {
            /* Captcha retry: skip identity/list (re-list invalidates captcha ick). */
            err = nvs_get_pending(NULL, 0, jar, sizeof(jar));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "send: pending 2FA state missing");
                return err;
            }
            if (nvs_get_pending_flag(&flag) != ESP_OK) {
                flag = FLAG_PHONE;
            }
            ESP_LOGI(TAG, "send with icode (skip list, flag=%d)", flag);
        }
    }
    if (!send_icode || !send_icode[0]) {
        err = identity_list_fetch(session, jar, sizeof(jar), &flag);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "send: identity/list failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = identity_verify_prepare(session, flag, jar);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send: verify prepare failed: %s", esp_err_to_name(err));
        return err;
    }

    err = send_ticket_request(session, flag, jar, sizeof(jar), send_icode);
    if (err != ESP_OK) {
        if (err != ESP_ERR_INVALID_STATE || !miot_cloud_last_2fa_captcha_b64()) {
            ESP_LOGE(TAG, "send: sendTicket failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    nvs_set_pending_jar(jar);
    log_jar_hint("after sendTicket", jar);
    miot_cloud_set_last_error(NULL);
    ESP_LOGI(TAG, "2FA code sent (%s), submit via /api/miot/login/verify",
             auth_key_for_flag(flag));
    return ESP_OK;
}

esp_err_t miot_cloud_verify_2fa_ticket(const char *ticket, miot_cloud_session_t *session)
{
    if (!ticket || !session || ticket[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char jar[PENDING_JAR_MAX];
    esp_err_t err = nvs_get_pending(NULL, 0, jar, sizeof(jar));
    if (err != ESP_OK) {
        return err;
    }

    int flag = FLAG_PHONE;
    if (nvs_get_pending_flag(&flag) != ESP_OK) {
        err = identity_list_fetch(session, jar, sizeof(jar), &flag);
        if (err != ESP_OK) {
            return err;
        }
    }

    char session_jar[PENDING_JAR_MAX];
    err = make_identity_session_jar(jar, session_jar, sizeof(session_jar));
    if (err != ESP_OK) {
        return err;
    }

    ensure_device_id(session);
    char ua[160];
    snprintf(ua, sizeof(ua), UA_FMT, session->device_id);

    char ticket_enc[32];
    miot_http_urlencode(ticket, ticket_enc, sizeof(ticket_enc));

    char url[384];
    snprintf(url, sizeof(url),
             "https://account.xiaomi.com/identity/auth/verify%s?_flag=%d&ticket=%s&trust=false&_json=true",
             auth_key_for_flag(flag), flag, ticket_enc);

    ESP_LOGI(TAG, "verify%s ticket_len=%u", auth_key_for_flag(flag), (unsigned)strlen(ticket));

    miot_http_buf_t resp = {0};
    err = miot_http_request_identity(url, HTTP_METHOD_POST, NULL, ua, session_jar,
                                     sizeof(session_jar), &resp, NULL, false);
    if (err != ESP_OK) {
        free(resp.body);
        return err;
    }
    if (!xiaomi_json_ok(resp.body)) {
        free(resp.body);
        return ESP_FAIL;
    }

    cJSON *verify_json = parse_xiaomi_json(resp.body);
    free(resp.body);
    resp.body = NULL;
    if (!verify_json) {
        return ESP_FAIL;
    }

    const cJSON *location = cJSON_GetObjectItem(verify_json, "location");
    const cJSON *user_id = cJSON_GetObjectItem(verify_json, "userId");
    const cJSON *ssecurity = cJSON_GetObjectItem(verify_json, "ssecurity");
    if (cJSON_IsString(user_id) && user_id->valuestring[0]) {
        strncpy(session->user_id, user_id->valuestring, sizeof(session->user_id) - 1);
        session->user_id[sizeof(session->user_id) - 1] = '\0';
    }
    if (cJSON_IsString(ssecurity) && ssecurity->valuestring[0]) {
        strncpy(session->ssecurity, ssecurity->valuestring, sizeof(session->ssecurity) - 1);
        session->ssecurity[sizeof(session->ssecurity) - 1] = '\0';
    }

    if (!cJSON_IsString(location) || !location->valuestring[0]) {
        char *raw = cJSON_PrintUnformatted(verify_json);
        ESP_LOGE(TAG, "2FA verify: missing location in response");
        if (raw) {
            ESP_LOGW(TAG, "verify body: %.200s", raw);
            free(raw);
        }
        cJSON_Delete(verify_json);
        return ESP_FAIL;
    }

    char finish_url[MIOT_2FA_URL_MAX];
    size_t loc_len = strlen(location->valuestring);
    strncpy(finish_url, location->valuestring, sizeof(finish_url) - 1);
    finish_url[sizeof(finish_url) - 1] = '\0';
    cJSON_Delete(verify_json);

    if (loc_len >= sizeof(finish_url)) {
        ESP_LOGW(TAG, "2FA location truncated %u -> %u", (unsigned)loc_len,
                 (unsigned)(sizeof(finish_url) - 1));
    }

    jar_merge_identity_cookies(session_jar, jar, sizeof(jar));
    log_jar_hint("after verifyPhone", jar);

    ESP_LOGI(TAG, "2FA finish location len=%u", (unsigned)strlen(finish_url));

    err = fetch_credentials_from_location(finish_url, ua, jar, sizeof(jar), session);
    if (err != ESP_OK) {
        return err;
    }

    if (session->region[0] == '\0') {
        strncpy(session->region, "cn", sizeof(session->region) - 1);
    }
    session->device_id_pinned = true;
    session->valid = true;
    nvs_clear_pending();
    ESP_LOGI(TAG, "2FA verify ok, user %s", session->user_id);
    return miot_cloud_session_save(session);
}
