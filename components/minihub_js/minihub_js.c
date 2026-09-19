/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * L2 JS plugins: QuickJS-NG + Host API v0 (host.log / point / command / http / config).
 * Also runs main.js for runtime:hybrid packages (after native load).
 *
 * Host API highlights for Devices-page demos:
 *   host.point.register({ ..., props | params })
 *   host.point.set(id, value, props?)
 *   host.point.mergeProps(id, patch, value?)
 *   host.point.mergeParams(id, params, value?)
 *   host.command.on(adapter, fn)  — route Core commands into JS
 *   host.command.invoke({ point_id, cmd, params? })
 *   host.event.subscribe(type, fn) — point.updated / point.registered / …
 */

#include "minihub_js.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_paths.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_js";

#if CONFIG_MINIHUB_JS_PLUGINS

#include "quickjs.h"

#define MINIHUB_JS_HTTP_BODY_MAX 16384
#define MINIHUB_JS_SCRIPT_MAX    (256 * 1024)
#define MINIHUB_JS_CMD_HANDLERS_MAX 8
#define MINIHUB_JS_EVENT_SUBS_MAX   8

static JSRuntime *s_rt;
static JSContext *s_ctx;

typedef struct {
    char adapter[MINIHUB_ADAPTER_MAX];
    JSValue fn;
} js_cmd_slot_t;

static js_cmd_slot_t s_js_cmds[MINIHUB_JS_CMD_HANDLERS_MAX];
static size_t s_js_cmd_count;

typedef struct {
    char event_type[MINIHUB_EVENT_MAX];
    JSValue fn;
    bool active;
} js_event_slot_t;

static js_event_slot_t s_js_events[MINIHUB_JS_EVENT_SUBS_MAX];
static size_t s_js_event_count;
static bool s_js_event_bus_hooked;

static char *read_file_alloc(const char *path, size_t max_len, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n <= 0 || (size_t)n > max_len) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

static JSValue js_log_level(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int level)
{
    (void)this_val;
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    const char *msg = JS_ToCString(ctx, argv[0]);
    if (!msg) {
        return JS_EXCEPTION;
    }
    if (level == 0) {
        ESP_LOGI(TAG, "%s", msg);
    } else if (level == 1) {
        ESP_LOGW(TAG, "%s", msg);
    } else {
        ESP_LOGE(TAG, "%s", msg);
    }
    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_log_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return js_log_level(ctx, this_val, argc, argv, 0);
}
static JSValue js_log_warn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return js_log_level(ctx, this_val, argc, argv, 1);
}
static JSValue js_log_error(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return js_log_level(ctx, this_val, argc, argv, 2);
}

static const char *js_get_str_prop(JSContext *ctx, JSValueConst obj, const char *key)
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

static cJSON *js_value_to_cjson(JSContext *ctx, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) {
        return NULL;
    }
    JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        return NULL;
    }
    const char *s = JS_ToCString(ctx, json);
    JS_FreeValue(ctx, json);
    if (!s) {
        return NULL;
    }
    cJSON *cj = cJSON_Parse(s);
    JS_FreeCString(ctx, s);
    return cj;
}

static JSValue cjson_to_js(JSContext *ctx, const cJSON *cj)
{
    if (!cj) {
        return JS_NULL;
    }
    char *printed = cJSON_PrintUnformatted(cj);
    if (!printed) {
        return JS_NULL;
    }
    JSValue v = JS_ParseJSON(ctx, printed, strlen(printed), "<json>");
    free(printed);
    return v;
}

static void dump_exception(JSContext *ctx);
static void js_event_dispatch(const minihub_event_t *event, void *user_data);

static void clear_js_cmd_handlers(JSContext *ctx)
{
    for (size_t i = 0; i < s_js_cmd_count; i++) {
        if (ctx) {
            JS_FreeValue(ctx, s_js_cmds[i].fn);
        }
        s_js_cmds[i].fn = JS_UNDEFINED;
        s_js_cmds[i].adapter[0] = '\0';
    }
    s_js_cmd_count = 0;
}

static void clear_js_event_handlers(JSContext *ctx)
{
    if (s_js_event_bus_hooked) {
        (void)minihub_event_bus_unsubscribe("*", js_event_dispatch);
        s_js_event_bus_hooked = false;
    }
    for (size_t i = 0; i < s_js_event_count; i++) {
        if (ctx) {
            JS_FreeValue(ctx, s_js_events[i].fn);
        }
        s_js_events[i].fn = JS_UNDEFINED;
        s_js_events[i].event_type[0] = '\0';
        s_js_events[i].active = false;
    }
    s_js_event_count = 0;
}

static void js_event_dispatch(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !s_ctx) {
        return;
    }
    for (size_t i = 0; i < s_js_event_count; i++) {
        if (!s_js_events[i].active) {
            continue;
        }
        if (strcmp(s_js_events[i].event_type, event->event_type) != 0 &&
            strcmp(s_js_events[i].event_type, "*") != 0) {
            continue;
        }
        if (!JS_IsFunction(s_ctx, s_js_events[i].fn)) {
            continue;
        }
        JSValue arg = JS_NewObject(s_ctx);
        JS_SetPropertyStr(s_ctx, arg, "event_type", JS_NewString(s_ctx, event->event_type));
        JS_SetPropertyStr(s_ctx, arg, "timestamp_ms", JS_NewInt64(s_ctx, event->timestamp_ms));
        if (strcmp(event->event_type, "point.updated") == 0) {
            minihub_point_t *pt = minihub_point_get_by_index(event->point_index);
            if (pt) {
                JS_SetPropertyStr(s_ctx, arg, "point_id", JS_NewString(s_ctx, pt->point_id));
                JS_SetPropertyStr(s_ctx, arg, "value", JS_NewString(s_ctx, pt->value));
                if (pt->props) {
                    JS_SetPropertyStr(s_ctx, arg, "props", cjson_to_js(s_ctx, pt->props));
                }
            }
            JS_SetPropertyStr(s_ctx, arg, "old_value", JS_NewString(s_ctx, event->old_value));
            JS_SetPropertyStr(s_ctx, arg, "new_value", JS_NewString(s_ctx, event->new_value));
        }
        if (event->data) {
            JS_SetPropertyStr(s_ctx, arg, "data", cjson_to_js(s_ctx, event->data));
        }
        JSValue ret = JS_Call(s_ctx, s_js_events[i].fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(s_ctx, arg);
        if (JS_IsException(ret)) {
            dump_exception(s_ctx);
        }
        JS_FreeValue(s_ctx, ret);
    }
}

static JSValue *find_js_cmd_fn(const char *adapter)
{
    if (!adapter || !adapter[0]) {
        return NULL;
    }
    for (size_t i = 0; i < s_js_cmd_count; i++) {
        if (strcmp(s_js_cmds[i].adapter, adapter) == 0) {
            return &s_js_cmds[i].fn;
        }
    }
    return NULL;
}

static esp_err_t js_command_dispatch(const minihub_command_t *cmd)
{
    if (!cmd || !s_ctx) {
        return ESP_ERR_INVALID_STATE;
    }

    minihub_point_t *pt = minihub_point_get(cmd->point_id);
    if (!pt) {
        return ESP_ERR_NOT_FOUND;
    }

    JSValue *fnp = find_js_cmd_fn(pt->adapter);
    if (!fnp || !JS_IsFunction(s_ctx, *fnp)) {
        return ESP_ERR_NOT_FOUND;
    }

    JSValue arg = JS_NewObject(s_ctx);
    JS_SetPropertyStr(s_ctx, arg, "point_id", JS_NewString(s_ctx, cmd->point_id));
    JS_SetPropertyStr(s_ctx, arg, "cmd", JS_NewString(s_ctx, cmd->cmd));
    if (cmd->params) {
        JS_SetPropertyStr(s_ctx, arg, "params", cjson_to_js(s_ctx, cmd->params));
    } else {
        JS_SetPropertyStr(s_ctx, arg, "params", JS_NULL);
    }

    JSValue ret = JS_Call(s_ctx, *fnp, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(s_ctx, arg);
    if (JS_IsException(ret)) {
        dump_exception(s_ctx);
        JS_FreeValue(s_ctx, ret);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    if (JS_IsBool(ret) && !JS_ToBool(s_ctx, ret)) {
        err = ESP_FAIL;
    }
    JS_FreeValue(s_ctx, ret);
    return err;
}

static JSValue js_point_register(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "register expects object");
    }
    minihub_point_t pt = {0};
    const char *pid = js_get_str_prop(ctx, argv[0], "point_id");
    const char *type = js_get_str_prop(ctx, argv[0], "type");
    const char *adapter = js_get_str_prop(ctx, argv[0], "adapter");
    const char *value = js_get_str_prop(ctx, argv[0], "value");
    if (!pid) {
        return JS_ThrowTypeError(ctx, "point_id required");
    }
    strncpy(pt.point_id, pid, sizeof(pt.point_id) - 1);
    if (type) {
        strncpy(pt.type, type, sizeof(pt.type) - 1);
    } else {
        strncpy(pt.type, "sensor", sizeof(pt.type) - 1);
    }
    if (adapter) {
        strncpy(pt.adapter, adapter, sizeof(pt.adapter) - 1);
    } else {
        strncpy(pt.adapter, "js", sizeof(pt.adapter) - 1);
    }
    if (value) {
        strncpy(pt.value, value, sizeof(pt.value) - 1);
    }
    JS_FreeCString(ctx, pid);
    if (type) {
        JS_FreeCString(ctx, type);
    }
    if (adapter) {
        JS_FreeCString(ctx, adapter);
    }
    if (value) {
        JS_FreeCString(ctx, value);
    }

    JSValue online_v = JS_GetPropertyStr(ctx, argv[0], "online");
    pt.online = JS_ToBool(ctx, online_v);
    JS_FreeValue(ctx, online_v);

    /* props: full props object; or params: shorthand → { params: <obj> } */
    JSValue props_v = JS_GetPropertyStr(ctx, argv[0], "props");
    JSValue params_v = JS_GetPropertyStr(ctx, argv[0], "params");
    if (JS_IsObject(props_v)) {
        pt.props = js_value_to_cjson(ctx, props_v);
    } else if (JS_IsObject(params_v)) {
        cJSON *params = js_value_to_cjson(ctx, params_v);
        if (params) {
            pt.props = cJSON_CreateObject();
            if (pt.props) {
                cJSON_AddItemToObject(pt.props, "params", params);
            } else {
                cJSON_Delete(params);
            }
        }
    }
    JS_FreeValue(ctx, props_v);
    JS_FreeValue(ctx, params_v);

    esp_err_t err = minihub_point_register(&pt);
    if (pt.props) {
        cJSON_Delete(pt.props);
        pt.props = NULL;
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_register failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_point_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_NULL;
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    if (!pid) {
        return JS_EXCEPTION;
    }
    minihub_point_t *p = minihub_point_get(pid);
    JS_FreeCString(ctx, pid);
    if (!p) {
        return JS_NULL;
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "point_id", JS_NewString(ctx, p->point_id));
    JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, p->type));
    JS_SetPropertyStr(ctx, obj, "value", JS_NewString(ctx, p->value));
    JS_SetPropertyStr(ctx, obj, "adapter", JS_NewString(ctx, p->adapter));
    JS_SetPropertyStr(ctx, obj, "online", JS_NewBool(ctx, p->online));
    if (p->props) {
        JS_SetPropertyStr(ctx, obj, "props", cjson_to_js(ctx, p->props));
    } else {
        JS_SetPropertyStr(ctx, obj, "props", JS_NULL);
    }
    return obj;
}

static JSValue js_command_invoke(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "invoke expects {point_id, cmd, params?}");
    }
    minihub_command_t cmd = {0};
    const char *pid = js_get_str_prop(ctx, argv[0], "point_id");
    const char *cname = js_get_str_prop(ctx, argv[0], "cmd");
    if (!pid || !cname) {
        if (pid) {
            JS_FreeCString(ctx, pid);
        }
        if (cname) {
            JS_FreeCString(ctx, cname);
        }
        return JS_ThrowTypeError(ctx, "point_id and cmd required");
    }
    strncpy(cmd.point_id, pid, sizeof(cmd.point_id) - 1);
    strncpy(cmd.cmd, cname, sizeof(cmd.cmd) - 1);
    JS_FreeCString(ctx, pid);
    JS_FreeCString(ctx, cname);

    JSValue params_v = JS_GetPropertyStr(ctx, argv[0], "params");
    if (JS_IsObject(params_v)) {
        cmd.params = js_value_to_cjson(ctx, params_v);
    }
    JS_FreeValue(ctx, params_v);

    esp_err_t err = minihub_command_invoke(&cmd);
    if (cmd.params) {
        cJSON_Delete(cmd.params);
        cmd.params = NULL;
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "command_invoke failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_command_on(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "on(adapter, handler)");
    }
    const char *adapter = JS_ToCString(ctx, argv[0]);
    if (!adapter || !adapter[0]) {
        if (adapter) {
            JS_FreeCString(ctx, adapter);
        }
        return JS_ThrowTypeError(ctx, "adapter required");
    }
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, adapter);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    js_cmd_slot_t *slot = NULL;
    for (size_t i = 0; i < s_js_cmd_count; i++) {
        if (strcmp(s_js_cmds[i].adapter, adapter) == 0) {
            slot = &s_js_cmds[i];
            break;
        }
    }
    if (!slot) {
        if (s_js_cmd_count >= MINIHUB_JS_CMD_HANDLERS_MAX) {
            JS_FreeCString(ctx, adapter);
            return JS_ThrowInternalError(ctx, "too many JS command handlers");
        }
        slot = &s_js_cmds[s_js_cmd_count++];
        strncpy(slot->adapter, adapter, sizeof(slot->adapter) - 1);
        slot->adapter[sizeof(slot->adapter) - 1] = '\0';
        slot->fn = JS_UNDEFINED;
    }

    JS_FreeValue(ctx, slot->fn);
    slot->fn = JS_DupValue(ctx, argv[1]);
    JS_FreeCString(ctx, adapter);

    esp_err_t err = minihub_command_register_adapter(slot->adapter, js_command_dispatch);
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "command_register_adapter failed %s",
                                     esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_point_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "set(id, value, props?)");
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!pid || !val) {
        if (pid) {
            JS_FreeCString(ctx, pid);
        }
        if (val) {
            JS_FreeCString(ctx, val);
        }
        return JS_EXCEPTION;
    }

    cJSON *props = NULL;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        props = js_value_to_cjson(ctx, argv[2]);
    }

    esp_err_t err = minihub_point_set(pid, val, props);
    if (props) {
        cJSON_Delete(props);
    }
    JS_FreeCString(ctx, pid);
    JS_FreeCString(ctx, val);
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_set failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_point_merge_props(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "mergeProps(id, propsPatch, value?)");
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    if (!pid) {
        return JS_EXCEPTION;
    }
    cJSON *patch = js_value_to_cjson(ctx, argv[1]);
    if (!patch) {
        JS_FreeCString(ctx, pid);
        return JS_ThrowTypeError(ctx, "propsPatch must be object");
    }
    const char *val = NULL;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        val = JS_ToCString(ctx, argv[2]);
        if (!val) {
            cJSON_Delete(patch);
            JS_FreeCString(ctx, pid);
            return JS_EXCEPTION;
        }
    }
    esp_err_t err = minihub_point_merge_props(pid, val, patch);
    cJSON_Delete(patch);
    JS_FreeCString(ctx, pid);
    if (val) {
        JS_FreeCString(ctx, val);
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_merge_props failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_point_merge_params(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "mergeParams(id, params, value?)");
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    if (!pid) {
        return JS_EXCEPTION;
    }
    cJSON *params = js_value_to_cjson(ctx, argv[1]);
    if (!params) {
        JS_FreeCString(ctx, pid);
        return JS_ThrowTypeError(ctx, "params must be object");
    }
    const char *val = NULL;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        val = JS_ToCString(ctx, argv[2]);
        if (!val) {
            cJSON_Delete(params);
            JS_FreeCString(ctx, pid);
            return JS_EXCEPTION;
        }
    }
    esp_err_t err = minihub_point_merge_params(pid, val, params);
    cJSON_Delete(params);
    JS_FreeCString(ctx, pid);
    if (val) {
        JS_FreeCString(ctx, val);
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_merge_params failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_http_request(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "request expects object");
    }
    const char *url = js_get_str_prop(ctx, argv[0], "url");
    const char *method = js_get_str_prop(ctx, argv[0], "method");
    if (!url) {
        return JS_ThrowTypeError(ctx, "url required");
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    JSValue to = JS_GetPropertyStr(ctx, argv[0], "timeout_ms");
    if (JS_IsNumber(to)) {
        int32_t t = 0;
        JS_ToInt32(ctx, &t, to);
        if (t > 0) {
            cfg.timeout_ms = t;
        }
    }
    JS_FreeValue(ctx, to);

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        JS_FreeCString(ctx, url);
        if (method) {
            JS_FreeCString(ctx, method);
        }
        return JS_ThrowInternalError(ctx, "http init failed");
    }

    if (method && method[0]) {
        if (strcasecmp(method, "POST") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
        } else if (strcasecmp(method, "PUT") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_PUT);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }
    }

    const char *body = js_get_str_prop(ctx, argv[0], "body");
    if (body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_open(client, body ? (int)strlen(body) : 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        JS_FreeCString(ctx, url);
        if (method) {
            JS_FreeCString(ctx, method);
        }
        if (body) {
            JS_FreeCString(ctx, body);
        }
        return JS_ThrowInternalError(ctx, "http open failed");
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *resp = calloc(1, MINIHUB_JS_HTTP_BODY_MAX + 1);
    size_t total = 0;
    if (resp) {
        while (total < MINIHUB_JS_HTTP_BODY_MAX) {
            int n = esp_http_client_read(client, resp + total,
                                         (int)(MINIHUB_JS_HTTP_BODY_MAX - total));
            if (n <= 0) {
                break;
            }
            total += (size_t)n;
        }
        resp[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    JS_FreeCString(ctx, url);
    if (method) {
        JS_FreeCString(ctx, method);
    }
    if (body) {
        JS_FreeCString(ctx, body);
    }

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, out, "body", JS_NewString(ctx, resp ? resp : ""));
    free(resp);
    return out;
}

static JSValue js_config_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    cJSON *adapters = minihub_config_get_adapters();
    if (!adapters) {
        return JS_NULL;
    }
    char *printed = cJSON_PrintUnformatted(adapters);
    if (!printed) {
        return JS_NULL;
    }
    JSValue v = JS_ParseJSON(ctx, printed, strlen(printed), "<config>");
    free(printed);
    return v;
}

static JSValue js_event_subscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "subscribe(event_type, handler)");
    }
    const char *etype = JS_ToCString(ctx, argv[0]);
    if (!etype || !etype[0]) {
        if (etype) {
            JS_FreeCString(ctx, etype);
        }
        return JS_ThrowTypeError(ctx, "event_type required");
    }
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, etype);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }
    if (s_js_event_count >= MINIHUB_JS_EVENT_SUBS_MAX) {
        JS_FreeCString(ctx, etype);
        return JS_ThrowInternalError(ctx, "too many JS event subscriptions");
    }

    js_event_slot_t *slot = &s_js_events[s_js_event_count++];
    strncpy(slot->event_type, etype, sizeof(slot->event_type) - 1);
    slot->event_type[sizeof(slot->event_type) - 1] = '\0';
    slot->fn = JS_DupValue(ctx, argv[1]);
    slot->active = true;
    JS_FreeCString(ctx, etype);

    if (!s_js_event_bus_hooked) {
        esp_err_t err = minihub_event_bus_subscribe("*", js_event_dispatch, NULL);
        if (err != ESP_OK) {
            slot->active = false;
            JS_FreeValue(ctx, slot->fn);
            slot->fn = JS_UNDEFINED;
            s_js_event_count--;
            return JS_ThrowInternalError(ctx, "event_bus_subscribe failed %s",
                                         esp_err_to_name(err));
        }
        s_js_event_bus_hooked = true;
    }
    return JS_UNDEFINED;
}

static JSValue make_host_object(JSContext *ctx)
{
    JSValue host = JS_NewObject(ctx);
    JSValue log = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, log, "info", JS_NewCFunction(ctx, js_log_info, "info", 1));
    JS_SetPropertyStr(ctx, log, "warn", JS_NewCFunction(ctx, js_log_warn, "warn", 1));
    JS_SetPropertyStr(ctx, log, "error", JS_NewCFunction(ctx, js_log_error, "error", 1));
    JS_SetPropertyStr(ctx, host, "log", log);

    JSValue point = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, point, "register", JS_NewCFunction(ctx, js_point_register, "register", 1));
    JS_SetPropertyStr(ctx, point, "get", JS_NewCFunction(ctx, js_point_get, "get", 1));
    JS_SetPropertyStr(ctx, point, "set", JS_NewCFunction(ctx, js_point_set, "set", 2));
    JS_SetPropertyStr(ctx, point, "mergeProps", JS_NewCFunction(ctx, js_point_merge_props, "mergeProps", 2));
    JS_SetPropertyStr(ctx, point, "mergeParams", JS_NewCFunction(ctx, js_point_merge_params, "mergeParams", 2));
    JS_SetPropertyStr(ctx, host, "point", point);

    JSValue command = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, command, "invoke", JS_NewCFunction(ctx, js_command_invoke, "invoke", 1));
    JS_SetPropertyStr(ctx, command, "on", JS_NewCFunction(ctx, js_command_on, "on", 2));
    JS_SetPropertyStr(ctx, host, "command", command);

    JSValue event = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, event, "subscribe", JS_NewCFunction(ctx, js_event_subscribe, "subscribe", 2));
    JS_SetPropertyStr(ctx, host, "event", event);

    JSValue http = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, http, "request", JS_NewCFunction(ctx, js_http_request, "request", 1));
    JS_SetPropertyStr(ctx, host, "http", http);

    JSValue config = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, config, "get", JS_NewCFunction(ctx, js_config_get, "get", 0));
    JS_SetPropertyStr(ctx, host, "config", config);
    return host;
}

static esp_err_t ensure_runtime(void)
{
    if (s_rt && s_ctx) {
        return ESP_OK;
    }
    s_rt = JS_NewRuntime();
    if (!s_rt) {
        return ESP_ERR_NO_MEM;
    }
    s_ctx = JS_NewContext(s_rt);
    if (!s_ctx) {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void dump_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, exc);
    ESP_LOGE(TAG, "JS exception: %s", s ? s : "?");
    if (s) {
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, exc);
}

static esp_err_t run_module_script(const char *id, const char *script_path)
{
    size_t len = 0;
    char *src = read_file_alloc(script_path, MINIHUB_JS_SCRIPT_MAX, &len);
    if (!src) {
        ESP_LOGE(TAG, "cannot read %s", script_path);
        return ESP_FAIL;
    }

    JSValue ev = JS_Eval(s_ctx, src, len, script_path, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(ev)) {
        dump_exception(s_ctx);
        JS_FreeValue(s_ctx, ev);
        return ESP_FAIL;
    }
    JS_FreeValue(s_ctx, ev);

    JSValue global = JS_GetGlobalObject(s_ctx);
    JSValue setup = JS_GetPropertyStr(s_ctx, global, "setup");
    JS_FreeValue(s_ctx, global);
    if (!JS_IsFunction(s_ctx, setup)) {
        ESP_LOGW(TAG, "module %s: no setup(host) function", id);
        JS_FreeValue(s_ctx, setup);
        return ESP_OK;
    }

    JSValue host = make_host_object(s_ctx);
    JSValue args[1] = { host };
    JSValue ret = JS_Call(s_ctx, setup, JS_UNDEFINED, 1, args);
    JS_FreeValue(s_ctx, setup);
    JS_FreeValue(s_ctx, host);
    if (JS_IsException(ret)) {
        dump_exception(s_ctx);
        JS_FreeValue(s_ctx, ret);
        return ESP_FAIL;
    }
    JS_FreeValue(s_ctx, ret);
    ESP_LOGI(TAG, "JS module '%s' setup ok", id);
    return ESP_OK;
}

static bool manifest_is_js_enabled(const char *path, char *id_out, size_t id_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0 || n > 65536) {
        fclose(f);
        return false;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        return false;
    }
    const cJSON *rt = cJSON_GetObjectItem(root, "runtime");
    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    bool ok = false;
    if (cJSON_IsString(rt)) {
        if (strcmp(rt->valuestring, "js") == 0 || strcmp(rt->valuestring, "hybrid") == 0) {
            ok = true;
        }
    }
    if (ok && cJSON_IsBool(en) && !cJSON_IsTrue(en)) {
        ok = false;
    }
    if (ok && cJSON_IsString(id) && id_out && id_len) {
        strncpy(id_out, id->valuestring, id_len - 1);
        id_out[id_len - 1] = '\0';
    }
    cJSON_Delete(root);
    return ok;
}

void minihub_js_unload_all(void)
{
    if (s_ctx) {
        clear_js_cmd_handlers(s_ctx);
        clear_js_event_handlers(s_ctx);
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    } else {
        clear_js_event_handlers(NULL);
    }
    if (s_rt) {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
    }
}

esp_err_t minihub_js_reload(void)
{
    minihub_js_unload_all();
    esp_err_t err = ensure_runtime();
    if (err != ESP_OK) {
        return err;
    }

    DIR *d = opendir(MINIHUB_MODULES_DIR);
    if (!d) {
        ESP_LOGI(TAG, "no modules dir yet (%s)", MINIHUB_MODULES_DIR);
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        /* Bound name length so snprintf cannot truncate under -Werror=format-truncation */
        char name[MINIHUB_MODULE_ID_MAX];
        if (strlen(ent->d_name) >= sizeof(name)) {
            ESP_LOGW(TAG, "skip overlong module dir name");
            continue;
        }
        memcpy(name, ent->d_name, strlen(ent->d_name) + 1);

        char manifest[MINIHUB_PATH_MAX];
        if (snprintf(manifest, sizeof(manifest), "%s/%s/module.json",
                     MINIHUB_MODULES_DIR, name) >= (int)sizeof(manifest)) {
            continue;
        }
        char id[MINIHUB_MODULE_ID_MAX] = {0};
        if (!manifest_is_js_enabled(manifest, id, sizeof(id))) {
            continue;
        }
        char script[MINIHUB_PATH_MAX];
        if (snprintf(script, sizeof(script), "%s/%s/main.js",
                     MINIHUB_MODULES_DIR, name) >= (int)sizeof(script)) {
            continue;
        }
        struct stat st;
        if (stat(script, &st) != 0) {
            ESP_LOGW(TAG, "JS module %s missing main.js", id[0] ? id : name);
            continue;
        }
        err = run_module_script(id[0] ? id : name, script);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "JS module %s failed: %s", id[0] ? id : name, esp_err_to_name(err));
        }
    }
    closedir(d);
    return ESP_OK;
}

#else /* !CONFIG_MINIHUB_JS_PLUGINS */

esp_err_t minihub_js_reload(void)
{
    ESP_LOGD(TAG, "JS plugins disabled");
    return ESP_OK;
}

void minihub_js_unload_all(void)
{
}

#endif
