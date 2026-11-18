/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.mqtt — per-plugin MQTT subscriptions and raw message delivery for L2
 * dynamic device discovery.
 *
 * The loopback esp-mqtt client (see minihub_mqtt_broker/mqtt_adapter.c) connects to the
 * Hub's own loopback listener. Every L2 plugin that uses host.mqtt is an
 * "owner" (0..MINIHUB_MQTT_OWNERS_MAX-1). A plugin subscribes through
 * `host.mqtt.client(id)` and drains only the messages that matched *its* own
 * filters — the adapter computes an owner bitmask per message, so one device
 * message is copied into each interested owner's ring instead of being grabbed
 * by whichever plugin polls first.
 *
 * Delivery model — push, not pull:
 *   - The adapter computes an owner bitmask per message and the C hook copies
 *     the bytes into each interested owner's bounded ring (no JSValue crosses
 *     the esp-mqtt task boundary — that stays forbidden).
 *   - A plugin registers a handler with `client.onMessage(cb)`. The hook, when
 *     it enqueues a message for an owner that has a handler, flips a pending
 *     flag and notifies js_worker. The worker loop then calls
 *     `minihub_js_mqtt_dispatch()`, which drains every ring with a handler and
 *     invokes the JS callback on the JS task — so from the plugin's view the
 *     message arrives as a callback, with no manual drain / host.adapter.poll
 *     needed for MQTT.
 *   - The Core event bus queue is shallow (32) and broadcast, so it is unsuitable
 *     for a discovery burst; the per-owner ring absorbs the burst instead.
 * `minihub_js_mqtt_reset()` frees the handlers, rings and broker subscriptions
 * on runtime unload, so a torn-down owner can never receive (or be referenced
 * by) a callback whose s_ctx is gone.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_MINIHUB_JS_MQTT && CONFIG_MINIHUB_MQTT_ADAPTER
#include "minihub_mqtt.h"
#define JS_MQTT_ENABLED 1
#define JS_MQTT_OWNERS   MINIHUB_MQTT_OWNERS_MAX
#else
#define JS_MQTT_ENABLED 0
/* Header unavailable in the disabled build; the arrays below stay unused. */
#define JS_MQTT_OWNERS   8
#endif

#define JS_MQTT_RING_MAX    24
#define JS_MQTT_TOPIC_MAX   128
#define JS_MQTT_PAYLOAD_MAX 2048
#define JS_MQTT_ID_MAX      31

typedef struct {
    char *topic;
    char *payload;
} js_mqtt_slot_t;

/* One bounded ring per owner. The adapter already de-duplicates the broker
 * subscription across owners, so this is cheap RAM: 8 * 24 * 2 pointers. */
static js_mqtt_slot_t s_rings[JS_MQTT_OWNERS][JS_MQTT_RING_MAX];
static size_t s_head[JS_MQTT_OWNERS];
static size_t s_tail[JS_MQTT_OWNERS];
static uint32_t s_dropped[JS_MQTT_OWNERS];
static bool s_owner_active[JS_MQTT_OWNERS];
static bool s_owner_has_cb[JS_MQTT_OWNERS];
static char s_owner_id[JS_MQTT_OWNERS][JS_MQTT_ID_MAX + 1];
/* The JS handler registered via client.onMessage(); one per owner. Replaced on
 * re-registration, freed on release/unload. Never invoked off the JS task. */
static JSValue s_owner_cb[JS_MQTT_OWNERS];
/* Set by the hook (esp-mqtt task) when a message lands for an owner that has a
 * handler; the worker loop reads and clears it, then dispatches. */
static volatile bool s_mqtt_pending;
/* js_worker task handle, captured at attach so the hook can wake it. */
static TaskHandle_t s_js_task;
static portMUX_TYPE s_ring_mux[JS_MQTT_OWNERS] = {
    [0 ... JS_MQTT_OWNERS - 1] = portMUX_INITIALIZER_UNLOCKED
};

static const char *TAG = "js_mqtt";

#if !JS_MQTT_ENABLED
static JSValue js_mqtt_disabled(JSContext *ctx)
{
    return JS_ThrowInternalError(ctx,
        "host.mqtt disabled (CONFIG_MINIHUB_JS_MQTT / CONFIG_MINIHUB_MQTT_ADAPTER)");
}
#endif

/* Default client id used by the top-level host.mqtt.subscribe/getMessages. */
#define JS_MQTT_DEFAULT_ID "*default*"

#if JS_MQTT_ENABLED
static void js_mqtt_ring_free_slot(js_mqtt_slot_t *slot)
{
    free(slot->topic);
    free(slot->payload);
    slot->topic = NULL;
    slot->payload = NULL;
}

/* Runs on the esp-mqtt event task. Copies one message into every owner whose
 * bit is set in owner_mask. Each owner gets its own malloc'd copy so a teardown
 * of one owner never frees bytes another still holds. Short, non-blocking, no
 * QuickJS calls. */
static void js_mqtt_hook(const char *topic, int topic_len,
                         const char *data, int data_len,
                         uint32_t owner_mask, void *user_data)
{
    (void)user_data;
    if (!topic || topic_len <= 0 || topic_len >= JS_MQTT_TOPIC_MAX ||
        data_len < 0 || data_len >= JS_MQTT_PAYLOAD_MAX) {
        return;
    }
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        if (((owner_mask >> o) & 1u) == 0) {
            continue;
        }
        if (!s_owner_active[o]) {
            continue; /* owner torn down mid-flight; ignore its share */
        }
        char *t = malloc((size_t)topic_len + 1);
        char *p = malloc((size_t)data_len + 1);
        if (!t || !p) {
            free(t);
            free(p);
            continue;
        }
        memcpy(t, topic, (size_t)topic_len);
        t[topic_len] = '\0';
        if (data_len > 0 && data) {
            memcpy(p, data, (size_t)data_len);
        }
        p[data_len] = '\0';

        portENTER_CRITICAL(&s_ring_mux[o]);
        size_t next = (s_head[o] + 1) % JS_MQTT_RING_MAX;
        if (next == s_tail[o]) {
            js_mqtt_ring_free_slot(&s_rings[o][s_tail[o]]);
            s_tail[o] = (s_tail[o] + 1) % JS_MQTT_RING_MAX;
            s_dropped[o]++;
        }
        s_rings[o][s_head[o]].topic = t;
        s_rings[o][s_head[o]].payload = p;
        s_head[o] = next;
        bool wake = s_owner_has_cb[o];
        portEXIT_CRITICAL(&s_ring_mux[o]);

        /* A plugin that registered onMessage should be woken so the worker can
         * dispatch this message as a callback. Without a handler the bytes just
         * buffer (and eventually drop) — never silently lost before a handler
         * is attached. */
        if (wake) {
            s_mqtt_pending = true;
            if (s_js_task) {
                xTaskNotifyGive(s_js_task);
            }
        }
    }
}
#endif

/* Forward declaration: defined later, but reset() needs it. */
static void js_mqtt_release_owner(JSContext *ctx, int owner);

/** Called from js_unload_job() before JS_FreeContext; releases all C state.
 * @p ctx is still valid at that point (the context is freed just after). */
void minihub_js_mqtt_reset(JSContext *ctx)
{
#if JS_MQTT_ENABLED
    minihub_mqtt_set_hook(NULL, NULL);
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        if (s_owner_active[o]) {
            js_mqtt_release_owner(ctx, o);
        } else if (JS_IsFunction(ctx, s_owner_cb[o])) {
            /* Defensive: free an orphaned handler even if the owner deactivated. */
            JS_FreeValue(ctx, s_owner_cb[o]);
            s_owner_cb[o] = JS_UNDEFINED;
        }
    }
    s_mqtt_pending = false;
    s_js_task = NULL;
#endif
}

#if JS_MQTT_ENABLED
/* Resolve an id string to a stable owner index, allocating one if needed.
 * Returns 0..JS_MQTT_OWNERS-1, or -1 if the pool is exhausted. */
static int js_mqtt_owner_for_id(const char *id)
{
    if (!id || !id[0]) {
        return -1;
    }
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        if (s_owner_active[o] && strcmp(s_owner_id[o], id) == 0) {
            return o;
        }
    }
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        if (!s_owner_active[o]) {
            s_owner_active[o] = true;
            s_owner_has_cb[o] = false;
            s_owner_cb[o] = JS_UNDEFINED;
            strncpy(s_owner_id[o], id, JS_MQTT_ID_MAX);
            s_owner_id[o][JS_MQTT_ID_MAX] = '\0';
            s_head[o] = 0;
            s_tail[o] = 0;
            s_dropped[o] = 0;
            return o;
        }
    }
    return -1;
}

static int js_mqtt_owner_from_this(JSContext *ctx, JSValueConst this_val)
{
    JSValue owner_val = JS_GetPropertyStr(ctx, this_val, "owner");
    int32_t owner = -1;
    if (JS_ToInt32(ctx, &owner, owner_val) != 0) {
        owner = -1;
    }
    JS_FreeValue(ctx, owner_val);
    if (owner < 0 || owner >= JS_MQTT_OWNERS || !s_owner_active[owner]) {
        return -1;
    }
    return (int)owner;
}
#endif

static JSValue js_mqtt_subscribe(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    int owner = js_mqtt_owner_from_this(ctx, this_val);
    if (owner < 0) {
        return JS_ThrowTypeError(ctx, "not a host.mqtt client");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "subscribe(filter, opts?) expects a string filter");
    }
    const char *filter = JS_ToCString(ctx, argv[0]);
    if (!filter || !filter[0]) {
        if (filter) {
            JS_FreeCString(ctx, filter);
        }
        return JS_ThrowTypeError(ctx, "filter must not be empty");
    }
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    int qos = js_opt_int(ctx, opts, "qos", 0);
    if (qos < 0 || qos > 2) {
        qos = 0;
    }
    esp_err_t err = minihub_mqtt_add_filter(filter, qos, owner);
    JS_FreeCString(ctx, filter);
    if (err == ESP_ERR_NO_MEM) {
        return JS_ThrowInternalError(ctx, "host.mqtt: filter table full (max %d owners, %d filters)",
                                      JS_MQTT_OWNERS, MINIHUB_MQTT_FILTERS_MAX);
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "host.mqtt.subscribe failed %s", esp_err_to_name(err));
    }
    minihub_mqtt_set_hook(js_mqtt_hook, NULL);
    ESP_LOGI(TAG, "[%s] subscribed (owner=%d, filters=%u)", s_owner_id[owner],
             owner, (unsigned)minihub_mqtt_filter_count());
    return JS_NewBool(ctx, true);
#endif
}

static JSValue js_mqtt_unsubscribe(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    int owner = js_mqtt_owner_from_this(ctx, this_val);
    if (owner < 0) {
        return JS_ThrowTypeError(ctx, "not a host.mqtt client");
    }
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "unsubscribe(filter) expects a string");
    }
    const char *filter = JS_ToCString(ctx, argv[0]);
    if (!filter) {
        return JS_ThrowTypeError(ctx, "filter required");
    }
    bool ok = minihub_mqtt_remove_filter(filter, owner) == ESP_OK;
    JS_FreeCString(ctx, filter);
    return JS_NewBool(ctx, ok);
#endif
}

static JSValue js_mqtt_on_message(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    int owner = js_mqtt_owner_from_this(ctx, this_val);
    if (owner < 0) {
        return JS_ThrowTypeError(ctx, "not a host.mqtt client");
    }
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "onMessage(cb) expects a function");
    }
    if (JS_IsFunction(ctx, s_owner_cb[owner])) {
        JS_FreeValue(ctx, s_owner_cb[owner]);
    }
    s_owner_cb[owner] = JS_DupValue(ctx, argv[0]);
    portENTER_CRITICAL(&s_ring_mux[owner]);
    s_owner_has_cb[owner] = true;
    portEXIT_CRITICAL(&s_ring_mux[owner]);
    ESP_LOGI(TAG, "[%s] onMessage registered (owner=%d)", s_owner_id[owner], owner);
    return JS_NewBool(ctx, true);
#endif
}

/* Called from the js_worker loop. Drains every owner that is still alive and has
 * a registered handler, invoking it with (topic, payload) — one callback per
 * queued message, on the JS task, so no JSValue crosses a task boundary and no
 * manual poll is needed. A callback that throws is caught; it does not stop the
 * remaining messages.
 *
 * Safety contract: if an owner is no longer active (its plugin was unloaded or
 * disabled) the message is dropped here and the callback is never invoked — even
 * if its broker filter lingered across a teardown race. This is the guaranteed
 * fallback even though unload also calls minihub_mqtt_clear_owner(). */
void minihub_js_mqtt_dispatch(JSContext *ctx)
{
#if JS_MQTT_ENABLED
    if (!ctx) {
        return;
    }
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        if (!s_owner_active[o] || !s_owner_has_cb[o] ||
            !JS_IsFunction(ctx, s_owner_cb[o])) {
            continue;
        }
        JSValue cb = s_owner_cb[o];
        for (;;) {
            char *topic = NULL;
            char *payload = NULL;
            portENTER_CRITICAL(&s_ring_mux[o]);
            if (s_tail[o] == s_head[o]) {
                portEXIT_CRITICAL(&s_ring_mux[o]);
                break;
            }
            topic = s_rings[o][s_tail[o]].topic;
            payload = s_rings[o][s_tail[o]].payload;
            s_rings[o][s_tail[o]].topic = NULL;
            s_rings[o][s_tail[o]].payload = NULL;
            s_tail[o] = (s_tail[o] + 1) % JS_MQTT_RING_MAX;
            portEXIT_CRITICAL(&s_ring_mux[o]);

            JSValue argv[2] = {
                JS_NewString(ctx, topic ? topic : ""),
                JS_NewString(ctx, payload ? payload : "")
            };
            JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, argv);
            JS_FreeValue(ctx, argv[0]);
            JS_FreeValue(ctx, argv[1]);
            if (JS_IsException(ret)) {
                /* Keep the handler; just surface the error and continue. */
                js_dump_exception(ctx);
            }
            JS_FreeValue(ctx, ret);
            free(topic);
            free(payload);
        }
    }
#endif
}

/* Read by the worker loop before dispatch; set directly by the hook. */
bool minihub_js_mqtt_has_pending(void)
{
    return s_mqtt_pending;
}

void minihub_js_mqtt_clear_pending(void)
{
    s_mqtt_pending = false;
}

/* Release one owner's resources without tearing down the whole runtime: drop its
 * broker filters (only unsubscribing filters no other owner still holds), free
 * its ring and JS handler. Used when a single module is unloaded/disabled. */
static void js_mqtt_release_owner(JSContext *ctx, int owner)
{
#if JS_MQTT_ENABLED
    if (owner < 0 || owner >= JS_MQTT_OWNERS || !s_owner_active[owner]) {
        return;
    }
    minihub_mqtt_clear_owner(owner);
    portENTER_CRITICAL(&s_ring_mux[owner]);
    while (s_tail[owner] != s_head[owner]) {
        js_mqtt_ring_free_slot(&s_rings[owner][s_tail[owner]]);
        s_tail[owner] = (s_tail[owner] + 1) % JS_MQTT_RING_MAX;
    }
    s_head[owner] = 0;
    s_tail[owner] = 0;
    s_dropped[owner] = 0;
    s_owner_active[owner] = false;
    s_owner_has_cb[owner] = false;
    s_owner_id[owner][0] = '\0';
    if (JS_IsFunction(ctx, s_owner_cb[owner])) {
        JS_FreeValue(ctx, s_owner_cb[owner]);
    }
    s_owner_cb[owner] = JS_UNDEFINED;
    portEXIT_CRITICAL(&s_ring_mux[owner]);
#endif
}

/* Release the owner bound to a module id (or "*default*"). No-op if not found. */
void minihub_js_mqtt_release_module(JSContext *ctx, const char *id)
{
#if JS_MQTT_ENABLED
    if (!id) {
        return;
    }
    for (int o = 0; o < JS_MQTT_OWNERS; o++) {
        bool match = false;
        portENTER_CRITICAL(&s_ring_mux[o]);
        match = s_owner_active[o] && strcmp(s_owner_id[o], id) == 0;
        portEXIT_CRITICAL(&s_ring_mux[o]);
        if (match) {
            js_mqtt_release_owner(ctx, o);
            return;
        }
    }
#endif
}

static JSValue js_mqtt_publish(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    if (argc < 2 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "publish(topic, payload, opts?) expects strings");
    }
    const char *topic = JS_ToCString(ctx, argv[0]);
    const char *payload = JS_ToCString(ctx, argv[1]);
    if (!topic || !payload) {
        if (topic) {
            JS_FreeCString(ctx, topic);
        }
        if (payload) {
            JS_FreeCString(ctx, payload);
        }
        return JS_ThrowTypeError(ctx, "topic and payload required");
    }
    JSValueConst opts = argc >= 3 ? argv[2] : JS_UNDEFINED;
    int qos = js_opt_int(ctx, opts, "qos", 0);
    int retain = js_opt_int(ctx, opts, "retain", 0);
    if (qos < 0 || qos > 2) {
        qos = 0;
    }
    esp_err_t err = minihub_mqtt_publish(topic, payload, qos, retain ? 1 : 0);
    JS_FreeCString(ctx, topic);
    JS_FreeCString(ctx, payload);
    return JS_NewBool(ctx, err == ESP_OK);
#endif
}

static JSValue js_mqtt_is_connected(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    (void)argc;
    (void)argv;
    return JS_NewBool(ctx, minihub_mqtt_is_connected());
#endif
}

static JSValue js_mqtt_stats(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    (void)argc;
    (void)argv;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "connected", JS_NewBool(ctx, minihub_mqtt_is_connected()));
    JS_SetPropertyStr(ctx, o, "filters",
                      JS_NewInt32(ctx, (int)minihub_mqtt_filter_count()));
    JS_SetPropertyStr(ctx, o, "capacity", JS_NewInt32(ctx, JS_MQTT_RING_MAX));
    JS_SetPropertyStr(ctx, o, "owners", JS_NewInt32(ctx, JS_MQTT_OWNERS));
    return o;
#endif
}

#if JS_MQTT_ENABLED
/* Create the JS object for one owner. Methods read `this.owner`. */
static JSValue js_mqtt_make_client(JSContext *ctx, int owner)
{
    JSValue client = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, client, "owner", JS_NewInt32(ctx, owner));
    JS_SetPropertyStr(ctx, client, "id", JS_NewString(ctx, s_owner_id[owner]));
    JS_SetPropertyStr(ctx, client, "subscribe",
                      JS_NewCFunction(ctx, js_mqtt_subscribe, "subscribe", 2));
    JS_SetPropertyStr(ctx, client, "unsubscribe",
                      JS_NewCFunction(ctx, js_mqtt_unsubscribe, "unsubscribe", 1));
    JS_SetPropertyStr(ctx, client, "onMessage",
                      JS_NewCFunction(ctx, js_mqtt_on_message, "onMessage", 1));
    JS_SetPropertyStr(ctx, client, "publish",
                      JS_NewCFunction(ctx, js_mqtt_publish, "publish", 3));
    return client;
}
#endif

static JSValue js_mqtt_client(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
#if !JS_MQTT_ENABLED
    (void)argc;
    (void)argv;
    return js_mqtt_disabled(ctx);
#else
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "client(id) expects a string id");
    }
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id || !id[0]) {
        if (id) {
            JS_FreeCString(ctx, id);
        }
        return JS_ThrowTypeError(ctx, "client id must not be empty");
    }
    int owner = js_mqtt_owner_for_id(id);
    JS_FreeCString(ctx, id);
    if (owner < 0) {
        return JS_ThrowInternalError(ctx, "host.mqtt: too many clients (max %d)",
                                      JS_MQTT_OWNERS);
    }
    return js_mqtt_make_client(ctx, owner);
#endif
}

void minihub_js_attach_mqtt(JSContext *ctx, JSValue host, const char *module_id)
{
    (void)module_id;
#if !JS_MQTT_ENABLED
    JSValue mqtt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mqtt, "subscribe", JS_NewCFunction(ctx, (JS_CFunction *)js_mqtt_disabled, "subscribe", 0));
    JS_SetPropertyStr(ctx, mqtt, "client", JS_NewCFunction(ctx, (JS_CFunction *)js_mqtt_disabled, "client", 0));
    JS_SetPropertyStr(ctx, host, "mqtt", mqtt);
    return;
#else
    /* The top-level methods are a thin convenience layer over a lazily created
     * "*default*" client, so single-plugin scripts keep working. Multi-plugin
     * discovery must use host.mqtt.client(id) for isolation. */
    s_js_task = xTaskGetCurrentTaskHandle(); /* attach runs on js_worker */
    int def = js_mqtt_owner_for_id(JS_MQTT_DEFAULT_ID);
    if (def < 0) {
        ESP_LOGW(TAG, "default mqtt client could not be allocated");
        def = 0;
    }
    JSValue mqtt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mqtt, "subscribe",
                      JS_NewCFunction(ctx, js_mqtt_subscribe, "subscribe", 2));
    JS_SetPropertyStr(ctx, mqtt, "unsubscribe",
                      JS_NewCFunction(ctx, js_mqtt_unsubscribe, "unsubscribe", 1));
    JS_SetPropertyStr(ctx, mqtt, "onMessage",
                      JS_NewCFunction(ctx, js_mqtt_on_message, "onMessage", 1));
    JS_SetPropertyStr(ctx, mqtt, "publish",
                      JS_NewCFunction(ctx, js_mqtt_publish, "publish", 3));
    JS_SetPropertyStr(ctx, mqtt, "isConnected",
                      JS_NewCFunction(ctx, js_mqtt_is_connected, "isConnected", 0));
    JS_SetPropertyStr(ctx, mqtt, "stats",
                      JS_NewCFunction(ctx, js_mqtt_stats, "stats", 0));
    JS_SetPropertyStr(ctx, mqtt, "client",
                      JS_NewCFunction(ctx, js_mqtt_client, "client", 1));
    /* Methods read this.owner; point it at the default client. */
    JS_SetPropertyStr(ctx, mqtt, "owner", JS_NewInt32(ctx, def));
    JS_SetPropertyStr(ctx, host, "mqtt", mqtt);
#endif
}

#endif /* CONFIG_MINIHUB_JS_PLUGINS */
