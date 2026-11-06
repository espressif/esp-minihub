/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Extra Host bindings for L2 plugins, one translation unit per namespace:
 *   minihub_js_crypto.c -> host.crypto
 *   minihub_js_net.c     -> host.net (udp + tcp)
 *   minihub_js_ble.c     -> host.ble  (shared passive scan)
 *   minihub_js_mdns.c    -> host.mdns (service discovery)
 *   minihub_js_mqtt.c    -> host.mqtt (wildcard subscribe / raw messages)
 *
 * This header is the shared surface: it declares the attach functions and
 * provides the small helpers (option parsing, base64/hex) as static inline so
 * every namespace TU gets them without a cross-file symbol dependency.
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "mbedtls/base64.h"

/** @brief Attach host.crypto (md5/sha1/sha256/hmacSha256/aes128Cbc). */
void minihub_js_attach_crypto(JSContext *ctx, JSValue host);

/** @brief Attach host.net (udp + tcp). */
void minihub_js_attach_net(JSContext *ctx, JSValue host);

/** @brief Attach host.ble (shared passive advertisement scanner). */
void minihub_js_attach_ble(JSContext *ctx, JSValue host);

/** @brief Attach host.mdns (one-shot service discovery). */
void minihub_js_attach_mdns(JSContext *ctx, JSValue host);

/** @brief Attach host.mqtt (wildcard subscribe / publish / raw messages).
 *
 * @p module_id names the calling plugin: it gets one owner slot, so its
 * subscriptions and its read position in the shared message ring are its own
 * and cannot be consumed by another plugin.
 */
void minihub_js_attach_mqtt(JSContext *ctx, JSValue host, const char *module_id);

/**
 * @brief Drop host.mqtt C state (hook, filters, queued messages) before the
 * QuickJS context is freed. Called from js_unload_job(); @p ctx is still valid.
 */
void minihub_js_mqtt_reset(JSContext *ctx);

/** @brief Release the host.mqtt owner bound to a module id (or "*default*").
 * Unbooks its broker filters, ring and JS handler. Used when a single module is
 * unloaded/disabled. Safe when disabled. */
void minihub_js_mqtt_release_module(JSContext *ctx, const char *id);

/** @brief If a host.mqtt message is pending, the worker loop calls this to
 * dispatch queued messages to their JS handlers on the worker task. */
void minihub_js_mqtt_dispatch(JSContext *ctx);

/** @brief True when the hook has queued a message for a handler since the last
 * dispatch; the worker loop checks this to decide whether to call dispatch(). */
bool minihub_js_mqtt_has_pending(void);

/** @brief Clear the pending flag; called by the worker loop before dispatch. */
void minihub_js_mqtt_clear_pending(void);

/** @brief Print the current pending QuickJS exception to the log (no throw). */
void js_dump_exception(JSContext *ctx);

/* --- shared option parsing helpers ------------------------------------- */

/** Read an integer field; returns @p dflt when missing / not a number. */
static inline int js_opt_int(JSContext *ctx, JSValueConst obj, const char *key, int dflt)
{
    if (!JS_IsObject(obj)) {
        return dflt;
    }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    int32_t out = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToInt32(ctx, &out, v) != 0) {
        out = dflt;
    }
    JS_FreeValue(ctx, v);
    return (int)out;
}

/** Read a string field; caller must JS_FreeCString() the result (NULL if absent). */
static inline const char *js_opt_str(JSContext *ctx, JSValueConst obj, const char *key)
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

/** Read a string field into @p out (bounded by @p cap); no allocation. */
static inline void js_opt_str_buf(JSContext *ctx, JSValueConst obj, const char *key,
                                 char *out, size_t cap)
{
    if (!JS_IsObject(obj) || cap == 0) {
        return;
    }
    JSValue v = JS_GetPropertyStr(ctx, obj, key);
    if (!JS_IsUndefined(v) && !JS_IsNull(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s) {
            snprintf(out, cap, "%s", s);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, v);
}

/* --- shared base64 / hex helpers --------------------------------------- */

#define JS_B64_IN_MAX (8 * 1024)

static inline uint8_t *b64_decode_alloc(const char *b64, size_t *out_len)
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

static inline char *b64_encode_alloc(const uint8_t *data, size_t len)
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

static inline char *hex_encode_alloc(const uint8_t *data, size_t len)
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
#endif
