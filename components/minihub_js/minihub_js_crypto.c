/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.crypto — md5/sha1/sha256 (lowercase hex) and hmacSha256/aes128Cbc
 * (base64 I/O) for L2 plugins. Built on mbedTLS 4 (PSA AES-CBC PKCS7).
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include <stdio.h>

#include "esp_log.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"

static const char *TAG = "minihub_js_crypto";

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

void minihub_js_attach_crypto(JSContext *ctx, JSValue host)
{
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
