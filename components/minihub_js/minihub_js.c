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
 *   host.point.unregister(id) — drop a point; missing id is not an error
 *   host.point.mergeProps(id, patch, value?)
 *   host.point.mergeParams(id, params, value?)
 *   host.command.on(adapter, fn)  — route Core commands into JS
 *   host.command.invoke({ point_id, cmd, params? })
 *   host.event.subscribe(type, fn) — point.updated / point.registered / …
 *   host.timer.setTimeout/setInterval/clearTimeout/clearInterval (also global)
 *   host.adapter.poll(handler) / host.adapter.clearPoll() — framework 5s tick
 *   host.http.request({ url, method?, body?, headers?, timeout_ms? })
 *   host.net.udp.open/sendto/recvfrom/close
 *   host.crypto.md5/sha1/sha256/hmacSha256/aes128CbcEncrypt|Decrypt
 *   host.ble.ensureScan/releaseScan/getAds (shared passive scan, minihub_js_ble.c)
 *   host.mdns.resolve({service,proto,timeout_ms,max_results}) (minihub_js_mdns.c)
 *   host.mqtt.subscribe(filter) / getMessages() / publish(topic,payload) (minihub_js_mqtt.c)
 *   host.time.offsetMin() / host.time.id() — hub timezone, minutes east of UTC
 *   host.config.get() / host.config.save(adaptersRoot)
 *   host.module.rpc(moduleId, method, params?) / host.module.registerRpc(method, fn)
 */

#include "minihub_js.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_tz.h"
#include "minihub_module.h"
#include "minihub_paths.h"
#include "minihub_task.h"
#include "minihub_adapter.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_js";

#if CONFIG_MINIHUB_JS_PLUGINS

#include "quickjs.h"
#include "minihub_js_ext.h"

#define MINIHUB_JS_HTTP_BODY_MAX (64 * 1024)
/* One task (16 KB stack) + up to MINIHUB_JS_HTTP_BODY_MAX of body per in-flight
 * requestAsync. A bulk cloud refresh fires many at once, so bound the
 * concurrency and reject the overflow instead of exhausting tasks or heap. */
#define MINIHUB_JS_HTTP_MAX 4
#define MINIHUB_JS_SCRIPT_MAX    (1024 * 1024)
#define MINIHUB_JS_CMD_HANDLERS_MAX 8
#define MINIHUB_JS_EVENT_SUBS_MAX   8
#define MINIHUB_JS_RPC_HANDLERS_MAX 48
#define MINIHUB_JS_RPC_MODULES_MAX  4

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

typedef struct {
    char module_id[32];
    char method[32];
    JSValue fn;
} js_rpc_slot_t;

static js_rpc_slot_t s_js_rpcs[MINIHUB_JS_RPC_HANDLERS_MAX];
static size_t s_js_rpc_count;

typedef struct {
    char module_id[32];
    bool registered;
} js_rpc_module_t;

static js_rpc_module_t s_js_rpc_modules[MINIHUB_JS_RPC_MODULES_MAX];
static size_t s_js_rpc_module_count;
static char s_setup_module_id[32];

/* Timers: setTimeout/setInterval exposed to JS. All JS (including timer
 * callbacks) runs on js_worker, so this table is only touched from that one
 * task — no extra locking needed. */
#define MINIHUB_JS_TIMERS_MAX 16
typedef struct {
    bool active;
    bool interval;     /* false = one-shot timeout, true = repeating */
    uint32_t delay_ms; /* original period, for interval re-arm */
    TickType_t due;    /* absolute tick when it next fires */
    JSValue fn;
} js_timer_slot_t;
static js_timer_slot_t s_js_timers[MINIHUB_JS_TIMERS_MAX];

/* Framework poll hooks: js_poll_tick is registered once with minihub_adapter and
 * runs on adapter_poll_task every ~5 s. It is fire-and-forget (only notifies
 * js_worker) so it never blocks the framework poll loop or a hub reload. The JS
 * poll callbacks themselves run on js_worker, keeping QuickJS single-threaded. */
#define MINIHUB_JS_POLLS_MAX 8
typedef struct {
    JSValue fn;
    bool active;
} js_poll_slot_t;
static js_poll_slot_t s_js_polls[MINIHUB_JS_POLLS_MAX];
static size_t s_js_poll_count;
static volatile bool s_js_poll_pending;
static volatile bool s_js_poll_inflight;
static bool s_js_poll_hook_registered;

/*
 * QuickJS is single-threaded and records the C stack pointer once in
 * JS_NewRuntime(). Every eval and call runs on this task so that snapshot
 * matches the stack actually in use. Stack size and budget come from
 * Kconfig (MINIHUB_JS_WORKER_STACK_*).
 */
#define JS_WORKER_STACK_BYTES  CONFIG_MINIHUB_JS_WORKER_STACK_SIZE
#define JS_WORKER_STACK_BUDGET CONFIG_MINIHUB_JS_WORKER_STACK_BUDGET

#if CONFIG_MINIHUB_JS_WORKER_STACK_BUDGET >= CONFIG_MINIHUB_JS_WORKER_STACK_SIZE
#error "MINIHUB_JS_WORKER_STACK_BUDGET must be smaller than MINIHUB_JS_WORKER_STACK_SIZE"
#endif

typedef esp_err_t (*js_job_fn)(void *arg);

typedef struct {
    js_job_fn fn;
    void *arg;
    esp_err_t result;
} js_job_t;

static js_job_t *s_js_job;
static SemaphoreHandle_t s_js_admit;
static SemaphoreHandle_t s_js_done;
static TaskHandle_t s_js_task;

/* Fire-and-forget jobs submitted by other tasks (e.g. the single httpd task).
 * The worker drains this queue every loop iteration, so a long RPC (miot_js
 * sync) executes on the worker WITHOUT blocking the caller: the httpd task
 * stays free to serve other requests (/api/points, device detail, the rules
 * editor) while a sync is running, instead of stalling the whole UI. */
typedef struct js_async_job {
    struct js_async_job *next;
    js_job_fn fn;
    void *arg;
} js_async_job_t;

static js_async_job_t *s_js_async_head;
static js_async_job_t *s_js_async_tail;
static SemaphoreHandle_t s_js_async_lock;

static void js_worker(void *arg);
static void js_bind_stack(void);
static void js_polls_fire(void);
static void js_timers_fire_due(void);
static TickType_t js_timers_wait_ticks(void);
static void js_timers_clear_all(JSContext *ctx);
static void js_timer_install_global(JSContext *ctx);

static void js_bind_stack(void)
{
    if (!s_rt) {
        return;
    }
    JS_SetMaxStackSize(s_rt, JS_WORKER_STACK_BUDGET);
    JS_UpdateStackTop(s_rt);
}

static esp_err_t js_ensure_task(void)
{
    if (s_js_task) {
        return ESP_OK;
    }
    if (!s_js_admit) {
        s_js_admit = xSemaphoreCreateMutex();
        s_js_done = xSemaphoreCreateBinary();
        s_js_async_lock = xSemaphoreCreateMutex();
        if (!s_js_admit || !s_js_done || !s_js_async_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t ok = minihub_task_create_psram(js_worker, "js_worker", JS_WORKER_STACK_BYTES,
                                             NULL, 4, &s_js_task);
    if (ok != pdPASS) {
        s_js_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t js_call_on_worker(js_job_fn fn, void *arg)
{
    if (js_ensure_task() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    /* Host calls made from a script are already on this task. */
    if (xTaskGetCurrentTaskHandle() == s_js_task) {
        return fn(arg);
    }
    if (xSemaphoreTake(s_js_admit, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGW(TAG, "js worker busy");
        return ESP_ERR_TIMEOUT;
    }
    js_job_t job = {
        .fn = fn,
        .arg = arg,
        .result = ESP_FAIL,
    };
    s_js_job = &job;
    xTaskNotifyGive(s_js_task);
    xSemaphoreTake(s_js_done, portMAX_DELAY);
    esp_err_t err = job.result;
    xSemaphoreGive(s_js_admit);
    return err;
}

/* Enqueue a job to run on the worker without blocking the caller. The worker
 * drains the queue each loop iteration. Used by the web server so a long module
 * RPC (e.g. miot_js sync) does not stall the single httpd task. */
static esp_err_t js_run_async(js_job_fn fn, void *arg)
{
    if (js_ensure_task() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_js_async_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    js_async_job_t *job = malloc(sizeof(js_async_job_t));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->next = NULL;
    job->fn = fn;
    job->arg = arg;
    if (xSemaphoreTake(s_js_async_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(job);
        return ESP_ERR_TIMEOUT;
    }
    if (s_js_async_tail) {
        s_js_async_tail->next = job;
    } else {
        s_js_async_head = job;
    }
    s_js_async_tail = job;
    xSemaphoreGive(s_js_async_lock);
    xTaskNotifyGive(s_js_task);
    return ESP_OK;
}

/* Run every queued async job. Called from the worker loop; each job runs inline
 * on the worker (so QuickJS stays single-threaded). A long job keeps the worker
 * busy but never blocks the httpd task that submitted it. */
static void js_async_drain(void)
{
    if (!s_js_async_lock) {
        return;
    }
    if (xSemaphoreTake(s_js_async_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    js_async_job_t *job = s_js_async_head;
    s_js_async_head = NULL;
    s_js_async_tail = NULL;
    xSemaphoreGive(s_js_async_lock);
    while (job) {
        js_async_job_t *next = job->next;
        if (job->fn) {
            job->fn(job->arg);
        }
        free(job);
        job = next;
    }
}

esp_err_t minihub_js_run_on_worker_async(js_job_fn fn, void *arg)
{
    return js_run_async(fn, arg);
}

/* Forward declaration: drains QuickJS pending jobs (promise reactions /
 * async resumes) so awaited host.http.requestAsync continuations resume. */
static bool js_jobs_drain(void);

static void js_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "js worker ready (stack %u, budget %u)",
             (unsigned)JS_WORKER_STACK_BYTES, (unsigned)JS_WORKER_STACK_BUDGET);
    bool jobs_remain = false;
    for (;;) {
        js_bind_stack();
        /* Fire any timers whose deadline passed — runs JS callbacks on this
         * same task, keeping QuickJS single-threaded. */
        js_timers_fire_due();
        /* Run any promise reactions / async resumes the timer callbacks queued. */
        jobs_remain = js_jobs_drain();

        /* Wait for the next job, or until the nearest timer comes due. A capped
         * drain leaves reactions queued and nothing else will wake us — with no
         * timer armed js_timers_wait_ticks() returns portMAX_DELAY, so an
         * await would sleep until some unrelated notify. Wake promptly instead. */
        TickType_t wait_ticks = js_timers_wait_ticks();
        if (jobs_remain && wait_ticks > 1) {
            wait_ticks = 1;
        }
        uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);
        if (notified == 0) {
            continue; /* timeout: a timer is due, or more jobs to drain */
        }
        /* A notify may carry a framework poll tick, a normal job, or both. */
        if (s_js_poll_pending) {
            s_js_poll_pending = false;
            js_polls_fire();
        }
        /* MQTT messages enqueued by the esp-mqtt task are delivered as JS
         * callbacks here (on the worker), so a plugin never polls for them. */
        if (minihub_js_mqtt_has_pending()) {
            minihub_js_mqtt_clear_pending();
            minihub_js_mqtt_dispatch(s_ctx);
        }
        js_job_t *job = s_js_job;
        if (job && job->fn) {
            js_bind_stack();
            job->result = job->fn(job->arg);
            s_js_job = NULL;
            xSemaphoreGive(s_js_done);
        }
        /* Drain fire-and-forget jobs (e.g. web RPCs). Runs every iteration so a
         * long job never permanently starves the queue; the httpd task that
         * submitted it is already free. */
        js_async_drain();
        /* Run any promise reactions / async resumes those jobs queued (e.g. a
         * requestAsync completion resolving back into the worker). No need to
         * carry its result: the drain at the top of the next iteration re-decides
         * the wait before anything blocks. */
        js_jobs_drain();
    }
}

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

/* Registered as a minihub_adapter poll hook; runs on adapter_poll_task every
 * ~5 s. Must NOT block or take locks — only wake js_worker when a JS poll is
 * due. A previous run that has not finished (JS still executing) is skipped to
 * avoid piling up notifications. */
static void js_poll_tick(void)
{
    if (!s_ctx || !s_js_task) {
        return;
    }
    if (s_js_poll_count == 0) {
        return;
    }
    if (s_js_poll_inflight) {
        return; /* previous run still executing on js_worker — skip */
    }
    s_js_poll_pending = true;
    s_js_poll_inflight = true;
    xTaskNotifyGive(s_js_task);
}

/* Runs on js_worker; invokes every registered JS poll callback, then releases
 * the in-flight guard so the next framework tick can wake us again. */
static void js_polls_fire(void)
{
    if (!s_ctx) {
        s_js_poll_inflight = false;
        return;
    }
    for (size_t i = 0; i < MINIHUB_JS_POLLS_MAX; i++) {
        if (!s_js_polls[i].active) {
            continue;
        }
        if (!JS_IsFunction(s_ctx, s_js_polls[i].fn)) {
            continue;
        }
        JSValue ret = JS_Call(s_ctx, s_js_polls[i].fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            dump_exception(s_ctx);
        }
        JS_FreeValue(s_ctx, ret);
    }
    s_js_poll_inflight = false;
}

static void js_polls_clear_all(JSContext *ctx)
{
    for (size_t i = 0; i < MINIHUB_JS_POLLS_MAX; i++) {
        if (s_js_polls[i].active) {
            s_js_polls[i].active = false;
            if (ctx) {
                JS_FreeValue(ctx, s_js_polls[i].fn);
            }
            s_js_polls[i].fn = JS_UNDEFINED;
        }
    }
    s_js_poll_count = 0;
    s_js_poll_pending = false;
    s_js_poll_inflight = false;
}
static void js_poll_tick(void);
static void js_polls_clear_all(JSContext *ctx);

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

static void clear_js_rpc_handlers(JSContext *ctx)
{
    for (size_t i = 0; i < s_js_rpc_count; i++) {
        if (ctx) {
            JS_FreeValue(ctx, s_js_rpcs[i].fn);
        }
        s_js_rpcs[i].fn = JS_UNDEFINED;
        s_js_rpcs[i].module_id[0] = '\0';
        s_js_rpcs[i].method[0] = '\0';
    }
    s_js_rpc_count = 0;
    for (size_t i = 0; i < s_js_rpc_module_count; i++) {
        if (s_js_rpc_modules[i].module_id[0]) {
            (void)minihub_module_forget_host_rpc(s_js_rpc_modules[i].module_id);
        }
        s_js_rpc_modules[i].module_id[0] = '\0';
        s_js_rpc_modules[i].registered = false;
    }
    s_js_rpc_module_count = 0;
    s_setup_module_id[0] = '\0';
}

static void js_event_run(const minihub_event_t *event, void *user_data)
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

static esp_err_t js_event_job(void *arg)
{
    js_event_run(arg, NULL);
    return ESP_OK;
}

static void js_event_dispatch(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event || !s_ctx) {
        return;
    }
    (void)js_call_on_worker(js_event_job, (void *)event);
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

static esp_err_t js_command_run(const minihub_command_t *cmd)
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

static esp_err_t js_command_job(void *arg)
{
    return js_command_run(arg);
}

static esp_err_t js_command_dispatch(const minihub_command_t *cmd)
{
    if (!cmd) {
        return ESP_ERR_INVALID_ARG;
    }
    return js_call_on_worker(js_command_job, (void *)cmd);
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

static JSValue js_point_set_online(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "setOnline(id, online)");
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    if (!pid || !pid[0]) {
        if (pid) {
            JS_FreeCString(ctx, pid);
        }
        return JS_ThrowTypeError(ctx, "point_id required");
    }
    bool online = argc >= 2 && JS_ToBool(ctx, argv[1]);
    esp_err_t err = minihub_point_set_online(pid, online);
    JS_FreeCString(ctx, pid);
    if (err == ESP_ERR_NOT_FOUND) {
        return JS_NewBool(ctx, false); /* not yet discovered; harmless for LWT */
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_set_online failed %s", esp_err_to_name(err));
    }
    return JS_NewBool(ctx, true);
}

static JSValue js_point_unregister(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "unregister(id)");
    }
    const char *pid = JS_ToCString(ctx, argv[0]);
    if (!pid || !pid[0]) {
        if (pid) {
            JS_FreeCString(ctx, pid);
        }
        return JS_ThrowTypeError(ctx, "point_id required");
    }
    esp_err_t err = minihub_point_unregister(pid);
    JS_FreeCString(ctx, pid);
    if (err == ESP_ERR_NOT_FOUND) {
        return JS_UNDEFINED;
    }
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "point_unregister failed %s", esp_err_to_name(err));
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

typedef struct {
    char *body;
    size_t body_len;
    size_t body_cap;
    char *location;
    char *set_cookie;
    size_t set_cookie_len;
    size_t set_cookie_cap;
    char *ext_pragma;
} js_http_evt_ctx_t;

#define JS_HTTP_SET_COOKIE_MAX  (8 * 1024)
#define JS_HTTP_LOCATION_MAX    2048
#define JS_HTTP_EXT_PRAGMA_MAX  768

static void js_http_evt_ctx_free(js_http_evt_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    free(ctx->body);
    free(ctx->location);
    free(ctx->set_cookie);
    free(ctx->ext_pragma);
    free(ctx);
}

static esp_err_t js_http_body_append(js_http_evt_ctx_t *ctx, const char *data, size_t len)
{
    if (!ctx || !data || len == 0) {
        return ESP_OK;
    }
    if (ctx->body_len + len > MINIHUB_JS_HTTP_BODY_MAX) {
        len = MINIHUB_JS_HTTP_BODY_MAX - ctx->body_len;
        if (len == 0) {
            return ESP_OK;
        }
    }
    size_t need = ctx->body_len + len + 1;
    if (need > ctx->body_cap) {
        size_t ncap = ctx->body_cap ? ctx->body_cap * 2 : 1024;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > MINIHUB_JS_HTTP_BODY_MAX + 1) {
            ncap = MINIHUB_JS_HTTP_BODY_MAX + 1;
        }
        char *nb = realloc(ctx->body, ncap);
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        ctx->body = nb;
        ctx->body_cap = ncap;
    }
    memcpy(ctx->body + ctx->body_len, data, len);
    ctx->body_len += len;
    ctx->body[ctx->body_len] = '\0';
    return ESP_OK;
}

static esp_err_t js_http_set_cookie_append(js_http_evt_ctx_t *ctx, const char *value)
{
    if (!ctx || !value || !value[0]) {
        return ESP_OK;
    }
    size_t vlen = strlen(value);
    size_t need = ctx->set_cookie_len + vlen + 2; /* optional \n + NUL */
    if (need > JS_HTTP_SET_COOKIE_MAX) {
        return ESP_OK; /* drop further cookies rather than OOM */
    }
    if (need > ctx->set_cookie_cap) {
        size_t ncap = ctx->set_cookie_cap ? ctx->set_cookie_cap * 2 : 512;
        while (ncap < need) {
            ncap *= 2;
        }
        if (ncap > JS_HTTP_SET_COOKIE_MAX) {
            ncap = JS_HTTP_SET_COOKIE_MAX;
        }
        char *nb = realloc(ctx->set_cookie, ncap);
        if (!nb) {
            return ESP_ERR_NO_MEM;
        }
        ctx->set_cookie = nb;
        ctx->set_cookie_cap = ncap;
    }
    if (ctx->set_cookie_len > 0) {
        ctx->set_cookie[ctx->set_cookie_len++] = '\n';
    }
    memcpy(ctx->set_cookie + ctx->set_cookie_len, value, vlen);
    ctx->set_cookie_len += vlen;
    ctx->set_cookie[ctx->set_cookie_len] = '\0';
    return ESP_OK;
}

static esp_err_t js_http_event_handler(esp_http_client_event_t *evt)
{
    js_http_evt_ctx_t *ctx = evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (!evt->header_key || !evt->header_value) {
            break;
        }
        if (strcasecmp(evt->header_key, "Location") == 0) {
            free(ctx->location);
            size_t n = strnlen(evt->header_value, JS_HTTP_LOCATION_MAX);
            ctx->location = malloc(n + 1);
            if (ctx->location) {
                memcpy(ctx->location, evt->header_value, n);
                ctx->location[n] = '\0';
            }
        } else if (strcasecmp(evt->header_key, "Set-Cookie") == 0) {
            (void)js_http_set_cookie_append(ctx, evt->header_value);
        } else if (strcasecmp(evt->header_key, "extension-pragma") == 0) {
            free(ctx->ext_pragma);
            size_t n = strnlen(evt->header_value, JS_HTTP_EXT_PRAGMA_MAX);
            ctx->ext_pragma = malloc(n + 1);
            if (ctx->ext_pragma) {
                memcpy(ctx->ext_pragma, evt->header_value, n);
                ctx->ext_pragma[n] = '\0';
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len > 0) {
            return js_http_body_append(ctx, evt->data, (size_t)evt->data_len);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
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

    js_http_evt_ctx_t *evt_ctx = calloc(1, sizeof(*evt_ctx));
    if (!evt_ctx) {
        JS_FreeCString(ctx, url);
        if (method) {
            JS_FreeCString(ctx, method);
        }
        return JS_ThrowOutOfMemory(ctx);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        /* Long Set-Cookie / Location headers need room for cloud login redirects. */
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
        /* Cloud APIs often return 401 for bad cookies/signatures — do not treat
         * that as an HTTP Basic/Digest challenge (would yield ESP_ERR_NOT_SUPPORTED). */
        .max_authorization_retries = -1,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = js_http_event_handler,
        .user_data = evt_ctx,
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
        js_http_evt_ctx_free(evt_ctx);
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
        } else if (strcasecmp(method, "DELETE") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_DELETE);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }
    }

    /* Prefer plaintext body for JSON APIs; avoid opaque gzip. */
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");

    JSValue headers_v = JS_GetPropertyStr(ctx, argv[0], "headers");
    if (JS_IsObject(headers_v)) {
        JSPropertyEnum *ptab = NULL;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, headers_v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < plen; i++) {
                JSValue key_v = JS_AtomToString(ctx, ptab[i].atom);
                const char *key = JS_ToCString(ctx, key_v);
                JSValue val_v = JS_GetProperty(ctx, headers_v, ptab[i].atom);
                const char *val = JS_ToCString(ctx, val_v);
                if (key && val) {
                    (void)esp_http_client_set_header(client, key, val);
                }
                if (key) {
                    JS_FreeCString(ctx, key);
                }
                if (val) {
                    JS_FreeCString(ctx, val);
                }
                JS_FreeValue(ctx, key_v);
                JS_FreeValue(ctx, val_v);
            }
            JS_FreePropertyEnum(ctx, ptab, plen);
        }
    }
    JS_FreeValue(ctx, headers_v);

    const char *body = js_get_str_prop(ctx, argv[0], "body");
    if (body) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    /* Transport failure (no status): throw. Soft HTTP errors (401/403/5xx): return
     * status+body so JS can handle them — esp_http_client may return ESP_FAIL after
     * max_authorization_retries=-1 on 401. */
    if (err != ESP_OK && status <= 0) {
        js_http_evt_ctx_free(evt_ctx);
        esp_http_client_cleanup(client);
        JS_FreeCString(ctx, url);
        if (method) {
            JS_FreeCString(ctx, method);
        }
        if (body) {
            JS_FreeCString(ctx, body);
        }
        return JS_ThrowInternalError(ctx, "http perform failed (%s)", esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http perform %s status=%d (%s) — returning to JS",
                 esp_err_to_name(err), status, url);
    }

    /* Fallback Location / Set-Cookie if event missed them */
    if (!evt_ctx->location || !evt_ctx->location[0]) {
        char *loc = NULL;
        if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK && loc && loc[0]) {
            free(evt_ctx->location);
            size_t n = strnlen(loc, JS_HTTP_LOCATION_MAX);
            evt_ctx->location = malloc(n + 1);
            if (evt_ctx->location) {
                memcpy(evt_ctx->location, loc, n);
                evt_ctx->location[n] = '\0';
            }
        }
    }
    if (evt_ctx->set_cookie_len == 0) {
        char *sc = NULL;
        if (esp_http_client_get_header(client, "Set-Cookie", &sc) == ESP_OK && sc && sc[0]) {
            (void)js_http_set_cookie_append(evt_ctx, sc);
        }
    }
    if (!evt_ctx->ext_pragma || !evt_ctx->ext_pragma[0]) {
        char *ep = NULL;
        if (esp_http_client_get_header(client, "extension-pragma", &ep) == ESP_OK && ep && ep[0]) {
            free(evt_ctx->ext_pragma);
            size_t n = strnlen(ep, JS_HTTP_EXT_PRAGMA_MAX);
            evt_ctx->ext_pragma = malloc(n + 1);
            if (evt_ctx->ext_pragma) {
                memcpy(evt_ctx->ext_pragma, ep, n);
                evt_ctx->ext_pragma[n] = '\0';
            }
        }
    }

    JSValue hdrs = JS_NewObject(ctx);
    if (evt_ctx->location && evt_ctx->location[0]) {
        JS_SetPropertyStr(ctx, hdrs, "location", JS_NewString(ctx, evt_ctx->location));
    }
    if (evt_ctx->set_cookie_len > 0 && evt_ctx->set_cookie) {
        JS_SetPropertyStr(ctx, hdrs, "set-cookie", JS_NewString(ctx, evt_ctx->set_cookie));
    }
    if (evt_ctx->ext_pragma && evt_ctx->ext_pragma[0]) {
        JS_SetPropertyStr(ctx, hdrs, "extension-pragma", JS_NewString(ctx, evt_ctx->ext_pragma));
    }

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
    bool want_b64 = false;
    JSValue rb = JS_GetPropertyStr(ctx, argv[0], "response_b64");
    want_b64 = JS_ToBool(ctx, rb);
    JS_FreeValue(ctx, rb);
    if (want_b64 && evt_ctx->body && evt_ctx->body_len > 0) {
        size_t olen = 0;
        size_t b64_cap = 4 * ((evt_ctx->body_len + 2) / 3) + 4;
        char *b64 = malloc(b64_cap);
        if (!b64) {
            js_http_evt_ctx_free(evt_ctx);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (mbedtls_base64_encode((unsigned char *)b64, b64_cap, &olen,
                                 (const unsigned char *)evt_ctx->body, evt_ctx->body_len) != 0) {
            free(b64);
            js_http_evt_ctx_free(evt_ctx);
            return JS_ThrowInternalError(ctx, "base64 encode failed");
        }
        b64[olen] = '\0';
        JS_SetPropertyStr(ctx, out, "body", JS_NewString(ctx, b64));
        free(b64);
    } else {
        JS_SetPropertyStr(ctx, out, "body", JS_NewString(ctx, evt_ctx->body ? evt_ctx->body : ""));
    }
    JS_SetPropertyStr(ctx, out, "headers", hdrs);
    js_http_evt_ctx_free(evt_ctx);
    return out;
}

/* --- Asynchronous HTTP (host.http.requestAsync) -----------------------------
 * Mirrors the synchronous js_http_request but runs the blocking socket I/O on a
 * dedicated lower-priority task (js_http_task, prio 3) instead of the js_worker
 * (prio 4). The worker stays free to service device control / UI while a cloud
 * request is in flight, fixing the "UI freezes during miot sync" class of bug.
 * The Promise is resolved back on the worker via js_call_on_worker, so QuickJS is
 * only ever touched on the worker thread (single-threaded). On task-spawn or
 * alloc failure the Promise is rejected (not left unsettled), so `await` never
 * hangs and `.catch()` also works. */

typedef struct {
    char *key;
    char *val;
} js_http_header_t;

typedef struct {
    char *url;
    char *method;
    char *body;
    int timeout_ms;
    bool response_b64;
    js_http_header_t *headers;
    size_t n_headers;
} js_http_req_t;

typedef struct {
    esp_err_t err;
    int status;
    char *location;
    char *set_cookie;
    char *ext_pragma;
    char *body;
    size_t body_len;
} js_http_res_t;

typedef struct js_http_async_t {
    js_http_req_t *req;
    js_http_res_t res;
    JSValue resolve;
    JSValue reject;
    bool response_b64;
    TaskHandle_t task;
    JSRuntime *rt;                      /* runtime that owns resolve/reject */
    bool cancelled;                     /* set on unload; http task skips settle */
    struct js_http_async_t *next_inflight;  /* in-flight registry link */
} js_http_async_t;

/* Registry of requestAsync requests whose HTTP task is still running. Only the
 * js_worker mutates it (creation / settle / unload), so no lock is needed; the
 * http task only reads `cancelled` after its socket I/O returns. */
static js_http_async_t *s_js_http_inflight;

static void js_http_task(void *arg);

static char *js_http_strdup(const char *s)
{
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n + 1);
    }
    return d;
}

static void js_http_req_free(js_http_req_t *req)
{
    if (!req) {
        return;
    }
    free(req->url);
    free(req->method);
    free(req->body);
    for (size_t i = 0; i < req->n_headers; i++) {
        free(req->headers[i].key);
        free(req->headers[i].val);
    }
    free(req->headers);
    free(req);
}

static js_http_req_t *js_http_req_parse(JSContext *ctx, JSValueConst opts)
{
    js_http_req_t *req = calloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }
    const char *url = js_get_str_prop(ctx, opts, "url");
    if (!url) {
        free(req);
        return NULL;
    }
    req->url = js_http_strdup(url);
    JS_FreeCString(ctx, url);
    const char *method = js_get_str_prop(ctx, opts, "method");
    if (method) {
        req->method = js_http_strdup(method);
        JS_FreeCString(ctx, method);
    }
    JSValue to = JS_GetPropertyStr(ctx, opts, "timeout_ms");
    if (JS_IsNumber(to)) {
        int32_t t = 0;
        JS_ToInt32(ctx, &t, to);
        if (t > 0) {
            req->timeout_ms = t;
        }
    }
    JS_FreeValue(ctx, to);
    JSValue rb = JS_GetPropertyStr(ctx, opts, "response_b64");
    req->response_b64 = JS_ToBool(ctx, rb);
    JS_FreeValue(ctx, rb);
    JSValue headers_v = JS_GetPropertyStr(ctx, opts, "headers");
    if (JS_IsObject(headers_v)) {
        JSPropertyEnum *ptab = NULL;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, headers_v,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0 && plen > 0) {
            req->headers = calloc(plen, sizeof(req->headers[0]));
            if (req->headers) {
                req->n_headers = plen;
                for (uint32_t i = 0; i < plen; i++) {
                    JSValue key_v = JS_AtomToString(ctx, ptab[i].atom);
                    const char *key = JS_ToCString(ctx, key_v);
                    JSValue val_v = JS_GetProperty(ctx, headers_v, ptab[i].atom);
                    const char *val = JS_ToCString(ctx, val_v);
                    if (key) {
                        req->headers[i].key = js_http_strdup(key);
                    }
                    if (val) {
                        req->headers[i].val = js_http_strdup(val);
                    }
                    if (key) {
                        JS_FreeCString(ctx, key);
                    }
                    if (val) {
                        JS_FreeCString(ctx, val);
                    }
                    JS_FreeValue(ctx, key_v);
                    JS_FreeValue(ctx, val_v);
                }
            }
            JS_FreePropertyEnum(ctx, ptab, plen);
        }
    }
    JS_FreeValue(ctx, headers_v);
    const char *body = js_get_str_prop(ctx, opts, "body");
    if (body) {
        req->body = js_http_strdup(body);
        JS_FreeCString(ctx, body);
    }
    if (!req->url) {
        js_http_req_free(req);
        return NULL;
    }
    return req;
}

static void js_http_res_free(js_http_res_t *res)
{
    if (!res) {
        return;
    }
    free(res->location);
    free(res->set_cookie);
    free(res->ext_pragma);
    free(res->body);
    *res = (js_http_res_t){0};
}

static esp_err_t js_http_perform(const js_http_req_t *req, js_http_res_t *res)
{
    *res = (js_http_res_t){0};
    js_http_evt_ctx_t *evt_ctx = calloc(1, sizeof(*evt_ctx));
    if (!evt_ctx) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = js_http_event_handler,
        .user_data = evt_ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        js_http_evt_ctx_free(evt_ctx);
        return ESP_FAIL;
    }
    if (req->method && req->method[0]) {
        if (strcasecmp(req->method, "POST") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
        } else if (strcasecmp(req->method, "PUT") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_PUT);
        } else if (strcasecmp(req->method, "DELETE") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_DELETE);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }
    }
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    for (size_t i = 0; i < req->n_headers; i++) {
        if (req->headers[i].key && req->headers[i].val) {
            (void)esp_http_client_set_header(client, req->headers[i].key, req->headers[i].val);
        }
    }
    if (req->body) {
        esp_http_client_set_post_field(client, req->body, (int)strlen(req->body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    res->err = err;
    res->status = status;
    if (!evt_ctx->location || !evt_ctx->location[0]) {
        char *loc = NULL;
        if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK && loc && loc[0]) {
            free(evt_ctx->location);
            size_t n = strnlen(loc, JS_HTTP_LOCATION_MAX);
            evt_ctx->location = malloc(n + 1);
            if (evt_ctx->location) {
                memcpy(evt_ctx->location, loc, n);
                evt_ctx->location[n] = '\0';
            }
        }
    }
    if (evt_ctx->set_cookie_len == 0) {
        char *sc = NULL;
        if (esp_http_client_get_header(client, "Set-Cookie", &sc) == ESP_OK && sc && sc[0]) {
            (void)js_http_set_cookie_append(evt_ctx, sc);
        }
    }
    if (!evt_ctx->ext_pragma || !evt_ctx->ext_pragma[0]) {
        char *ep = NULL;
        if (esp_http_client_get_header(client, "extension-pragma", &ep) == ESP_OK && ep && ep[0]) {
            free(evt_ctx->ext_pragma);
            size_t n = strnlen(ep, JS_HTTP_EXT_PRAGMA_MAX);
            evt_ctx->ext_pragma = malloc(n + 1);
            if (evt_ctx->ext_pragma) {
                memcpy(evt_ctx->ext_pragma, ep, n);
                evt_ctx->ext_pragma[n] = '\0';
            }
        }
    }
    if (evt_ctx->location && evt_ctx->location[0]) {
        size_t n = strnlen(evt_ctx->location, JS_HTTP_LOCATION_MAX);
        res->location = malloc(n + 1);
        if (res->location) {
            memcpy(res->location, evt_ctx->location, n + 1);
        }
    }
    if (evt_ctx->set_cookie && evt_ctx->set_cookie[0]) {
        size_t n = strnlen(evt_ctx->set_cookie, JS_HTTP_SET_COOKIE_MAX);
        res->set_cookie = malloc(n + 1);
        if (res->set_cookie) {
            memcpy(res->set_cookie, evt_ctx->set_cookie, n + 1);
        }
    }
    if (evt_ctx->ext_pragma && evt_ctx->ext_pragma[0]) {
        size_t n = strnlen(evt_ctx->ext_pragma, JS_HTTP_EXT_PRAGMA_MAX);
        res->ext_pragma = malloc(n + 1);
        if (res->ext_pragma) {
            memcpy(res->ext_pragma, evt_ctx->ext_pragma, n + 1);
        }
    }
    if (evt_ctx->body && evt_ctx->body_len > 0) {
        res->body = malloc(evt_ctx->body_len);
        if (res->body) {
            memcpy(res->body, evt_ctx->body, evt_ctx->body_len);
            res->body_len = evt_ctx->body_len;
        }
    }
    esp_http_client_cleanup(client);
    js_http_evt_ctx_free(evt_ctx);
    return err;
}

/* Reject an already-created Promise capability (funcs[1] = reject) and free all
   associated resources, without leaking. Used when the request cannot actually be
   dispatched after the capability exists, so callers using `await`+try/catch and
   `.catch()` both receive the error instead of hanging on an unsettled Promise. */
static void js_http_reject(JSContext *ctx, JSValueConst funcs[2],
                           js_http_req_t *req, const char *msg)
{
    JSValue err = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg));
    JSValue arg = err;
    JSValue ret = JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, err);
    JS_FreeValue(ctx, funcs[0]);
    JS_FreeValue(ctx, funcs[1]);
    if (req) {
        js_http_req_free(req);
    }
}

/* Runs on the js_worker (via js_call_on_worker). Builds the JS response object
   from the completed request and settles the Promise. QuickJS is only touched
   here, never in js_http_task. */
static void js_http_inflight_unlink(js_http_async_t *a)
{
    if (!s_js_http_inflight || !a) {
        return;
    }
    if (s_js_http_inflight == a) {
        s_js_http_inflight = a->next_inflight;
        a->next_inflight = NULL;
        return;
    }
    for (js_http_async_t *p = s_js_http_inflight; p->next_inflight;
         p = p->next_inflight) {
        if (p->next_inflight == a) {
            p->next_inflight = a->next_inflight;
            a->next_inflight = NULL;
            return;
        }
    }
}

static esp_err_t js_http_resolve_job(void *arg)
{
    js_http_async_t *a = arg;
    /* Drop from the registry first; js_unload_job may already have unlinked it
     * (idempotent). */
    js_http_inflight_unlink(a);
    /* The runtime may have been reloaded (module unload / install) while the
     * HTTP task was in flight. resolve/reject belong to the OLD runtime, so
     * touching them now would be a use-after-free (or a NULL-context crash).
     * js_unload_job already rejected the promise and freed its JSValues; the
     * http task frees `a` itself once its socket I/O returns. */
    if (!s_ctx || a->rt != s_rt || a->cancelled) {
        return ESP_OK;
    }
    js_http_res_t *res = &a->res;
    if (res->err != ESP_OK && res->status <= 0) {
        JSValue err = JS_NewObject(s_ctx);
        JS_SetPropertyStr(s_ctx, err, "message",
                          JS_NewString(s_ctx, "http perform failed"));
        JSValue arg2 = err;
        JSValue r = JS_Call(s_ctx, a->reject, JS_UNDEFINED, 1, &arg2);
        JS_FreeValue(s_ctx, r);
        JS_FreeValue(s_ctx, err);
    } else {
        JSValue out = JS_NewObject(s_ctx);
        JS_SetPropertyStr(s_ctx, out, "status", JS_NewInt32(s_ctx, res->status));
        JSValue hdrs = JS_NewObject(s_ctx);
        if (res->location && res->location[0]) {
            JS_SetPropertyStr(s_ctx, hdrs, "location", JS_NewString(s_ctx, res->location));
        }
        if (res->set_cookie && res->set_cookie[0]) {
            JS_SetPropertyStr(s_ctx, hdrs, "set-cookie", JS_NewString(s_ctx, res->set_cookie));
        }
        if (res->ext_pragma && res->ext_pragma[0]) {
            JS_SetPropertyStr(s_ctx, hdrs, "extension-pragma", JS_NewString(s_ctx, res->ext_pragma));
        }
        JS_SetPropertyStr(s_ctx, out, "headers", hdrs);
        if (a->response_b64 && res->body && res->body_len > 0) {
            size_t olen = 0;
            size_t b64_cap = 4 * ((res->body_len + 2) / 3) + 4;
            char *b64 = malloc(b64_cap);
            if (b64) {
                if (mbedtls_base64_encode((unsigned char *)b64, b64_cap, &olen,
                                          (const unsigned char *)res->body,
                                          res->body_len) == 0) {
                    b64[olen] = '\0';
                    JS_SetPropertyStr(s_ctx, out, "body", JS_NewString(s_ctx, b64));
                }
                free(b64);
            }
        } else if (res->body && res->body_len > 0) {
            JS_SetPropertyStr(s_ctx, out, "body",
                              JS_NewStringLen(s_ctx, res->body, res->body_len));
        }
        JSValue r = JS_Call(s_ctx, a->resolve, JS_UNDEFINED, 1, (JSValueConst *)&out);
        JS_FreeValue(s_ctx, r);
        JS_FreeValue(s_ctx, out);
    }
    JS_FreeValue(s_ctx, a->resolve);
    JS_FreeValue(s_ctx, a->reject);
    return ESP_OK;
}

/* Runs on a dedicated FreeRTOS task (prio 3, below the js_worker's 4) so the
   blocking socket I/O never stalls the worker. After the request completes it
   asks the worker to settle the Promise; js_call_on_worker blocks this task
   until that job finishes, after which it is safe to free everything. */
static void js_http_task(void *arg)
{
    js_http_async_t *a = arg;
    js_http_perform(a->req, &a->res);
    /* If the module was unloaded mid-flight, js_unload_job already rejected the
     * promise and freed its resolve/reject JSValues; do not settle again. */
    if (!a->cancelled) {
        js_call_on_worker(js_http_resolve_job, a);
    }
    js_http_req_free(a->req);
    js_http_res_free(&a->res);
    free(a);
    vTaskDelete(NULL);
}

/* Cancel every in-flight requestAsync while the runtime is being torn down.
 * The promise is rejected so the awaiting plugin code settles via .catch()
 * instead of hanging; its resolve/reject JSValues (owned by this runtime) are
 * freed here while the context is still alive. The request struct itself is
 * left for js_http_task to free once its socket I/O returns, to avoid a double
 * free (js_http_task always frees `a`). */
static void js_http_inflight_cancel_all(JSContext *ctx)
{
    js_http_async_t *a = s_js_http_inflight;
    s_js_http_inflight = NULL;
    while (a) {
        js_http_async_t *next = a->next_inflight;
        a->cancelled = true;
        if (ctx && a->rt == s_rt) {
            JSValue err = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, err, "message",
                              JS_NewString(ctx, "module unloaded during request"));
            JSValue arg = err;
            JSValue r = JS_Call(ctx, a->reject, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, err);
            JS_FreeValue(ctx, a->resolve);
            JS_FreeValue(ctx, a->reject);
        }
        a->next_inflight = NULL;
        a = next;
    }
}

/* Worker-thread only (the registry is mutated from JS calls and unload jobs
 * there), so counting the list is enough — no second counter to drift. */
static int js_http_inflight_count(void)
{
    int n = 0;
    for (js_http_async_t *a = s_js_http_inflight; a; a = a->next_inflight) {
        n++;
    }
    return n;
}

static JSValue js_http_request_async(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "request expects object");
    }
    js_http_req_t *req = js_http_req_parse(ctx, argv[0]);
    if (!req) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) {
        js_http_req_free(req);
        return JS_EXCEPTION;
    }
    if (js_http_inflight_count() >= MINIHUB_JS_HTTP_MAX) {
        js_http_reject(ctx, funcs, req, "http async busy: too many in flight");
        return promise;
    }
    js_http_async_t *a = calloc(1, sizeof(*a));
    if (!a) {
        js_http_reject(ctx, funcs, req, "http async alloc failed");
        return promise;
    }
    a->req = req;
    a->resolve = funcs[0];
    a->reject = funcs[1];
    a->response_b64 = req->response_b64;
    /* Lower priority than js_worker (4) so resolving Promises never starves the
       runtime; the blocking I/O happens here, freeing js_worker entirely. */
    BaseType_t ok = xTaskCreate(js_http_task, "js_http", 16384, a, 3, &a->task);
    if (ok != pdPASS) {
        js_http_reject(ctx, funcs, req, "http task spawn failed");
        free(a);
        return promise;
    }
    /* Register so a module unload / install mid-flight can cancel and reject the
     * promise instead of leaving the awaiting JS code wedged on a dead context. */
    a->rt = s_rt;
    a->cancelled = false;
    a->next_inflight = s_js_http_inflight;
    s_js_http_inflight = a;
    return promise;
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

static JSValue js_config_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "save(adaptersRoot)");
    }
    cJSON *root = js_value_to_cjson(ctx, argv[0]);
    if (!root) {
        return JS_ThrowTypeError(ctx, "adaptersRoot must be object");
    }
    esp_err_t err = minihub_config_save_adapters(root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return JS_ThrowInternalError(ctx, "config_save_adapters failed %s", esp_err_to_name(err));
    }
    return JS_UNDEFINED;
}

static JSValue js_cjson_to_value(JSContext *ctx, const cJSON *cj)
{
    if (!cj) {
        return JS_NULL;
    }
    char *printed = cJSON_PrintUnformatted(cj);
    if (!printed) {
        return JS_NULL;
    }
    JSValue v = JS_ParseJSON(ctx, printed, strlen(printed), "<rpc>");
    free(printed);
    return v;
}

static JSValue js_module_rpc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "rpc(moduleId, method, params?)");
    }
    const char *mid = JS_ToCString(ctx, argv[0]);
    const char *method = JS_ToCString(ctx, argv[1]);
    if (!mid || !method) {
        if (mid) {
            JS_FreeCString(ctx, mid);
        }
        if (method) {
            JS_FreeCString(ctx, method);
        }
        return JS_EXCEPTION;
    }
    cJSON *params = NULL;
    if (argc >= 3 && JS_IsObject(argv[2])) {
        params = js_value_to_cjson(ctx, argv[2]);
    }
    cJSON *result = NULL;
    esp_err_t err = minihub_module_invoke_rpc(mid, method, params, &result);
    if (params) {
        cJSON_Delete(params);
    }
    JS_FreeCString(ctx, mid);
    JS_FreeCString(ctx, method);
    if (err != ESP_OK) {
        if (result) {
            cJSON_Delete(result);
        }
        return JS_ThrowInternalError(ctx, "module_rpc failed %s", esp_err_to_name(err));
    }
    JSValue out = js_cjson_to_value(ctx, result);
    if (result) {
        cJSON_Delete(result);
    }
    return out;
}

static esp_err_t js_module_rpc_dispatch_for(const char *module_id, const char *method,
                                            const cJSON *params, cJSON **out)
{
    if (!module_id || !method || !out || !s_ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    js_rpc_slot_t *slot = NULL;
    for (size_t i = 0; i < s_js_rpc_count; i++) {
        if (strcmp(s_js_rpcs[i].module_id, module_id) == 0 &&
            strcmp(s_js_rpcs[i].method, method) == 0) {
            slot = &s_js_rpcs[i];
            break;
        }
    }
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }

    JSValue args[2];
    args[0] = JS_NewString(s_ctx, method);
    if (params) {
        args[1] = js_cjson_to_value(s_ctx, params);
    } else {
        args[1] = JS_NULL;
    }
    JSValue ret = JS_Call(s_ctx, slot->fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(s_ctx, args[0]);
    JS_FreeValue(s_ctx, args[1]);
    if (JS_IsException(ret)) {
        dump_exception(s_ctx);
        JS_FreeValue(s_ctx, ret);
        return ESP_FAIL;
    }
    if (JS_IsBool(ret) && !JS_ToBool(s_ctx, ret)) {
        JS_FreeValue(s_ctx, ret);
        return ESP_FAIL;
    }
    if (JS_IsObject(ret) || JS_IsArray(ret)) {
        *out = js_value_to_cjson(s_ctx, ret);
    } else if (JS_IsString(ret)) {
        const char *s = JS_ToCString(s_ctx, ret);
        *out = cJSON_CreateObject();
        if (*out && s) {
            cJSON_AddStringToObject(*out, "result", s);
        }
        if (s) {
            JS_FreeCString(s_ctx, s);
        }
    } else {
        *out = cJSON_CreateObject();
        if (*out) {
            cJSON_AddStringToObject(*out, "status", "ok");
        }
    }
    JS_FreeValue(s_ctx, ret);
    return ESP_OK;
}

typedef struct {
    size_t idx;
    const char *method;
    const cJSON *params;
    cJSON **out;
} js_rpc_job_t;

static esp_err_t js_rpc_job(void *arg)
{
    const js_rpc_job_t *a = arg;
    if (!a || a->idx >= s_js_rpc_module_count) {
        return ESP_ERR_INVALID_STATE;
    }
    return js_module_rpc_dispatch_for(s_js_rpc_modules[a->idx].module_id, a->method, a->params, a->out);
}

static esp_err_t js_rpc_on_worker(size_t idx, const char *method, const cJSON *params, cJSON **out)
{
    js_rpc_job_t a = {
        .idx = idx,
        .method = method,
        .params = params,
        .out = out,
    };
    return js_call_on_worker(js_rpc_job, &a);
}

static esp_err_t js_rpc_tramp_0(const char *method, const cJSON *params, cJSON **out)
{
    return js_rpc_on_worker(0, method, params, out);
}
static esp_err_t js_rpc_tramp_1(const char *method, const cJSON *params, cJSON **out)
{
    return js_rpc_on_worker(1, method, params, out);
}
static esp_err_t js_rpc_tramp_2(const char *method, const cJSON *params, cJSON **out)
{
    return js_rpc_on_worker(2, method, params, out);
}
static esp_err_t js_rpc_tramp_3(const char *method, const cJSON *params, cJSON **out)
{
    return js_rpc_on_worker(3, method, params, out);
}

static minihub_module_rpc_fn js_rpc_tramp_for_index(size_t idx)
{
    switch (idx) {
    case 0:
        return js_rpc_tramp_0;
    case 1:
        return js_rpc_tramp_1;
    case 2:
        return js_rpc_tramp_2;
    default:
        return js_rpc_tramp_3;
    }
}

static JSValue js_module_register_rpc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "registerRpc(method, handler)");
    }
    if (!s_setup_module_id[0]) {
        return JS_ThrowInternalError(ctx, "registerRpc only during setup(host)");
    }
    const char *method = JS_ToCString(ctx, argv[0]);
    if (!method || !method[0]) {
        if (method) {
            JS_FreeCString(ctx, method);
        }
        return JS_ThrowTypeError(ctx, "method required");
    }
    if (!JS_IsFunction(ctx, argv[1])) {
        JS_FreeCString(ctx, method);
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    js_rpc_slot_t *slot = NULL;
    for (size_t i = 0; i < s_js_rpc_count; i++) {
        if (strcmp(s_js_rpcs[i].module_id, s_setup_module_id) == 0 &&
            strcmp(s_js_rpcs[i].method, method) == 0) {
            slot = &s_js_rpcs[i];
            break;
        }
    }
    if (!slot) {
        if (s_js_rpc_count >= MINIHUB_JS_RPC_HANDLERS_MAX) {
            JS_FreeCString(ctx, method);
            return JS_ThrowInternalError(ctx, "too many JS RPC handlers");
        }
        slot = &s_js_rpcs[s_js_rpc_count++];
        strncpy(slot->module_id, s_setup_module_id, sizeof(slot->module_id) - 1);
        slot->fn = JS_UNDEFINED;
    }
    strncpy(slot->method, method, sizeof(slot->method) - 1);
    slot->method[sizeof(slot->method) - 1] = '\0';
    JS_FreeValue(ctx, slot->fn);
    slot->fn = JS_DupValue(ctx, argv[1]);
    JS_FreeCString(ctx, method);

    size_t mod_idx = (size_t)-1;
    for (size_t i = 0; i < s_js_rpc_module_count; i++) {
        if (strcmp(s_js_rpc_modules[i].module_id, s_setup_module_id) == 0) {
            mod_idx = i;
            break;
        }
    }
    if (mod_idx == (size_t)-1) {
        if (s_js_rpc_module_count >= MINIHUB_JS_RPC_MODULES_MAX) {
            return JS_ThrowInternalError(ctx, "too many JS RPC modules");
        }
        mod_idx = s_js_rpc_module_count++;
        strncpy(s_js_rpc_modules[mod_idx].module_id, s_setup_module_id,
                sizeof(s_js_rpc_modules[mod_idx].module_id) - 1);
        s_js_rpc_modules[mod_idx].registered = false;
    }
    if (!s_js_rpc_modules[mod_idx].registered) {
        esp_err_t err = minihub_module_register_rpc(s_setup_module_id, js_rpc_tramp_for_index(mod_idx));
        if (err != ESP_OK) {
            return JS_ThrowInternalError(ctx, "module_register_rpc failed %s", esp_err_to_name(err));
        }
        s_js_rpc_modules[mod_idx].registered = true;
    }
    return JS_UNDEFINED;
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

/* ---- JS timers: setTimeout / setInterval ----------------------------------
 * Only ever called on js_worker (all JS runs there), so s_js_timers needs no
 * lock. Callbacks are invoked from js_timers_fire_due() on the same task.
 * Accepts (fn, ms) or (ms, fn); returns an integer id (1-based). */

static JSValue js_timer_set_impl(JSContext *ctx, int argc, JSValueConst *argv, bool interval)
{
    JSValue fn = JS_UNDEFINED;
    JSValue ms_v = JS_UNDEFINED;
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) {
        fn = argv[0];
        if (argc >= 2) {
            ms_v = argv[1];
        }
    } else if (argc >= 2 && JS_IsFunction(ctx, argv[1])) {
        fn = argv[1];
        ms_v = argv[0];
    }
    if (JS_IsUndefined(fn)) {
        return JS_ThrowTypeError(ctx, "expected (fn, ms) or (ms, fn)");
    }
    int32_t ms = 0;
    if (!JS_IsUndefined(ms_v)) {
        JS_ToInt32(ctx, &ms, ms_v);
    }
    if (ms <= 0) {
        ms = 1;
    }

    int slot = -1;
    for (int i = 0; i < MINIHUB_JS_TIMERS_MAX; i++) {
        if (!s_js_timers[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return JS_ThrowInternalError(ctx, "too many JS timers (max %d)", MINIHUB_JS_TIMERS_MAX);
    }
    s_js_timers[slot].active = true;
    s_js_timers[slot].interval = interval;
    s_js_timers[slot].delay_ms = (uint32_t)ms;
    s_js_timers[slot].due = xTaskGetTickCount() + pdMS_TO_TICKS((TickType_t)ms);
    s_js_timers[slot].fn = JS_DupValue(ctx, fn);
    return JS_NewInt32(ctx, slot + 1);
}

static JSValue js_timer_clear(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0 || id > MINIHUB_JS_TIMERS_MAX) {
        return JS_UNDEFINED;
    }
    js_timer_slot_t *s = &s_js_timers[id - 1];
    if (s->active) {
        s->active = false;
        JS_FreeValue(ctx, s->fn);
        s->fn = JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

static JSValue js_timer_set_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_timer_set_impl(ctx, argc, argv, false);
}
static JSValue js_timer_set_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_timer_set_impl(ctx, argc, argv, true);
}
static JSValue js_timer_clear_timeout(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_timer_clear(ctx, argc, argv);
}
static JSValue js_timer_clear_interval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_timer_clear(ctx, argc, argv);
}

static void js_timer_install_global(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, js_timer_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_timer_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval", JS_NewCFunction(ctx, js_timer_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval", JS_NewCFunction(ctx, js_timer_clear_interval, "clearInterval", 1));
    JS_FreeValue(ctx, global);
}

static void js_timers_fire_due(void)
{
    if (!s_ctx) {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < MINIHUB_JS_TIMERS_MAX; i++) {
        js_timer_slot_t *s = &s_js_timers[i];
        if (!s->active) {
            continue;
        }
        if ((int32_t)(s->due - now) > 0) {
            continue; /* not due yet */
        }
        JSValue ret = JS_Call(s_ctx, s->fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(ret)) {
            dump_exception(s_ctx);
        }
        JS_FreeValue(s_ctx, ret);
        /* The callback may have cleared or re-armed this timer. */
        if (s->active) {
            if (s->interval) {
                s->due = xTaskGetTickCount() + pdMS_TO_TICKS((TickType_t)s->delay_ms);
            } else {
                JS_FreeValue(s_ctx, s->fn);
                s->fn = JS_UNDEFINED;
                s->active = false;
            }
        }
    }
}

/* Drain QuickJS pending jobs (promise reactions and async-function resumes).
 * quickjs-ng queues these into rt->job_list and ONLY JS_ExecutePendingJob
 * consumes them. Unlike Node, the worker loop does NOT auto-drain this queue,
 * so an awaited Promise would never resume here — the plugin's async refresh
 * chain (host.http.requestAsync -> await) would stall on its first await and
 * never run the code after it (including a `finally` guard reset). Run after
 * timers and after async jobs every iteration.
 *
 * Upper bound on jobs drained per worker iteration. A self-resolving Promise
 * chain could otherwise loop forever inside this single call and wedge the
 * worker; capping it bounds one drain pass and lets the loop service timers /
 * polls / I/O next iteration. The cap is only safe because js_jobs_drain()
 * reports it: a leftover job has no notify of its own, and with no timer armed
 * js_timers_wait_ticks() returns portMAX_DELAY, so the worker must not block on
 * it — the caller shortens that wait instead. */
#define MINIHUB_JS_JOBS_DRAIN_MAX 64

static bool js_jobs_drain(void)
{
    if (!s_rt || !s_ctx) {
        return false;
    }
    JSContext *ctx = s_ctx;
    for (int n = 0; n < MINIHUB_JS_JOBS_DRAIN_MAX; n++) {
        int rc = JS_ExecutePendingJob(s_rt, &ctx);
        if (rc == 0) {
            return false; /* queue empty: only now may the worker block */
        }
        if (rc < 0) {
            /* JS_ExecutePendingJob unlinks the entry BEFORE running it, so a
             * throwing job is consumed while later jobs stay in rt->job_list.
             * Report the error and keep draining — returning false here would
             * strand the rest exactly like a missing drain. */
            dump_exception(ctx);
        }
    }
    return true; /* cap hit: more may be queued, do not sleep on them */
}

static TickType_t js_timers_wait_ticks(void)
{
    TickType_t now = xTaskGetTickCount();
    int32_t best = -1; /* smallest remaining ticks; -1 = no active timer */
    for (int i = 0; i < MINIHUB_JS_TIMERS_MAX; i++) {
        if (!s_js_timers[i].active) {
            continue;
        }
        int32_t remaining = (int32_t)(s_js_timers[i].due - now);
        if (remaining < 0) {
            remaining = 0;
        }
        if (best < 0 || remaining < best) {
            best = remaining;
        }
    }
    if (best < 0) {
        return portMAX_DELAY; /* nothing scheduled: block until a job arrives */
    }
    if (best == 0) {
        return 1; /* already due: wake promptly, fire at loop top */
    }
    return (TickType_t)best;
}

static void js_timers_clear_all(JSContext *ctx)
{
    for (int i = 0; i < MINIHUB_JS_TIMERS_MAX; i++) {
        if (s_js_timers[i].active) {
            s_js_timers[i].active = false;
            if (ctx) {
                JS_FreeValue(ctx, s_js_timers[i].fn);
            }
            s_js_timers[i].fn = JS_UNDEFINED;
        }
    }
}

static JSValue js_adapter_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "poll(handler)");
    }
    if (s_js_poll_count >= MINIHUB_JS_POLLS_MAX) {
        return JS_ThrowInternalError(ctx, "too many JS poll handlers");
    }
    js_poll_slot_t *slot = &s_js_polls[s_js_poll_count++];
    slot->fn = JS_DupValue(ctx, argv[0]);
    slot->active = true;
    return JS_UNDEFINED;
}

static JSValue js_adapter_clear_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    js_polls_clear_all(ctx);
    return JS_UNDEFINED;
}

static JSValue js_time_offset_min(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, minihub_tz_offset_min());
}

static JSValue js_time_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, minihub_tz_id());
}

static JSValue make_host_object(JSContext *ctx, const char *module_id)
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
    JS_SetPropertyStr(ctx, point, "setOnline", JS_NewCFunction(ctx, js_point_set_online, "setOnline", 2));
    JS_SetPropertyStr(ctx, point, "unregister", JS_NewCFunction(ctx, js_point_unregister, "unregister", 1));
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
    JS_SetPropertyStr(ctx, http, "requestAsync",
                      JS_NewCFunction(ctx, js_http_request_async, "requestAsync", 1));
    JS_SetPropertyStr(ctx, host, "http", http);

    JSValue config = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, config, "get", JS_NewCFunction(ctx, js_config_get, "get", 0));
    JS_SetPropertyStr(ctx, config, "save", JS_NewCFunction(ctx, js_config_save, "save", 1));
    JS_SetPropertyStr(ctx, host, "config", config);

    JSValue module = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, module, "rpc", JS_NewCFunction(ctx, js_module_rpc, "rpc", 3));
    JS_SetPropertyStr(ctx, module, "registerRpc",
                      JS_NewCFunction(ctx, js_module_register_rpc, "registerRpc", 2));
    JS_SetPropertyStr(ctx, host, "module", module);

    JSValue timer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, timer, "setTimeout", JS_NewCFunction(ctx, js_timer_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, timer, "clearTimeout", JS_NewCFunction(ctx, js_timer_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, timer, "setInterval", JS_NewCFunction(ctx, js_timer_set_interval, "setInterval", 2));
    JS_SetPropertyStr(ctx, timer, "clearInterval", JS_NewCFunction(ctx, js_timer_clear_interval, "clearInterval", 1));
    JS_SetPropertyStr(ctx, host, "timer", timer);

    JSValue adapter = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, adapter, "poll", JS_NewCFunction(ctx, js_adapter_poll, "poll", 1));
    JS_SetPropertyStr(ctx, adapter, "clearPoll",
                      JS_NewCFunction(ctx, js_adapter_clear_poll, "clearPoll", 0));
    JS_SetPropertyStr(ctx, host, "adapter", adapter);

    JSValue time_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, time_obj, "offsetMin",
                      JS_NewCFunction(ctx, js_time_offset_min, "offsetMin", 0));
    JS_SetPropertyStr(ctx, time_obj, "id", JS_NewCFunction(ctx, js_time_id, "id", 0));
    JS_SetPropertyStr(ctx, host, "time", time_obj);

    minihub_js_attach_crypto(ctx, host);
    minihub_js_attach_net(ctx, host);
    minihub_js_attach_ble(ctx, host);
    minihub_js_attach_mdns(ctx, host);
    minihub_js_attach_mqtt(ctx, host, module_id);
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
    /* JS_NewRuntime records a 1 MB budget. Re-bind to this task's real stack. */
    js_bind_stack();
    /* Inject global setTimeout/clearTimeout/setInterval/clearInterval. */
    js_timer_install_global(s_ctx);
    /* Register the framework poll hook once (component lifetime). The hook is
     * fire-and-forget; it only activates once a JS module calls host.adapter.poll. */
    if (!s_js_poll_hook_registered) {
        (void)minihub_adapter_register_poll_hook(js_poll_tick);
        s_js_poll_hook_registered = true;
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

/* Exported wrapper so other Host-binding TUs (e.g. minihub_js_mqtt.c) can log a
 * pending JS exception without exposing the static helper above. */
void js_dump_exception(JSContext *ctx)
{
    dump_exception(ctx);
}

/* Every main.js shares one JSContext. A raw global eval would let the next
 * module replace function setup / handleCommand, and closures registered by
 * the previous module would call the new function. Run each file in its own
 * function so those names stay with that module. The tail publishes setup on
 * the real global under a reserved key; the caller clears it. */
static char *wrap_module_source(const char *src, size_t len, size_t *out_len)
{
    static const char head[] = "(function(){\n";
    static const char tail[] =
        "\n;if(typeof setup==='function')globalThis.__mh_mod_setup=setup;\n})();\n";
    size_t total = (sizeof(head) - 1) + len + (sizeof(tail) - 1);
    char *buf = malloc(total + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, head, sizeof(head) - 1);
    memcpy(buf + (sizeof(head) - 1), src, len);
    memcpy(buf + (sizeof(head) - 1) + len, tail, sizeof(tail));
    if (out_len) {
        *out_len = total;
    }
    return buf;
}

static esp_err_t run_module_script(const char *id, const char *script_path)
{
    size_t len = 0;
    char *src = read_file_alloc(script_path, MINIHUB_JS_SCRIPT_MAX, &len);
    if (!src) {
        ESP_LOGE(TAG, "cannot read %s", script_path);
        return ESP_FAIL;
    }

    size_t wrapped_len = 0;
    char *wrapped = wrap_module_source(src, len, &wrapped_len);
    free(src);
    if (!wrapped) {
        return ESP_ERR_NO_MEM;
    }

    JSValue global = JS_GetGlobalObject(s_ctx);
    JS_SetPropertyStr(s_ctx, global, "__mh_mod_setup", JS_UNDEFINED);

    /* Filename stays the real main.js. The wrapper adds one line, so exception
     * line numbers are one past the source. */
    JSValue ev = JS_Eval(s_ctx, wrapped, wrapped_len, script_path, JS_EVAL_TYPE_GLOBAL);
    free(wrapped);
    if (JS_IsException(ev)) {
        dump_exception(s_ctx);
        JS_FreeValue(s_ctx, ev);
        JS_FreeValue(s_ctx, global);
        return ESP_FAIL;
    }
    JS_FreeValue(s_ctx, ev);

    JSValue setup = JS_GetPropertyStr(s_ctx, global, "__mh_mod_setup");
    JS_SetPropertyStr(s_ctx, global, "__mh_mod_setup", JS_UNDEFINED);
    JS_FreeValue(s_ctx, global);
    if (!JS_IsFunction(s_ctx, setup)) {
        ESP_LOGW(TAG, "module %s: no setup(host) function", id);
        JS_FreeValue(s_ctx, setup);
        return ESP_OK;
    }

    JSValue host = make_host_object(s_ctx, id);
    strncpy(s_setup_module_id, id, sizeof(s_setup_module_id) - 1);
    s_setup_module_id[sizeof(s_setup_module_id) - 1] = '\0';
    JSValue args[1] = { host };
    JSValue ret = JS_Call(s_ctx, setup, JS_UNDEFINED, 1, args);
    s_setup_module_id[0] = '\0';
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
    if (ok && !minihub_module_requires_met(cJSON_GetObjectItem(root, "requires"))) {
        ESP_LOGI(TAG, "skip JS module '%s' (required capability is off)",
                 cJSON_IsString(id) ? id->valuestring : "?");
        ok = false;
    }
    if (ok && cJSON_IsString(id) && id_out && id_len) {
        strncpy(id_out, id->valuestring, id_len - 1);
        id_out[id_len - 1] = '\0';
    }
    cJSON_Delete(root);
    return ok;
}

static esp_err_t js_unload_job(void *arg)
{
    (void)arg;
    if (s_ctx) {
        clear_js_cmd_handlers(s_ctx);
        clear_js_event_handlers(s_ctx);
        clear_js_rpc_handlers(s_ctx);
        js_timers_clear_all(s_ctx);
        js_polls_clear_all(s_ctx);
        js_http_inflight_cancel_all(s_ctx);
        minihub_js_mqtt_reset(s_ctx); /* must precede JS_FreeContext: frees C-side hook state */
        JS_FreeContext(s_ctx);
        s_ctx = NULL;
    } else {
        clear_js_event_handlers(NULL);
        clear_js_rpc_handlers(NULL);
        js_timers_clear_all(NULL);
        js_polls_clear_all(NULL);
        minihub_js_mqtt_reset(NULL);
    }
    if (s_rt) {
        JS_FreeRuntime(s_rt);
        s_rt = NULL;
    }
    return ESP_OK;
}

void minihub_js_unload_all(void)
{
    (void)js_call_on_worker(js_unload_job, NULL);
}

static esp_err_t js_reload_job(void *arg)
{
    (void)arg;
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

esp_err_t minihub_js_reload(void)
{
    return js_call_on_worker(js_reload_job, NULL);
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
