/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.mdns — one-shot PTR query for LAN service discovery. mDNS is initialised
 * lazily and idempotently, so a Matter/Thread responder already running is left
 * alone.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include "esp_log.h"

#if CONFIG_MINIHUB_JS_MDNS
#include "mdns.h"

static bool s_mdns_inited;
#endif

static JSValue js_mdns_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_JS_MDNS
    (void)argc;
    (void)argv;
    return JS_ThrowInternalError(ctx, "host.mdns disabled (CONFIG_MINIHUB_JS_MDNS)");
#else
    JSValueConst opts = argc >= 1 ? argv[0] : JS_UNDEFINED;
    char service[64] = "_http";
    char proto[16] = "_tcp";
    js_opt_str_buf(ctx, opts, "service", service, sizeof(service));
    js_opt_str_buf(ctx, opts, "proto", proto, sizeof(proto));
    int timeout_ms = js_opt_int(ctx, opts, "timeout_ms", 2000);
    int max_results = js_opt_int(ctx, opts, "max_results", 8);
    if (timeout_ms < 100) {
        timeout_ms = 100;
    }
    if (timeout_ms > 10000) {
        timeout_ms = 10000;
    }
    if (max_results < 1) {
        max_results = 1;
    }
    if (max_results > 16) {
        max_results = 16;
    }

    if (!s_mdns_inited) {
        /* mdns_init() is idempotent; Matter may already own the responder. */
        esp_err_t err = mdns_init();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_mdns_inited = true;
        } else {
            return JS_ThrowInternalError(ctx, "host.mdns: init failed (%d)", (int)err);
        }
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(service, proto, (uint32_t)timeout_ms,
                                   (size_t)max_results, &results);
    JSValue out = JS_NewArray(ctx);
    if (err != ESP_OK) {
        return out;  /* discovery is best-effort: no answers -> empty array */
    }

    uint32_t n = 0;
    for (mdns_result_t *r = results; r && (int)n < max_results; r = r->next) {
        JSValue o = JS_NewObject(ctx);
        if (r->instance_name) {
            JS_SetPropertyStr(ctx, o, "instance", JS_NewString(ctx, r->instance_name));
        }
        if (r->service_type) {
            JS_SetPropertyStr(ctx, o, "service", JS_NewString(ctx, r->service_type));
        }
        if (r->hostname) {
            JS_SetPropertyStr(ctx, o, "hostname", JS_NewString(ctx, r->hostname));
        }
        JS_SetPropertyStr(ctx, o, "port", JS_NewInt32(ctx, r->port));
        if (r->addr) {
            uint32_t v = r->addr->addr.u_addr.ip4.addr;
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                     (unsigned)(v & 0xffu), (unsigned)((v >> 8) & 0xffu),
                     (unsigned)((v >> 16) & 0xffu), (unsigned)((v >> 24) & 0xffu));
            JS_SetPropertyStr(ctx, o, "ip", JS_NewString(ctx, ip));
        }
        JSValue txt = JS_NewObject(ctx);
        for (size_t i = 0; i < r->txt_count; i++) {
            if (r->txt[i].key && r->txt[i].value) {
                JS_SetPropertyStr(ctx, txt, r->txt[i].key,
                                  JS_NewString(ctx, r->txt[i].value));
            }
        }
        JS_SetPropertyStr(ctx, o, "txt", txt);
        JS_SetPropertyUint32(ctx, out, n++, o);
    }
    mdns_query_results_free(results);
    return out;
#endif
}

void minihub_js_attach_mdns(JSContext *ctx, JSValue host)
{
    JSValue mdns = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mdns, "resolve",
                      JS_NewCFunction(ctx, js_mdns_resolve, "resolve", 1));
    JS_SetPropertyStr(ctx, host, "mdns", mdns);
}

#endif /* CONFIG_MINIHUB_JS_PLUGINS */
