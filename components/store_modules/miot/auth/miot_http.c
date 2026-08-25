/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "miot_http.h"
#include "minihub_http_worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdarg.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "miot_http";
static char s_last_get_error[1024];

const char *miot_http_last_get_error(void)
{
    return s_last_get_error[0] ? s_last_get_error : NULL;
}

static void set_get_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_get_error, sizeof(s_last_get_error), fmt, ap);
    va_end(ap);
}

void miot_http_client_config_init(esp_http_client_config_t *cfg, const char *url)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 30000;
    cfg->buffer_size = 4096;
    cfg->buffer_size_tx = 2048;
    /* Cloud APIs return 401 for bad cookies/signatures; do not treat as HTTP auth challenge */
    cfg->max_authorization_retries = -1;
    /* Do not pin account.xiaomi.com: Xiaomi rotated off GoDaddy Secure G2
     * (2026-08-16) onto GoDaddy TLS Root R1. Mozilla bundle covers that chain. */
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
}

size_t miot_http_urlencode(const char *src, char *dst, size_t dst_len)
{
    if (!src || !dst || dst_len == 0) {
        return 0;
    }
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0'; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            if (di + 1 >= dst_len) {
                break;
            }
            dst[di++] = (char)c;
        } else {
            if (di + 3 >= dst_len) {
                break;
            }
            snprintf(dst + di, dst_len - di, "%%%02X", c);
            di += 3;
        }
    }
    dst[di] = '\0';
    return di;
}

static size_t jar_remain(const char *jar, size_t jar_len)
{
    size_t used = jar ? strlen(jar) : 0;
    return used < jar_len ? jar_len - used : 0;
}

#define COOKIE_VALUE_MAX 512
#define COOKIE_PAIR_MAX  (40 + COOKIE_VALUE_MAX)

void miot_http_cookie_jar_put(char *jar, size_t jar_len, const char *name, const char *value)
{
    if (!jar || jar_len == 0 || !name || !name[0] || !value) {
        return;
    }
    if (strcmp(value, "EXPIRED") == 0) {
        return;
    }

    char pair[COOKIE_PAIR_MAX];
    int n = snprintf(pair, sizeof(pair), "%s=%s", name, value);
    if (n < 0 || (size_t)n >= sizeof(pair)) {
        ESP_LOGW(TAG, "cookie %s value too long (%u), skip", name, (unsigned)strlen(value));
        return;
    }
    if (jar_remain(jar, jar_len) <= (size_t)n + 2) {
        ESP_LOGW(TAG, "cookie jar full, skip %s", name);
        return;
    }

    char pattern[40];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    char *pos = strstr(jar, pattern);
    if (pos) {
        char *end = strchr(pos, ';');
        if (end) {
            size_t tail = strlen(end);
            memmove(pos, end + 1, tail);
            if (jar[0] != '\0' && jar[strlen(jar) - 1] != ';') {
                strncat(jar, ";", jar_remain(jar, jar_len) - 1);
            }
        } else {
            *pos = '\0';
        }
    }
    if (jar[0] != '\0' && jar[strlen(jar) - 1] != ';') {
        strncat(jar, ";", jar_remain(jar, jar_len) - 1);
    }
    strncat(jar, pair, jar_remain(jar, jar_len) - 1);
}

void miot_http_cookie_jar_put_long(char *jar, size_t jar_len, const char *name, const char *value)
{
    if (!jar || jar_len == 0 || !name || !name[0] || !value) {
        return;
    }
    if (strcmp(value, "EXPIRED") == 0) {
        return;
    }

    char *pair = malloc(strlen(name) + strlen(value) + 2);
    if (!pair) {
        ESP_LOGW(TAG, "cookie alloc failed for %s", name);
        return;
    }
    snprintf(pair, strlen(name) + strlen(value) + 2, "%s=%s", name, value);
    if (jar_remain(jar, jar_len) <= strlen(pair) + 2) {
        ESP_LOGW(TAG, "cookie jar full, skip %s", name);
        free(pair);
        return;
    }

    char pattern[40];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    char *pos = strstr(jar, pattern);
    if (pos) {
        char *end = strchr(pos, ';');
        if (end) {
            size_t tail = strlen(end);
            memmove(pos, end + 1, tail);
            if (jar[0] != '\0' && jar[strlen(jar) - 1] != ';') {
                strncat(jar, ";", jar_remain(jar, jar_len) - 1);
            }
        } else {
            *pos = '\0';
        }
    }
    if (jar[0] != '\0' && jar[strlen(jar) - 1] != ';') {
        strncat(jar, ";", jar_remain(jar, jar_len) - 1);
    }
    strncat(jar, pair, jar_remain(jar, jar_len) - 1);
    free(pair);
}

void miot_http_cookie_jar_merge(char *jar, size_t jar_len, const char *set_cookie)
{
    if (!set_cookie || !jar || jar_len == 0) {
        return;
    }
    char name[32];
    const char *eq = strchr(set_cookie, '=');
    if (!eq || (size_t)(eq - set_cookie) >= sizeof(name)) {
        return;
    }
    memcpy(name, set_cookie, (size_t)(eq - set_cookie));
    name[eq - set_cookie] = '\0';

    char value[COOKIE_VALUE_MAX];
    const char *semi = strchr(eq + 1, ';');
    size_t vlen = semi ? (size_t)(semi - eq - 1) : strlen(eq + 1);
    if (vlen >= sizeof(value)) {
        if (strcmp(name, "passToken") == 0 || strcmp(name, "identity_session") == 0 ||
            strcmp(name, "ick") == 0) {
            char *full = malloc(vlen + 1);
            if (!full) {
                return;
            }
            memcpy(full, eq + 1, vlen);
            full[vlen] = '\0';
            miot_http_cookie_jar_put_long(jar, jar_len, name, full);
            free(full);
            return;
        }
        vlen = sizeof(value) - 1;
    }
    memcpy(value, eq + 1, vlen);
    value[vlen] = '\0';
    if (strcmp(name, "passToken") == 0 || strcmp(name, "identity_session") == 0 ||
        strcmp(name, "ick") == 0) {
        miot_http_cookie_jar_put_long(jar, jar_len, name, value);
    } else {
        miot_http_cookie_jar_put(jar, jar_len, name, value);
    }
}

typedef struct {
    char *jar;
    size_t jar_len;
    char *set_cookie_out;
    size_t set_cookie_len;
    miot_http_buf_t *resp;
    miot_http_extra_t *extra;
} miot_http_req_ctx_t;

static esp_err_t http_buf_append(miot_http_buf_t *buf, const char *data, size_t len)
{
    if (!buf) {
        return ESP_OK;
    }
    if (buf->max_len && buf->len + len > buf->max_len) {
        ESP_LOGW(TAG, "HTTP body exceeds cap %u", (unsigned)buf->max_len);
        return ESP_ERR_NO_MEM;
    }
    size_t ncap = buf->cap ? buf->cap : 256;
    while (ncap < buf->len + len + 1) {
        ncap *= 2;
    }
    if (buf->max_len && ncap > buf->max_len + 1) {
        ncap = buf->max_len + 1;
    }
    char *nb = realloc(buf->body, ncap);
    if (!nb) {
        return ESP_ERR_NO_MEM;
    }
    buf->body = nb;
    buf->cap = ncap;
    memcpy(buf->body + buf->len, data, len);
    buf->len += len;
    buf->body[buf->len] = '\0';
    return ESP_OK;
}

static esp_err_t miot_http_event_handler(esp_http_client_event_t *evt)
{
    miot_http_req_ctx_t *ctx = evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (!evt->header_key || !evt->header_value) {
            break;
        }
        if (strcasecmp(evt->header_key, "Set-Cookie") == 0) {
            if (ctx->jar && ctx->jar_len > 0) {
                miot_http_cookie_jar_merge(ctx->jar, ctx->jar_len, evt->header_value);
            }
            if (ctx->set_cookie_out && ctx->set_cookie_len > 0) {
                strncpy(ctx->set_cookie_out, evt->header_value, ctx->set_cookie_len - 1);
                ctx->set_cookie_out[ctx->set_cookie_len - 1] = '\0';
            }
            break;
        }
        if (ctx->extra) {
            if (strcasecmp(evt->header_key, "extension-pragma") == 0) {
                strncpy(ctx->extra->ext_pragma, evt->header_value,
                        sizeof(ctx->extra->ext_pragma) - 1);
                ctx->extra->ext_pragma[sizeof(ctx->extra->ext_pragma) - 1] = '\0';
            } else if (strcasecmp(evt->header_key, "Location") == 0) {
                strncpy(ctx->extra->location, evt->header_value,
                        sizeof(ctx->extra->location) - 1);
                ctx->extra->location[sizeof(ctx->extra->location) - 1] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (ctx->resp && evt->data_len > 0) {
            return http_buf_append(ctx->resp, evt->data, (size_t)evt->data_len);
        }
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP transport error");
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t miot_http_request_ex(const char *url, esp_http_client_method_t method,
                               const char *post_body, const char *user_agent,
                               char *cookie_jar, size_t cookie_jar_len,
                               miot_http_buf_t *resp,
                               char *set_cookie_out, size_t set_cookie_len,
                               miot_http_extra_t *extra, bool no_auto_redirect)
{
    if (resp) {
        resp->body = NULL;
        resp->len = 0;
        resp->cap = 0;
    }
    if (set_cookie_out && set_cookie_len > 0) {
        set_cookie_out[0] = '\0';
    }
    if (extra) {
        extra->ext_pragma[0] = '\0';
        extra->location[0] = '\0';
        extra->http_status = 0;
    }

    esp_http_client_config_t cfg;
    miot_http_client_config_init(&cfg, url);
    cfg.disable_auto_redirect = no_auto_redirect;
    miot_http_req_ctx_t req_ctx = {
        .jar = cookie_jar,
        .jar_len = cookie_jar_len,
        .set_cookie_out = set_cookie_out,
        .set_cookie_len = set_cookie_len,
        .resp = resp,
        .extra = extra,
    };
    cfg.event_handler = miot_http_event_handler;
    cfg.user_data = &req_ctx;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, method);
    if (user_agent) {
        esp_http_client_set_header(client, "User-Agent", user_agent);
    }
    if (cookie_jar && cookie_jar[0]) {
        esp_http_client_set_header(client, "Cookie", cookie_jar);
    }
    if (method == HTTP_METHOD_POST && post_body) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (extra) {
        extra->http_status = status;
        if (extra->location[0] == '\0' &&
            (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            char *loc_hdr = NULL;
            if (esp_http_client_get_header(client, "Location", &loc_hdr) == ESP_OK && loc_hdr &&
                loc_hdr[0]) {
                strncpy(extra->location, loc_hdr, sizeof(extra->location) - 1);
                extra->location[sizeof(extra->location) - 1] = '\0';
            }
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed %s: %s (status=%d errno=%d)",
                 url, esp_err_to_name(err), status, esp_http_client_get_errno(client));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_cleanup(client);

    if (no_auto_redirect &&
        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
        return ESP_OK;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d from %s", status, url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t miot_http_request_identity(const char *url, esp_http_client_method_t method,
                                     const char *post_body, const char *user_agent,
                                     char *cookie_jar, size_t cookie_jar_len,
                                     miot_http_buf_t *resp, miot_http_extra_t *extra,
                                     bool no_auto_redirect)
{
    if (resp) {
        resp->body = NULL;
        resp->len = 0;
        resp->cap = 0;
    }
    if (extra) {
        extra->ext_pragma[0] = '\0';
        extra->location[0] = '\0';
        extra->http_status = 0;
    }

    esp_http_client_config_t cfg;
    miot_http_client_config_init(&cfg, url);
    cfg.disable_auto_redirect = no_auto_redirect;
    miot_http_req_ctx_t req_ctx = {
        .jar = cookie_jar,
        .jar_len = cookie_jar_len,
        .set_cookie_out = NULL,
        .set_cookie_len = 0,
        .resp = resp,
        .extra = extra,
    };
    cfg.event_handler = miot_http_event_handler;
    cfg.user_data = &req_ctx;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, method);
    if (user_agent) {
        esp_http_client_set_header(client, "User-Agent", user_agent);
    }
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    if (cookie_jar && cookie_jar[0]) {
        esp_http_client_set_header(client, "Cookie", cookie_jar);
    }
    if (method == HTTP_METHOD_POST && post_body) {
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (extra) {
        extra->http_status = status;
        if (extra->location[0] == '\0' &&
            (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            char *loc_hdr = NULL;
            if (esp_http_client_get_header(client, "Location", &loc_hdr) == ESP_OK && loc_hdr &&
                loc_hdr[0]) {
                strncpy(extra->location, loc_hdr, sizeof(extra->location) - 1);
                extra->location[sizeof(extra->location) - 1] = '\0';
            }
        }
    }
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "identity perform failed %s: %s (status=%d)",
                 url, esp_err_to_name(err), status);
        return err;
    }
    if (no_auto_redirect &&
        (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
        return ESP_OK;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "identity HTTP %d from %s", status, url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t miot_http_post_cloud_internal(const char *url, const char *post_body,
                                               const char *user_agent, const char *cookie_hdr,
                                               miot_http_buf_t *resp, bool encrypt_rc4)
{
    if (resp) {
        resp->body = NULL;
        resp->len = 0;
        resp->cap = 0;
    }

    esp_http_client_config_t cfg;
    miot_http_client_config_init(&cfg, url);
    miot_http_req_ctx_t req_ctx = {
        .jar = NULL,
        .jar_len = 0,
        .set_cookie_out = NULL,
        .set_cookie_len = 0,
        .resp = resp,
        .extra = NULL,
    };
    cfg.event_handler = miot_http_event_handler;
    cfg.user_data = &req_ctx;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (user_agent) {
        esp_http_client_set_header(client, "User-Agent", user_agent);
    }
    esp_http_client_set_header(client, "x-xiaomi-protocal-flag-cli", "PROTOCAL-HTTP2");
    if (encrypt_rc4) {
        esp_http_client_set_header(client, "MIOT-ENCRYPT-ALGORITHM", "ENCRYPT-RC4");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
    }
    if (cookie_hdr && cookie_hdr[0]) {
        esp_http_client_set_header(client, "Cookie", cookie_hdr);
    }
    if (post_body) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cloud POST failed %s: %s (status=%d errno=%d)",
                 url, esp_err_to_name(err), status, esp_http_client_get_errno(client));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "cloud POST HTTP %d from %s", status, url);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t miot_http_post_cloud(const char *url, const char *post_body,
                               const char *user_agent, const char *cookie_hdr,
                               miot_http_buf_t *resp)
{
    return miot_http_post_cloud_internal(url, post_body, user_agent, cookie_hdr, resp, false);
}

esp_err_t miot_http_post_cloud_rc4(const char *url, const char *post_body,
                                   const char *user_agent, const char *cookie_hdr,
                                   miot_http_buf_t *resp)
{
    return miot_http_post_cloud_internal(url, post_body, user_agent, cookie_hdr, resp, true);
}

static void rewrite_http_to_https(char *url, size_t cap)
{
    if (!url || strncmp(url, "http://", 7) != 0) {
        return;
    }
    size_t n = strlen(url);
    if (n + 1 >= cap) {
        return;
    }
    memmove(url + 8, url + 7, n - 7 + 1);
    url[4] = 's';
}

static esp_err_t miot_http_get_cloud_ex(const char *url, const char *user_agent,
                                        const char *cookie_hdr, miot_http_buf_t *resp,
                                        bool rc4_first)
{
    size_t max_len = resp ? resp->max_len : 0;
    if (resp) {
        resp->body = NULL;
        resp->len = 0;
        resp->cap = 0;
        resp->max_len = max_len;
    }
    if (!url || !url[0]) {
        set_get_error("empty URL");
        return ESP_ERR_INVALID_ARG;
    }

    char *cur_url = malloc(4096);
    if (!cur_url) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(cur_url, url, 4095);
    cur_url[4095] = '\0';

    esp_err_t result = ESP_FAIL;
    int seen_hosts = 0;
    for (int hop = 0; hop < 6; hop++) {
        bool rc4 = rc4_first && hop == 0;
        miot_http_extra_t extra = {0};
        esp_http_client_config_t cfg;
        miot_http_client_config_init(&cfg, cur_url);
        cfg.buffer_size_tx = 4096;
        cfg.buffer_size = 4096;
        cfg.disable_auto_redirect = true;
        miot_http_req_ctx_t req_ctx = {
            .jar = NULL,
            .jar_len = 0,
            .set_cookie_out = NULL,
            .set_cookie_len = 0,
            .resp = resp,
            .extra = &extra,
        };
        cfg.event_handler = miot_http_event_handler;
        cfg.user_data = &req_ctx;
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            result = ESP_ERR_NO_MEM;
            break;
        }

        esp_http_client_set_method(client, HTTP_METHOD_GET);
        if (user_agent) {
            esp_http_client_set_header(client, "User-Agent", user_agent);
        }
        if (rc4) {
            esp_http_client_set_header(client, "x-xiaomi-protocal-flag-cli", "PROTOCAL-HTTP2");
            esp_http_client_set_header(client, "MIOT-ENCRYPT-ALGORITHM", "ENCRYPT-RC4");
            esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        }
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        /* Img redirects bounce between app.processor and alibj.app.processor if
         * account cookies are replayed; signed query is enough (HA/ffmpeg). */
        if (cookie_hdr && cookie_hdr[0] && hop == 0) {
            esp_http_client_set_header(client, "Cookie", cookie_hdr);
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (extra.location[0] == '\0' &&
            (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            char *loc_hdr = NULL;
            if (esp_http_client_get_header(client, "Location", &loc_hdr) == ESP_OK && loc_hdr && loc_hdr[0]) {
                strncpy(extra.location, loc_hdr, sizeof(extra.location) - 1);
                extra.location[sizeof(extra.location) - 1] = '\0';
            }
        }
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            set_get_error("GET %s status=%d loc=%.80s", esp_err_to_name(err), status,
                          extra.location[0] ? extra.location : "-");
            ESP_LOGE(TAG, "cloud GET failed %s: %s (status=%d loc=%s)", cur_url, esp_err_to_name(err),
                     status, extra.location[0] ? extra.location : "-");
            result = err;
            break;
        }
        if (status >= 200 && status < 300) {
            s_last_get_error[0] = '\0';
            result = ESP_OK;
            break;
        }
        ESP_LOGI(TAG, "cloud GET status=%d hop=%d body_len=%u loc_len=%u magic=%02x%02x",
                 status, hop, (unsigned)(resp ? resp->len : 0),
                 (unsigned)strlen(extra.location),
                 (resp && resp->len > 0) ? (unsigned char)resp->body[0] : 0,
                 (resp && resp->len > 1) ? (unsigned char)resp->body[1] : 0);
        if ((status == 301 || status == 302 || status == 303) && resp && resp->len > 64) {
            /* Some CDNs attach the payload on 302; do not drop it to follow GSLB. */
            s_last_get_error[0] = '\0';
            result = ESP_OK;
            break;
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
            extra.location[0]) {
            size_t loc_len = strlen(extra.location);
            strncpy(cur_url, extra.location, 4095);
            cur_url[4095] = '\0';
            rewrite_http_to_https(cur_url, 4096);
            int kind = 0;
            if (strstr(cur_url, "alibj.app.processor")) {
                kind = 1;
            } else if (strstr(cur_url, "app.processor")) {
                kind = 2;
            }
            if (kind) {
                if ((seen_hosts & kind) && (seen_hosts & (kind == 1 ? 2 : 1))) {
                    set_get_error("camera CDN redirect loop");
                    result = ESP_FAIL;
                    break;
                }
                seen_hosts |= kind;
            }
            ESP_LOGI(TAG, "cloud GET %d redirect hop=%d rc4=%d loc_len=%u",
                     status, hop, (int)rc4, (unsigned)loc_len);
            if (resp) {
                free(resp->body);
                resp->body = NULL;
                resp->len = 0;
                resp->cap = 0;
                resp->max_len = max_len;
            }
            result = ESP_FAIL;
            continue;
        }
        set_get_error("HTTP %d loc=%.100s", status, extra.location[0] ? extra.location : "-");
        ESP_LOGW(TAG, "cloud GET HTTP %d from %s loc=%s", status, cur_url,
                 extra.location[0] ? extra.location : "-");
        result = ESP_FAIL;
        break;
    }
    if (result != ESP_OK && s_last_get_error[0] == '\0') {
        set_get_error("too many redirects last=%.900s", cur_url);
    }
    free(cur_url);
    return result;
}

esp_err_t miot_http_get_cloud_rc4(const char *url, const char *user_agent,
                                  const char *cookie_hdr, miot_http_buf_t *resp)
{
    return miot_http_get_cloud_ex(url, user_agent, cookie_hdr, resp, true);
}

esp_err_t miot_http_get_cloud(const char *url, const char *user_agent,
                              const char *cookie_hdr, miot_http_buf_t *resp)
{
    return miot_http_get_cloud_ex(url, user_agent, cookie_hdr, resp, false);
}

esp_err_t miot_http_request(const char *url, esp_http_client_method_t method,
                            const char *post_body, const char *user_agent,
                            char *cookie_jar, size_t cookie_jar_len,
                            miot_http_buf_t *resp,
                            char *set_cookie_out, size_t set_cookie_len)
{
    return miot_http_request_ex(url, method, post_body, user_agent, cookie_jar, cookie_jar_len,
                                resp, set_cookie_out, set_cookie_len, NULL, false);
}

esp_err_t miot_http_run_worker(miot_http_worker_fn_t fn, void *ctx)
{
    if (minihub_http_is_worker_task()) {
        return fn ? fn(ctx) : ESP_ERR_INVALID_ARG;
    }
    return minihub_http_run_worker(fn, ctx);
}

esp_err_t miot_http_run_worker_async(miot_http_worker_fn_t fn, void *ctx, miot_http_worker_done_cb_t cb,
                                     void *user_ctx)
{
    return minihub_http_run_worker_async(fn, ctx, cb, user_ctx);
}
