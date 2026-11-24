/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.ble — one shared, reference-counted passive advertisement scanner for
 * all BLE vendor plugins. Fourteen plugins all need advertisements; starting
 * and stopping the single NimBLE scanner fourteen times would reset the
 * accumulator on every setup() and starve the others, so the runtime owns one
 * continuous scan and hands out filtered snapshots.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include "esp_log.h"

#if CONFIG_MINIHUB_BLE_SDK
#include "minihub_ble_gatt.h"
#endif

#define JS_BLE_MAX_REFS 16

#if CONFIG_MINIHUB_BLE_SDK
static int s_ble_refs;
static minihub_ble_gatt_adv_t s_ble_ads[MINIHUB_BLE_GATT_ADV_MAX];
#endif

#if !CONFIG_MINIHUB_BLE_SDK
static JSValue js_ble_disabled(JSContext *ctx)
{
    return JS_ThrowInternalError(ctx, "host.ble disabled (needs CONFIG_MINIHUB_BLE_MESH_SDK)");
}
#endif

#if CONFIG_MINIHUB_BLE_SDK
static JSValue js_bytes_array(JSContext *ctx, const uint8_t *data, size_t len)
{
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < len; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewInt32(ctx, data[i]));
    }
    return arr;
}
#endif

static JSValue js_ble_ensure_scan(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#if !CONFIG_MINIHUB_BLE_SDK
    return js_ble_disabled(ctx);
#else
    if (s_ble_refs >= JS_BLE_MAX_REFS) {
        return JS_ThrowInternalError(ctx, "host.ble: too many scan users");
    }
    if (s_ble_refs == 0) {
        if (!minihub_ble_gatt_ready()) {
            esp_err_t err = minihub_ble_gatt_init();
            if (err != ESP_OK) {
                return JS_ThrowInternalError(ctx, "host.ble: NimBLE init failed (%d)", (int)err);
            }
        }
        /* duration 0 = scan until stopped; filter 0 = keep every advertisement. */
        esp_err_t err = minihub_ble_gatt_scan_start(0, 0);
        if (err != ESP_OK) {
            return JS_ThrowInternalError(ctx, "host.ble: scan_start failed (%d)", (int)err);
        }
    }
    s_ble_refs++;
    return JS_NewInt32(ctx, s_ble_refs);
#endif
}

static JSValue js_ble_release_scan(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
#if !CONFIG_MINIHUB_BLE_SDK
    return js_ble_disabled(ctx);
#else
    if (s_ble_refs > 0 && --s_ble_refs == 0) {
        (void)minihub_ble_gatt_scan_stop();
    }
    return JS_NewInt32(ctx, s_ble_refs);
#endif
}

static JSValue js_ble_get_ads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_BLE_SDK
    (void)argc;
    (void)argv;
    return js_ble_disabled(ctx);
#else
    JSValueConst opts = argc >= 1 ? argv[0] : JS_UNDEFINED;
    int want_company = js_opt_int(ctx, opts, "company", -1);
    int want_uuid = js_opt_int(ctx, opts, "uuid16", -1);
    int cap = js_opt_int(ctx, opts, "max", MINIHUB_BLE_GATT_ADV_MAX);
    if (cap < 0) {
        cap = 0;
    }
    if (cap > MINIHUB_BLE_GATT_ADV_MAX) {
        cap = MINIHUB_BLE_GATT_ADV_MAX;
    }

    size_t count = 0;
    if (minihub_ble_gatt_scan_get(s_ble_ads, MINIHUB_BLE_GATT_ADV_MAX, &count) != ESP_OK) {
        count = 0;
    }

    JSValue out = JS_NewArray(ctx);
    uint32_t n = 0;
    for (size_t i = 0; i < count && (int)n < cap; i++) {
        const minihub_ble_gatt_adv_t *a = &s_ble_ads[i];
        bool company_ok = want_company < 0 ||
                          (a->has_mfg_data && a->mfg_company == (uint16_t)want_company);
        bool uuid_ok = want_uuid < 0 ||
                       (a->has_uuid16 && a->uuid16 == (uint16_t)want_uuid) ||
                       (a->has_svc_data && a->svc_data_uuid16 == (uint16_t)want_uuid);
        if (!company_ok || !uuid_ok) {
            continue;
        }

        JSValue o = JS_NewObject(ctx);
        char addr[13];
        snprintf(addr, sizeof(addr), "%02x%02x%02x%02x%02x%02x",
                 a->addr[0], a->addr[1], a->addr[2],
                 a->addr[3], a->addr[4], a->addr[5]);
        JS_SetPropertyStr(ctx, o, "addr", JS_NewString(ctx, addr));
        JS_SetPropertyStr(ctx, o, "addrType", JS_NewInt32(ctx, a->addr_type));
        JS_SetPropertyStr(ctx, o, "rssi", JS_NewInt32(ctx, a->rssi));
        JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, a->name));
        if (a->has_uuid16) {
            JS_SetPropertyStr(ctx, o, "uuid16", JS_NewInt32(ctx, a->uuid16));
        }
        if (a->has_mfg_data) {
            JS_SetPropertyStr(ctx, o, "mfgCompany", JS_NewInt32(ctx, a->mfg_company));
            /* Includes the 2 little-endian company bytes, so JS parsers keep C's +2 offsets. */
            JS_SetPropertyStr(ctx, o, "mfgData",
                              js_bytes_array(ctx, a->mfg_data, a->mfg_data_len));
        }
        if (a->has_svc_data) {
            JS_SetPropertyStr(ctx, o, "svcUuid16", JS_NewInt32(ctx, a->svc_data_uuid16));
            /* Includes the 2 little-endian UUID16 bytes. */
            JS_SetPropertyStr(ctx, o, "svcData",
                              js_bytes_array(ctx, a->svc_data, a->svc_data_len));
        }
        JS_SetPropertyUint32(ctx, out, n++, o);
    }
    return out;
#endif
}

void minihub_js_attach_ble(JSContext *ctx, JSValue host)
{
    JSValue ble = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ble, "ensureScan",
                      JS_NewCFunction(ctx, js_ble_ensure_scan, "ensureScan", 1));
    JS_SetPropertyStr(ctx, ble, "releaseScan",
                      JS_NewCFunction(ctx, js_ble_release_scan, "releaseScan", 0));
    JS_SetPropertyStr(ctx, ble, "getAds",
                      JS_NewCFunction(ctx, js_ble_get_ads, "getAds", 1));
    JS_SetPropertyStr(ctx, host, "ble", ble);
}

#endif /* CONFIG_MINIHUB_JS_PLUGINS */
