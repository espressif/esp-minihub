/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.net.udp — IPv4 UDP datagrams for LAN plugin protocols
 * host.crypto  — md5/sha1/sha256/hmacSha256/aes128Cbc (base64 I/O)
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"

static const char *TAG = "minihub_js_ext";

#define JS_UDP_MAX_FDS    4
#define JS_UDP_RECV_MAX   2048
#define JS_B64_IN_MAX     (8 * 1024)

static int s_udp_fds[JS_UDP_MAX_FDS];
static bool s_udp_inited;

static void udp_table_init(void)
{
    if (s_udp_inited) {
        return;
    }
    for (int i = 0; i < JS_UDP_MAX_FDS; i++) {
        s_udp_fds[i] = -1;
    }
    s_udp_inited = true;
}

static int udp_slot_alloc(int fd)
{
    udp_table_init();
    for (int i = 0; i < JS_UDP_MAX_FDS; i++) {
        if (s_udp_fds[i] < 0) {
            s_udp_fds[i] = fd;
            return i;
        }
    }
    return -1;
}

static int udp_slot_fd(int slot)
{
    udp_table_init();
    if (slot < 0 || slot >= JS_UDP_MAX_FDS) {
        return -1;
    }
    return s_udp_fds[slot];
}

static void udp_slot_clear(int slot)
{
    if (slot >= 0 && slot < JS_UDP_MAX_FDS) {
        s_udp_fds[slot] = -1;
    }
}

static uint8_t *b64_decode_alloc(const char *b64, size_t *out_len)
{
    if (!b64 || !out_len) {
        return NULL;
    }
    size_t blen = strlen(b64);
    if (blen == 0 || blen > JS_B64_IN_MAX * 2) {
        return NULL;
    }
    size_t cap = (blen * 3) / 4 + 4;
    uint8_t *buf = calloc(1, cap);
    if (!buf) {
        return NULL;
    }
    size_t olen = 0;
    if (mbedtls_base64_decode(buf, cap, &olen, (const unsigned char *)b64, blen) != 0) {
        free(buf);
        return NULL;
    }
    *out_len = olen;
    return buf;
}

static char *b64_encode_alloc(const uint8_t *data, size_t len)
{
    size_t cap = 4 * ((len + 2) / 3) + 4;
    char *out = calloc(1, cap);
    if (!out) {
        return NULL;
    }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, cap, &olen,
                              data ? data : (const uint8_t *)"", len) != 0) {
        free(out);
        return NULL;
    }
    out[olen] = '\0';
    return out;
}

static char *hex_encode_alloc(const uint8_t *data, size_t len)
{
    char *out = calloc(1, len * 2 + 1);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        sprintf(out + i * 2, "%02x", data[i]);
    }
    return out;
}

static const char *js_opt_str(JSContext *ctx, JSValueConst obj, const char *key)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    return s;
}

static JSValue js_udp_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    bool broadcast = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue b = JS_GetPropertyStr(ctx, argv[0], "broadcast");
        broadcast = JS_ToBool(ctx, b);
        JS_FreeValue(ctx, b);
    }

    int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return JS_ThrowInternalError(ctx, "udp socket failed");
    }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (broadcast) {
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(0);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(fd);
        return JS_ThrowInternalError(ctx, "udp bind failed");
    }
    int slot = udp_slot_alloc(fd);
    if (slot < 0) {
        close(fd);
        return JS_ThrowInternalError(ctx, "too many udp sockets");
    }
    return JS_NewInt32(ctx, slot);
}

static JSValue js_udp_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "close(fd)");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd >= 0) {
        close(fd);
        udp_slot_clear(slot);
    }
    return JS_UNDEFINED;
}

static JSValue js_udp_sendto(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "sendto(fd, {ip,port,data_b64})");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowTypeError(ctx, "invalid udp fd");
    }
    const char *ip = js_opt_str(ctx, argv[1], "ip");
    const char *data_b64 = js_opt_str(ctx, argv[1], "data_b64");
    JSValue port_v = JS_GetPropertyStr(ctx, argv[1], "port");
    int32_t port = 0;
    JS_ToInt32(ctx, &port, port_v);
    JS_FreeValue(ctx, port_v);
    if (!ip || !data_b64 || port <= 0 || port > 65535) {
        if (ip) {
            JS_FreeCString(ctx, ip);
        }
        if (data_b64) {
            JS_FreeCString(ctx, data_b64);
        }
        return JS_ThrowTypeError(ctx, "ip/port/data_b64 required");
    }
    size_t raw_len = 0;
    uint8_t *raw = b64_decode_alloc(data_b64, &raw_len);
    JS_FreeCString(ctx, data_b64);
    if (!raw) {
        JS_FreeCString(ctx, ip);
        return JS_ThrowTypeError(ctx, "bad data_b64");
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_aton(ip, &to.sin_addr) == 0) {
        free(raw);
        JS_FreeCString(ctx, ip);
        return JS_ThrowTypeError(ctx, "bad ip");
    }
    JS_FreeCString(ctx, ip);
    ssize_t n = sendto(fd, raw, raw_len, 0, (struct sockaddr *)&to, sizeof(to));
    free(raw);
    if (n < 0) {
        return JS_ThrowInternalError(ctx, "sendto failed");
    }
    return JS_NewInt32(ctx, (int)n);
}

static JSValue js_udp_recvfrom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "recvfrom(fd, {timeout_ms,max_len?})");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowTypeError(ctx, "invalid udp fd");
    }
    int32_t timeout_ms = 2000;
    int32_t max_len = JS_UDP_RECV_MAX;
    JSValue t = JS_GetPropertyStr(ctx, argv[1], "timeout_ms");
    if (JS_IsNumber(t)) {
        JS_ToInt32(ctx, &timeout_ms, t);
    }
    JS_FreeValue(ctx, t);
    JSValue m = JS_GetPropertyStr(ctx, argv[1], "max_len");
    if (JS_IsNumber(m)) {
        JS_ToInt32(ctx, &max_len, m);
        if (max_len <= 0 || max_len > JS_UDP_RECV_MAX) {
            max_len = JS_UDP_RECV_MAX;
        }
    }
    JS_FreeValue(ctx, m);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *buf = malloc((size_t)max_len);
    if (!buf) {
        return JS_ThrowOutOfMemory(ctx);
    }
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, buf, (size_t)max_len, 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        free(buf);
        return JS_NULL;
    }
    char *b64 = b64_encode_alloc(buf, (size_t)n);
    free(buf);
    if (!b64) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ip", JS_NewString(ctx, inet_ntoa(from.sin_addr)));
    JS_SetPropertyStr(ctx, obj, "port", JS_NewInt32(ctx, (int)ntohs(from.sin_port)));
    JS_SetPropertyStr(ctx, obj, "data_b64", JS_NewString(ctx, b64));
    free(b64);
    return obj;
}

static JSValue js_hash_common(JSContext *ctx, JSValueConst argv0, mbedtls_md_type_t type)
{
    const char *owned_cstr = NULL;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    bool free_raw = false;

    if (JS_IsString(argv0)) {
        owned_cstr = JS_ToCString(ctx, argv0);
        if (!owned_cstr) {
            return JS_EXCEPTION;
        }
        raw = (uint8_t *)owned_cstr;
        raw_len = strlen(owned_cstr);
    } else if (JS_IsObject(argv0)) {
        const char *b64 = js_opt_str(ctx, argv0, "data_b64");
        const char *text = js_opt_str(ctx, argv0, "data");
        if (b64) {
            raw = b64_decode_alloc(b64, &raw_len);
            free_raw = true;
            JS_FreeCString(ctx, b64);
            if (text) {
                JS_FreeCString(ctx, text);
            }
            if (!raw) {
                return JS_ThrowTypeError(ctx, "bad data_b64");
            }
        } else if (text) {
            owned_cstr = text;
            raw = (uint8_t *)text;
            raw_len = strlen(text);
        } else {
            return JS_ThrowTypeError(ctx, "data_b64 or data required");
        }
    } else {
        return JS_ThrowTypeError(ctx, "string or {data_b64|data} expected");
    }

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    uint8_t dig[64];
    size_t dlen = info ? mbedtls_md_get_size(info) : 0;
    int rc = info ? mbedtls_md(info, raw, raw_len, dig) : -1;
    if (free_raw) {
        free(raw);
    }
    if (owned_cstr) {
        JS_FreeCString(ctx, owned_cstr);
    }
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "hash failed");
    }
    char *hex = hex_encode_alloc(dig, dlen);
    if (!hex) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue v = JS_NewString(ctx, hex);
    free(hex);
    return v;
}

static JSValue js_md5(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "md5(input)");
    }
    return js_hash_common(ctx, argv[0], MBEDTLS_MD_MD5);
}

static JSValue js_sha1(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sha1(input)");
    }
    return js_hash_common(ctx, argv[0], MBEDTLS_MD_SHA1);
}

static JSValue js_sha256(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sha256(input)");
    }
    return js_hash_common(ctx, argv[0], MBEDTLS_MD_SHA256);
}

static JSValue js_hmac_sha256(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "hmacSha256({key_b64,data_b64})");
    }
    const char *kb = js_opt_str(ctx, argv[0], "key_b64");
    const char *db = js_opt_str(ctx, argv[0], "data_b64");
    if (!kb || !db) {
        if (kb) {
            JS_FreeCString(ctx, kb);
        }
        if (db) {
            JS_FreeCString(ctx, db);
        }
        return JS_ThrowTypeError(ctx, "key_b64 and data_b64 required");
    }
    size_t klen = 0, dlen = 0;
    uint8_t *key = b64_decode_alloc(kb, &klen);
    uint8_t *data = b64_decode_alloc(db, &dlen);
    JS_FreeCString(ctx, kb);
    JS_FreeCString(ctx, db);
    if (!key || !data) {
        free(key);
        free(data);
        return JS_ThrowTypeError(ctx, "bad base64");
    }
    uint8_t out[32];
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, data, dlen,
                             out);
    free(key);
    free(data);
    if (rc != 0) {
        return JS_ThrowInternalError(ctx, "hmac failed");
    }
    char *b64 = b64_encode_alloc(out, 32);
    if (!b64) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue v = JS_NewString(ctx, b64);
    free(b64);
    return v;
}

static JSValue js_aes128_cbc(JSContext *ctx, JSValueConst argv0, bool encrypt)
{
    if (!JS_IsObject(argv0)) {
        return JS_ThrowTypeError(ctx, "object required");
    }
    const char *kb = js_opt_str(ctx, argv0, "key_b64");
    const char *ib = js_opt_str(ctx, argv0, "iv_b64");
    const char *db = js_opt_str(ctx, argv0, "data_b64");
    if (!kb || !ib || !db) {
        if (kb) {
            JS_FreeCString(ctx, kb);
        }
        if (ib) {
            JS_FreeCString(ctx, ib);
        }
        if (db) {
            JS_FreeCString(ctx, db);
        }
        return JS_ThrowTypeError(ctx, "key_b64/iv_b64/data_b64 required");
    }
    size_t klen = 0, ilen = 0, dlen = 0;
    uint8_t *key = b64_decode_alloc(kb, &klen);
    uint8_t *iv = b64_decode_alloc(ib, &ilen);
    uint8_t *data = b64_decode_alloc(db, &dlen);
    JS_FreeCString(ctx, kb);
    JS_FreeCString(ctx, ib);
    JS_FreeCString(ctx, db);
    if (!key || !iv || !data || klen != 16 || ilen != 16) {
        free(key);
        free(iv);
        free(data);
        return JS_ThrowTypeError(ctx, "key/iv must be 16 bytes");
    }

    /* mbedtls 4: legacy cipher.h gone — use PSA AES-CBC PKCS7. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        free(key);
        free(iv);
        free(data);
        return JS_ThrowInternalError(ctx, "psa init failed");
    }

    size_t out_cap = dlen + 32;
    uint8_t *out = malloc(out_cap);
    if (!out) {
        free(key);
        free(iv);
        free(data);
        return JS_ThrowOutOfMemory(ctx);
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_PKCS7);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    psa_status_t status = psa_import_key(&attributes, key, 16, &key_id);
    psa_reset_key_attributes(&attributes);
    free(key);
    if (status != PSA_SUCCESS) {
        free(iv);
        free(data);
        free(out);
        return JS_ThrowInternalError(ctx, "psa import key failed");
    }

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t chunk = 0;
    size_t total = 0;
    if (encrypt) {
        status = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CBC_PKCS7);
    } else {
        if (dlen == 0 || (dlen % 16) != 0) {
            psa_destroy_key(key_id);
            free(iv);
            free(data);
            free(out);
            return JS_ThrowTypeError(ctx, "ciphertext must be 16-byte multiple");
        }
        status = psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CBC_PKCS7);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&op, iv, 16);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&op, data, dlen, out, out_cap, &chunk);
        total = chunk;
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&op, out + total, out_cap - total, &chunk);
        total += chunk;
    } else {
        psa_cipher_abort(&op);
    }
    psa_destroy_key(key_id);
    free(iv);
    free(data);
    if (status != PSA_SUCCESS) {
        free(out);
        ESP_LOGW(TAG, "aes cbc failed status=%d", (int)status);
        return JS_ThrowInternalError(ctx, "aes128 cbc failed");
    }
    char *b64 = b64_encode_alloc(out, total);
    free(out);
    if (!b64) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue v = JS_NewString(ctx, b64);
    free(b64);
    return v;
}

static JSValue js_aes_enc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "aes128CbcEncrypt(opts)");
    }
    return js_aes128_cbc(ctx, argv[0], true);
}

static JSValue js_aes_dec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "aes128CbcDecrypt(opts)");
    }
    return js_aes128_cbc(ctx, argv[0], false);
}

void minihub_js_attach_net_crypto(JSContext *ctx, JSValue host)
{
    JSValue net = JS_NewObject(ctx);
    JSValue udp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, udp, "open", JS_NewCFunction(ctx, js_udp_open, "open", 1));
    JS_SetPropertyStr(ctx, udp, "close", JS_NewCFunction(ctx, js_udp_close, "close", 1));
    JS_SetPropertyStr(ctx, udp, "sendto", JS_NewCFunction(ctx, js_udp_sendto, "sendto", 2));
    JS_SetPropertyStr(ctx, udp, "recvfrom", JS_NewCFunction(ctx, js_udp_recvfrom, "recvfrom", 2));
    JS_SetPropertyStr(ctx, net, "udp", udp);
    JS_SetPropertyStr(ctx, host, "net", net);

    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "md5", JS_NewCFunction(ctx, js_md5, "md5", 1));
    JS_SetPropertyStr(ctx, crypto, "sha1", JS_NewCFunction(ctx, js_sha1, "sha1", 1));
    JS_SetPropertyStr(ctx, crypto, "sha256", JS_NewCFunction(ctx, js_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "hmacSha256",
                      JS_NewCFunction(ctx, js_hmac_sha256, "hmacSha256", 1));
    JS_SetPropertyStr(ctx, crypto, "aes128CbcEncrypt",
                      JS_NewCFunction(ctx, js_aes_enc, "aes128CbcEncrypt", 1));
    JS_SetPropertyStr(ctx, crypto, "aes128CbcDecrypt",
                      JS_NewCFunction(ctx, js_aes_dec, "aes128CbcDecrypt", 1));
    JS_SetPropertyStr(ctx, host, "crypto", crypto);
}

#endif /* CONFIG_MINIHUB_JS_PLUGINS */
