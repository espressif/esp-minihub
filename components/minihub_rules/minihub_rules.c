/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "minihub_core.h"
#include "minihub_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "minihub_rules";

#define MAX_RULES 16
#define RULE_BUCKETS 16
#define MAX_RULES_PER_BUCKET 8
/* Poll cadence for time-triggered rules. Matching is minute-resolution
 * (HH:MM), so the rule fires at the first poll inside the target minute.
 * 30s made rules fire up to ~30s late (observed ~10s); 1s keeps the firing
 * within ~1s of the minute boundary, which is imperceptible for a wall-clock
 * timer. The per-poll cost is a cheap string compare over the rule set. */
#define TIME_CHECK_INTERVAL_MS 1000

#define MAX_IF_CONDITIONS 4
#define RULE_PARAM_MAX 32
#define RULE_OP_MAX 8

/* `then` is modelled as a sequence of up to MAX_STEPS "moments". Each step runs
 * its actions together, then (if not the last step and delay_after_sec > 0) waits
 * before the next step. A delay therefore separates steps, and a step may carry at
 * most one delay. Legacy flat `then` (a bare action array, possibly with inline
 * `delay` actions) is converted into this step form on parse (see parse_rule). */
#define MAX_STEPS 5
#define MAX_ACTIONS_PER_STEP 4

/* Pool of one-shot timers used to implement non-blocking delays. Each pending
 * delay occupies one slot; the callback only enqueues an execution job — it must
 * never run a command inline, because adapter command handlers perform blocking
 * I/O that would stall the esp_timer service task. */
#define RULE_TIMER_POOL 8
#define RULE_EXEC_QUEUE_LEN 16

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char parameter[RULE_PARAM_MAX];
    char op[RULE_OP_MAX];
    char equals[MINIHUB_VALUE_MAX]; /* threshold / expected value (also JSON "value") */
} if_condition_t;

typedef struct {
    char type[32];
    char point_id[MINIHUB_POINT_ID_MAX];
    char cmd[MINIHUB_CMD_MAX];
    char types[48];
    char message[128];
    int delay_sec;
    cJSON *params; /* optional command parameters (e.g. {"brightness": 50}); owned by the rule */
} then_action_t;

typedef struct {
    then_action_t actions[MAX_ACTIONS_PER_STEP];
    size_t action_count;
    int delay_after_sec; /* wait this long AFTER this step, before the next step; ignored on last step */
} rule_step_t;

typedef struct {
    char id[32];
    char name[64];
    bool enabled;
    char when_type[16];
    char point_id[MINIHUB_POINT_ID_MAX];
    char parameter[RULE_PARAM_MAX];
    char op[RULE_OP_MAX];
    char equals[MINIHUB_VALUE_MAX]; /* threshold / expected value (also JSON "value") */
    char time_at[8];
    if_condition_t if_cond[MAX_IF_CONDITIONS];
    size_t if_count;
    rule_step_t steps[MAX_STEPS];
    size_t step_count;
    int when_delay_sec; /* when.delay_sec: delay between trigger and first step */
    int last_fired_yday;
    char last_fired_at[8];
} rule_t;

EXT_RAM_BSS_ATTR static rule_t s_rules[MAX_RULES];
static size_t s_rule_count;
static uint8_t s_rule_buckets[RULE_BUCKETS][MAX_RULES_PER_BUCKET];
static uint8_t s_rule_bucket_len[RULE_BUCKETS];

/* Monotonic generation counter. Bumped on every load/reload so that delayed
 * executions armed against an older ruleset are recognised as stale and dropped,
 * preventing use-after-free of rule data after a reload. */
static uint32_t s_rules_gen;

typedef struct {
    uint32_t gen;
    int rule_idx;
    int step;
    int act_idx; /* action index within the step; 0 starts the step */
} exec_job_t;

typedef struct {
    esp_timer_handle_t handle;
    bool pending;
    uint32_t gen;
    int rule_idx;
    int step;
    int act_idx;
} rule_timer_slot_t;

static rule_timer_slot_t s_timers[RULE_TIMER_POOL];
static QueueHandle_t s_exec_queue;
static TaskHandle_t s_time_task;
static volatile bool s_time_task_run;
static TaskHandle_t s_exec_task;
static volatile bool s_exec_run;

static uint32_t hash_point_id(const char *point_id)
{
    uint32_t hash = 5381;
    for (const char *p = point_id; *p; p++) {
        hash = ((hash << 5) + hash) + (uint8_t)(*p);
    }
    return hash;
}

static void rebuild_rule_index(void)
{
    memset(s_rule_buckets, 0, sizeof(s_rule_buckets));
    memset(s_rule_bucket_len, 0, sizeof(s_rule_bucket_len));

    for (size_t i = 0; i < s_rule_count; i++) {
        const rule_t *rule = &s_rules[i];
        if (!rule->enabled || strcmp(rule->when_type, "point") != 0 || rule->point_id[0] == '\0') {
            continue;
        }
        uint32_t bucket = hash_point_id(rule->point_id) % RULE_BUCKETS;
        if (s_rule_bucket_len[bucket] >= MAX_RULES_PER_BUCKET) {
            ESP_LOGW(TAG, "rule bucket %u full, skip rule %s", (unsigned)bucket, rule->id);
            continue;
        }
        s_rule_buckets[bucket][s_rule_bucket_len[bucket]++] = (uint8_t)i;
    }
}

/** Normalize op tokens: "=", "eq", "==" → eq; "<="/lt; "≦"/"<="/le; etc. */
static void normalize_op(const char *raw, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw || !raw[0]) {
        strncpy(out, "eq", out_sz - 1);
        return;
    }
    if (strcmp(raw, "eq") == 0 || strcmp(raw, "=") == 0 || strcmp(raw, "==") == 0) {
        strncpy(out, "eq", out_sz - 1);
    } else if (strcmp(raw, "ne") == 0 || strcmp(raw, "!=") == 0 || strcmp(raw, "<>") == 0) {
        strncpy(out, "ne", out_sz - 1);
    } else if (strcmp(raw, "lt") == 0 || strcmp(raw, "<") == 0) {
        strncpy(out, "lt", out_sz - 1);
    } else if (strcmp(raw, "le") == 0 || strcmp(raw, "<=") == 0 || strcmp(raw, "≦") == 0) {
        strncpy(out, "le", out_sz - 1);
    } else if (strcmp(raw, "gt") == 0 || strcmp(raw, ">") == 0) {
        strncpy(out, "gt", out_sz - 1);
    } else if (strcmp(raw, "ge") == 0 || strcmp(raw, ">=") == 0 || strcmp(raw, "≧") == 0) {
        strncpy(out, "ge", out_sz - 1);
    } else {
        strncpy(out, "eq", out_sz - 1);
    }
    out[out_sz - 1] = '\0';
}

static bool json_value_to_str(const cJSON *v, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) {
        return false;
    }
    out[0] = '\0';
    if (!v) {
        return false;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        strncpy(out, v->valuestring, out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }
    if (cJSON_IsBool(v)) {
        strncpy(out, cJSON_IsTrue(v) ? "true" : "false", out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }
    if (cJSON_IsNumber(v)) {
        if (v->valuedouble == (double)v->valueint) {
            snprintf(out, out_sz, "%d", v->valueint);
        } else {
            snprintf(out, out_sz, "%g", v->valuedouble);
        }
        return true;
    }
    return false;
}

/**
 * Read live value for a parameter from props.params.<name>.
 * Entry may be a plain string/number or { "value": ... }.
 * Empty parameter → use point summary `value` (legacy equals-on-summary).
 */
static bool resolve_compare_lhs(const minihub_point_t *snap, const char *parameter,
                                char *out, size_t out_sz)
{
    if (!snap || !out || out_sz == 0) {
        return false;
    }
    if (!parameter || parameter[0] == '\0') {
        strncpy(out, snap->value, out_sz - 1);
        out[out_sz - 1] = '\0';
        return true;
    }
    if (!snap->props) {
        return false;
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(snap->props, "params");
    if (!cJSON_IsObject(params) && !cJSON_IsArray(params)) {
        return false;
    }
    const cJSON *ent = NULL;
    if (cJSON_IsObject(params)) {
        ent = cJSON_GetObjectItemCaseSensitive(params, parameter);
    } else {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, params) {
            const cJSON *n = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (cJSON_IsString(n) && n->valuestring && strcmp(n->valuestring, parameter) == 0) {
                ent = it;
                break;
            }
        }
    }
    if (!ent) {
        return false;
    }
    if (cJSON_IsObject(ent)) {
        return json_value_to_str(cJSON_GetObjectItemCaseSensitive(ent, "value"), out, out_sz);
    }
    return json_value_to_str(ent, out, out_sz);
}

static bool try_parse_number(const char *s, double *out)
{
    if (!s || !out) {
        return false;
    }
    /* Bool-ish tokens are not treated as numbers for ordering ops. */
    if (strcmp(s, "on") == 0 || strcmp(s, "off") == 0 ||
        strcmp(s, "true") == 0 || strcmp(s, "false") == 0 ||
        strcmp(s, "pressed") == 0 || strcmp(s, "released") == 0) {
        return false;
    }
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || (end && *end != '\0')) {
        return false;
    }
    *out = v;
    return true;
}

static bool values_equal_loose(const char *a, const char *b)
{
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    bool at = (strcmp(a, "on") == 0 || strcmp(a, "true") == 0 || strcmp(a, "1") == 0 ||
               strcmp(a, "ON") == 0);
    bool af = (strcmp(a, "off") == 0 || strcmp(a, "false") == 0 || strcmp(a, "0") == 0 ||
               strcmp(a, "OFF") == 0);
    bool bt = (strcmp(b, "on") == 0 || strcmp(b, "true") == 0 || strcmp(b, "1") == 0 ||
               strcmp(b, "ON") == 0);
    bool bf = (strcmp(b, "off") == 0 || strcmp(b, "false") == 0 || strcmp(b, "0") == 0 ||
               strcmp(b, "OFF") == 0);
    if ((at && bt) || (af && bf)) {
        return true;
    }
    double da, db;
    if (try_parse_number(a, &da) && try_parse_number(b, &db)) {
        return da == db;
    }
    return false;
}

static bool compare_values(const char *lhs, const char *op_raw, const char *rhs)
{
    char op[RULE_OP_MAX];
    normalize_op(op_raw, op, sizeof(op));
    if (!lhs) {
        lhs = "";
    }
    if (!rhs) {
        rhs = "";
    }

    if (strcmp(op, "eq") == 0) {
        return values_equal_loose(lhs, rhs);
    }
    if (strcmp(op, "ne") == 0) {
        return !values_equal_loose(lhs, rhs);
    }

    double dl, dr;
    bool nl = try_parse_number(lhs, &dl);
    bool nr = try_parse_number(rhs, &dr);
    if (nl && nr) {
        if (strcmp(op, "lt") == 0) {
            return dl < dr;
        }
        if (strcmp(op, "le") == 0) {
            return dl <= dr;
        }
        if (strcmp(op, "gt") == 0) {
            return dl > dr;
        }
        if (strcmp(op, "ge") == 0) {
            return dl >= dr;
        }
    }
    /* Non-numeric ordering: lexicographic fallback. */
    int cmp = strcmp(lhs, rhs);
    if (strcmp(op, "lt") == 0) {
        return cmp < 0;
    }
    if (strcmp(op, "le") == 0) {
        return cmp <= 0;
    }
    if (strcmp(op, "gt") == 0) {
        return cmp > 0;
    }
    if (strcmp(op, "ge") == 0) {
        return cmp >= 0;
    }
    return false;
}

/** Evaluate point_id + optional parameter against op/expected using a live snapshot. */
static bool condition_matches_snapshot(const char *point_id, const char *parameter,
                                       const char *op, const char *expected)
{
    if (!point_id || point_id[0] == '\0') {
        return false;
    }
    minihub_point_t snap = {0};
    if (minihub_point_get_snapshot(point_id, &snap) != ESP_OK) {
        return false;
    }
    char lhs[MINIHUB_VALUE_MAX];
    bool ok = resolve_compare_lhs(&snap, parameter, lhs, sizeof(lhs));
    if (snap.props) {
        cJSON_Delete(snap.props);
    }
    if (!ok) {
        return false;
    }
    return compare_values(lhs, op, expected);
}

static bool if_conditions_match(const rule_t *rule)
{
    for (size_t i = 0; i < rule->if_count; i++) {
        const if_condition_t *ic = &rule->if_cond[i];
        if (!condition_matches_snapshot(ic->point_id, ic->parameter, ic->op, ic->equals)) {
            return false;
        }
    }
    return true;
}

esp_err_t minihub_im_notify_text(const char *text) __attribute__((weak));

static bool type_in_csv(const char *csv, const char *type)
{
    if (!csv || !csv[0] || !type || !type[0]) {
        return false;
    }
    const char *p = csv;
    size_t tlen = strlen(type);
    while (*p) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        if (strncmp(p, type, tlen) == 0 && (p[tlen] == '\0' || p[tlen] == ',' || p[tlen] == ' ')) {
            return true;
        }
        while (*p && *p != ',') {
            p++;
        }
    }
    return false;
}

static bool str_has_token(const char *hay, const char *needle)
{
    return hay && needle && hay[0] && strstr(hay, needle) != NULL;
}

/** Lights and real plugs only — skip mock, meters, PIR, remotes, cameras. */
static bool point_is_light_or_plug(const minihub_point_t *p)
{
    if (!p || p->adapter[0] == '\0' || strcmp(p->adapter, "mock") == 0) {
        return false;
    }
    if (str_has_token(p->point_id, "pir") || str_has_token(p->point_id, "motion")) {
        return false;
    }
    if (strcmp(p->type, "light") == 0) {
        return true;
    }
    if (strcmp(p->type, "switch") != 0) {
        return false;
    }
    if (p->props) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(p->props, "no_control"))) {
            return false;
        }
        const cJSON *model = cJSON_GetObjectItem(p->props, "model");
        const char *ms = cJSON_IsString(model) ? model->valuestring : "";
        if (str_has_token(ms, "motion") || str_has_token(ms, "pir") ||
            str_has_token(ms, "remote") || str_has_token(ms, "magnet") ||
            str_has_token(ms, "sensor")) {
            return false;
        }
        const cJSON *name = cJSON_GetObjectItem(p->props, "name");
        const char *ns = cJSON_IsString(name) ? name->valuestring : "";
        if (str_has_token(ns, "人体") || str_has_token(ns, "motion") ||
            str_has_token(ns, "遥控")) {
            return false;
        }
    }
    return true;
}

static void execute_command_by_type(const rule_t *rule, const then_action_t *act)
{
    minihub_point_t *points = NULL;
    size_t count = 0;
    const char *types = act->types[0] ? act->types : "light,switch";
    if (minihub_point_list(&points, &count) != ESP_OK || !points) {
        ESP_LOGW(TAG, "rule '%s' command_by_type: list failed", rule->id);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (!type_in_csv(types, points[i].type) || !point_is_light_or_plug(&points[i])) {
            continue;
        }
        minihub_command_t cmd = {0};
        strncpy(cmd.point_id, points[i].point_id, sizeof(cmd.point_id) - 1);
        strncpy(cmd.cmd, act->cmd, sizeof(cmd.cmd) - 1);
        esp_err_t err = minihub_command_invoke(&cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rule '%s' %s %s failed: %s", rule->id, cmd.cmd, cmd.point_id,
                     esp_err_to_name(err));
        } else {
            n++;
            ESP_LOGI(TAG, "rule '%s' executed: %s -> %s", rule->name, cmd.point_id, cmd.cmd);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (points[i].props) {
            cJSON_Delete(points[i].props);
            points[i].props = NULL;
        }
    }
    free(points);
    if (n == 0) {
        ESP_LOGW(TAG, "rule '%s' command_by_type matched 0 lights/plugs", rule->id);
    }
}

static void execute_one_then(const rule_t *rule, const then_action_t *act,
                             const char *point_id, const char *new_value)
{
    if (!act || act->type[0] == '\0') {
        return;
    }

    if (strcmp(act->type, "notify") == 0) {
        char msg[160];
        if (act->message[0]) {
            snprintf(msg, sizeof(msg), "%s", act->message);
        } else {
            snprintf(msg, sizeof(msg), "Hub %s → %s",
                     point_id ? point_id : "?", new_value ? new_value : "?");
        }
        if (minihub_im_notify_text) {
            esp_err_t nerr = minihub_im_notify_text(msg);
            if (nerr != ESP_OK) {
                ESP_LOGW(TAG, "rule '%s' notify failed: %s", rule->id, esp_err_to_name(nerr));
            } else {
                ESP_LOGI(TAG, "rule '%s' notified IM", rule->name);
            }
        } else {
            ESP_LOGW(TAG, "rule '%s' then=notify but IM notify is not linked", rule->id);
        }
        return;
    }

    if (strcmp(act->type, "command_by_type") == 0) {
        execute_command_by_type(rule, act);
        return;
    }

    if (strcmp(act->type, "command") != 0) {
        ESP_LOGW(TAG, "rule '%s' unknown then.type='%s'", rule->id, act->type);
        return;
    }

    minihub_command_t cmd = {0};
    strncpy(cmd.point_id, act->point_id, sizeof(cmd.point_id) - 1);
    strncpy(cmd.cmd, act->cmd, sizeof(cmd.cmd) - 1);
    cmd.params = act->params;
    esp_err_t err = minihub_command_invoke(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rule '%s' command failed: %s", rule->id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "rule '%s' executed: %s -> %s", rule->name, act->point_id, act->cmd);
    }
}

/* Forward declarations so the sequential executor can schedule the next action
 * via the timer pool defined just below. */
static void arm_step(int rule_idx, int step, int act_idx, uint32_t gen, int delay_sec);
static void execute_step_continue(int rule_idx, int step, int act_idx, uint32_t gen);

/* Run a step's actions in order. A `delay` action waits (non-blocking, via
 * arm_step) before the next action; when the last action is done, delay_after
 * advances to the next step. Each continuation is re-checked against the
 * generation counter by the executor task, so a reload discards stale steps. */
static void execute_step_continue(int rule_idx, int step, int act_idx, uint32_t gen)
{
    if (rule_idx < 0 || (size_t)rule_idx >= s_rule_count) {
        return;
    }
    if (step < 0 || (size_t)step >= s_rules[rule_idx].step_count) {
        return;
    }
    rule_t *rule = &s_rules[rule_idx];
    rule_step_t *st = &rule->steps[step];
    if (act_idx >= (int)st->action_count) {
        if ((size_t)(step + 1) < rule->step_count) {
            arm_step(rule_idx, step + 1, 0, gen, st->delay_after_sec);
        }
        return;
    }
    then_action_t *act = &st->actions[act_idx];
    if (strcmp(act->type, "delay") == 0) {
        if (act->delay_sec > 0) {
            arm_step(rule_idx, step, act_idx + 1, gen, act->delay_sec);
        } else {
            execute_step_continue(rule_idx, step, act_idx + 1, gen);
        }
        return;
    }
    execute_one_then(rule, act, rule->point_id, rule->equals);
    execute_step_continue(rule_idx, step, act_idx + 1, gen);
}

/** Enqueue (or, for a positive delay, schedule via a one-shot timer) the
 * execution of a rule step. The timer callback never runs commands itself — it
 * only pushes a job onto the executor queue. */
static void arm_step(int rule_idx, int step, int act_idx, uint32_t gen, int delay_sec)
{
    if (rule_idx < 0 || (size_t)rule_idx >= s_rule_count) {
        return;
    }
    exec_job_t job = { .gen = gen, .rule_idx = rule_idx, .step = step, .act_idx = act_idx };
    if (delay_sec <= 0) {
        if (s_exec_queue) {
            xQueueSend(s_exec_queue, &job, 0);
        }
        return;
    }
    for (int i = 0; i < RULE_TIMER_POOL; i++) {
        if (!s_timers[i].handle || s_timers[i].pending) {
            continue;
        }
        s_timers[i].pending = true;
        s_timers[i].gen = gen;
        s_timers[i].rule_idx = rule_idx;
        s_timers[i].step = step;
        s_timers[i].act_idx = act_idx;
        esp_timer_stop(s_timers[i].handle);
        esp_timer_start_once(s_timers[i].handle, (uint64_t)delay_sec * 1000000ULL);
        return;
    }
    ESP_LOGW(TAG, "rule timer pool exhausted, running step without delay");
    if (s_exec_queue) {
        xQueueSend(s_exec_queue, &job, 0);
    }
}

static void rule_timer_cb(void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= RULE_TIMER_POOL) {
        return;
    }
    rule_timer_slot_t *slot = &s_timers[idx];
    if (!slot->pending) {
        return;
    }
    slot->pending = false;
    exec_job_t job = { .gen = slot->gen, .rule_idx = slot->rule_idx, .step = slot->step, .act_idx = slot->act_idx };
    if (s_exec_queue) {
        xQueueSend(s_exec_queue, &job, 0);
    }
}

static void rules_exec_task(void *arg)
{
    (void)arg;
    exec_job_t job;
    for (;;) {
        /* Poll with a timeout so the task can observe s_exec_run and exit on stop. */
        if (xQueueReceive(s_exec_queue, &job, pdMS_TO_TICKS(200)) == pdTRUE) {
            /* Stale generation: the ruleset was reloaded after this job was armed. */
            if (job.gen != s_rules_gen) {
                continue;
            }
            if (job.rule_idx < 0 || (size_t)job.rule_idx >= s_rule_count) {
                continue;
            }
            rule_t *rule = &s_rules[job.rule_idx];
            if (!rule->enabled) {
                continue;
            }
            if (job.step < 0 || (size_t)job.step >= rule->step_count) {
                continue;
            }
            execute_step_continue(job.rule_idx, job.step, job.act_idx, job.gen);
        }
        if (!s_exec_run) {
            break;
        }
    }
    s_exec_task = NULL;
    vTaskDelete(NULL);
}


static void schedule_rule(size_t rule_idx)
{
    if (rule_idx >= s_rule_count) {
        return;
    }
    arm_step((int)rule_idx, 0, 0, s_rules_gen, s_rules[rule_idx].when_delay_sec);
}

/* Edge detection: a point-triggered rule should fire on a *change* of its
 * parameter value, not on every redundant point.updated (e.g. after a restart
 * or a device re-sync that re-publishes the current state). Cache the last seen
 * value per (point_id, parameter); on an unchanged value we skip the rule. */
#define EDGE_CACHE_MAX 256
typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char parameter[RULE_PARAM_MAX];
    char value[MINIHUB_VALUE_MAX];
} edge_entry_t;
static edge_entry_t s_edge[EDGE_CACHE_MAX];
static size_t s_edge_count = 0;

static const char *edge_lookup(const char *point_id, const char *parameter)
{
    const char *key_param = parameter ? parameter : "";
    for (size_t i = 0; i < s_edge_count; i++) {
        if (strcmp(s_edge[i].point_id, point_id) == 0 &&
            strcmp(s_edge[i].parameter, key_param) == 0) {
            return s_edge[i].value;
        }
    }
    return NULL;
}

static void edge_store(const char *point_id, const char *parameter, const char *value)
{
    const char *key_param = parameter ? parameter : "";
    for (size_t i = 0; i < s_edge_count; i++) {
        if (strcmp(s_edge[i].point_id, point_id) == 0 &&
            strcmp(s_edge[i].parameter, key_param) == 0) {
            strncpy(s_edge[i].value, value ? value : "", sizeof(s_edge[i].value) - 1);
            s_edge[i].value[sizeof(s_edge[i].value) - 1] = '\0';
            return;
        }
    }
    if (s_edge_count >= EDGE_CACHE_MAX) {
        return; /* cache full: this entry may re-fire once on next change */
    }
    strncpy(s_edge[s_edge_count].point_id, point_id, sizeof(s_edge[s_edge_count].point_id) - 1);
    strncpy(s_edge[s_edge_count].parameter, key_param, sizeof(s_edge[s_edge_count].parameter) - 1);
    strncpy(s_edge[s_edge_count].value, value ? value : "", sizeof(s_edge[s_edge_count].value) - 1);
    s_edge[s_edge_count].point_id[sizeof(s_edge[s_edge_count].point_id) - 1] = '\0';
    s_edge[s_edge_count].parameter[sizeof(s_edge[s_edge_count].parameter) - 1] = '\0';
    s_edge[s_edge_count].value[sizeof(s_edge[s_edge_count].value) - 1] = '\0';
    s_edge_count++;
}

/** Seed the edge cache from the current point values so a freshly booted hub
 *  does not immediately re-fire rules whose state already matches. */
static void seed_edge_cache(void)
{
    s_edge_count = 0;
    for (size_t i = 0; i < s_rule_count; i++) {
        const rule_t *rule = &s_rules[i];
        if (rule->point_id[0] == '\0') {
            continue;
        }
        const char *param = rule->parameter[0] ? rule->parameter : NULL;
        char lhs[MINIHUB_VALUE_MAX];
        bool have = false;
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(rule->point_id, &snap) == ESP_OK) {
            if (param) {
                have = resolve_compare_lhs(&snap, param, lhs, sizeof(lhs));
            } else {
                strncpy(lhs, snap.value, sizeof(lhs) - 1);
                lhs[sizeof(lhs) - 1] = '\0';
                have = true;
            }
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
        }
        /* Only seed with a concrete value. Seeding an empty/"not-yet-synced"
         * value plants a wrong baseline, so the first real data sync (which
         * carries the actual value) would look like an edge and fire the rule.
         * Points that have not synced yet stay unseeded and become baseline on
         * their first observed sync instead (see match_rules_for_point). */
        if (have && lhs[0] != '\0') {
            edge_store(rule->point_id, param, lhs);
        }
    }
}

static void match_rules_for_point(const char *point_id, const char *new_value)
{
    if (!point_id) {
        return;
    }
    uint32_t bucket = hash_point_id(point_id) % RULE_BUCKETS;
    char lhs[MINIHUB_VALUE_MAX];
    for (uint8_t i = 0; i < s_rule_bucket_len[bucket]; i++) {
        uint8_t ridx = s_rule_buckets[bucket][i];
        rule_t *rule = &s_rules[ridx];
        if (strcmp(rule->point_id, point_id) != 0) {
            continue;
        }

        /* Resolve the rule's trigger parameter to its current value. */
        const char *param = rule->parameter[0] ? rule->parameter : NULL;
        bool have_lhs = false;
        if (param) {
            minihub_point_t snap = {0};
            if (minihub_point_get_snapshot(point_id, &snap) == ESP_OK) {
                have_lhs = resolve_compare_lhs(&snap, param, lhs, sizeof(lhs));
                if (snap.props) {
                    cJSON_Delete(snap.props);
                }
            }
        } else {
            strncpy(lhs, new_value ? new_value : "", sizeof(lhs) - 1);
            lhs[sizeof(lhs) - 1] = '\0';
            have_lhs = true;
        }
        if (!have_lhs) {
            continue;
        }

        /* Edge trigger: the very first observed value of a (point, parameter)
         * pair is the baseline established by the initial data sync. We record
         * it but do not fire the rule, so a freshly booted / re-synced hub does
         * not trigger rules just because the current state was published.
         * Only a *change* since that baseline (or since the last published value)
         * actually fires the rule. */
        const char *prev = edge_lookup(point_id, param);
        if (prev == NULL) {
            edge_store(point_id, param, lhs);
            continue; /* first sync -> establish baseline, no trigger */
        }
        if (strcmp(prev, lhs) == 0) {
            continue; /* value unchanged -> not an edge */
        }
        edge_store(point_id, param, lhs);

        bool matched = compare_values(lhs, rule->op, rule->equals);
        if (matched && if_conditions_match(rule)) {
            schedule_rule(ridx);
        }
    }
}

static bool clock_is_valid(void)
{
    time_t now = time(NULL);
    if (now < 0) {
        return false;
    }
    /* Time rules are stored as UTC wall clock. libc TZ is the display
     * timezone, so this check must stay on gmtime. */
    struct tm local_tm;
    if (!gmtime_r(&now, &local_tm)) {
        return false;
    }
    /* Accept any clock set to a plausible year. We deliberately do not require
     * SNTP sync to be *completed*: on LAN-only setups (no reachable NTP) the
     * sync status stays RESET and the old hard gate left time rules dead, so
     * scheduled rules never fired. With a valid local clock the HH:MM compare
     * simply won't match a 1970 timestamp, so relaxing this is safe. */
    return (local_tm.tm_year + 1900) >= 2020;
}

static void check_time_rules(void)
{
    if (!clock_is_valid()) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_tm;
    if (!gmtime_r(&now, &local_tm)) {
        return;
    }

    char now_hm[8];
    snprintf(now_hm, sizeof(now_hm), "%02d:%02d", local_tm.tm_hour, local_tm.tm_min);

    for (size_t i = 0; i < s_rule_count; i++) {
        rule_t *rule = &s_rules[i];
        if (!rule->enabled || strcmp(rule->when_type, "time") != 0 || rule->time_at[0] == '\0') {
            continue;
        }
        if (strcmp(rule->time_at, now_hm) != 0) {
            continue;
        }
        if (rule->last_fired_yday == local_tm.tm_yday && strcmp(rule->last_fired_at, now_hm) == 0) {
            continue;
        }

        rule->last_fired_yday = local_tm.tm_yday;
        strncpy(rule->last_fired_at, now_hm, sizeof(rule->last_fired_at) - 1);
        ESP_LOGI(TAG, "time rule '%s' fired at %s", rule->name, now_hm);
        if (if_conditions_match(rule)) {
            schedule_rule(i);
        }
    }
}

static void time_rule_task(void *arg)
{
    (void)arg;
    while (s_time_task_run) {
        check_time_rules();
        vTaskDelay(pdMS_TO_TICKS(TIME_CHECK_INTERVAL_MS));
    }
    s_time_task = NULL;
    vTaskDelete(NULL);
}

static void on_point_updated(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) {
        return;
    }

    if (event->data) {
        const cJSON *point_id = cJSON_GetObjectItem(event->data, "point_id");
        const cJSON *new_value = cJSON_GetObjectItem(event->data, "new_value");
        if (!cJSON_IsString(point_id) || !cJSON_IsString(new_value)) {
            return;
        }
        match_rules_for_point(point_id->valuestring, new_value->valuestring);
        return;
    }

    minihub_point_t *point = minihub_point_get_by_index(event->point_index);
    if (!point) {
        return;
    }
    match_rules_for_point(point->point_id, event->new_value);
}

static void parse_then_action(const cJSON *el, then_action_t *act)
{
    memset(act, 0, sizeof(*act));
    const cJSON *type = cJSON_GetObjectItem(el, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        strncpy(act->type, type->valuestring, sizeof(act->type) - 1);
    }
    const cJSON *pid = cJSON_GetObjectItem(el, "point_id");
    if (cJSON_IsString(pid) && pid->valuestring) {
        strncpy(act->point_id, pid->valuestring, sizeof(act->point_id) - 1);
    }
    const cJSON *cmd = cJSON_GetObjectItem(el, "cmd");
    if (cJSON_IsString(cmd) && cmd->valuestring) {
        strncpy(act->cmd, cmd->valuestring, sizeof(act->cmd) - 1);
    }
    const cJSON *params = cJSON_GetObjectItem(el, "params");
    if (cJSON_IsObject(params)) {
        act->params = cJSON_Duplicate(params, 1);
    }
    const cJSON *delay = cJSON_GetObjectItem(el, "delay_sec");
    if (!cJSON_IsNumber(delay)) {
        delay = cJSON_GetObjectItem(el, "seconds");
    }
    if (cJSON_IsNumber(delay)) {
        act->delay_sec = delay->valueint;
    }
    const cJSON *msg = cJSON_GetObjectItem(el, "message");
    if (cJSON_IsString(msg) && msg->valuestring) {
        strncpy(act->message, msg->valuestring, sizeof(act->message) - 1);
    }
    const cJSON *types = cJSON_GetObjectItem(el, "types");
    if (cJSON_IsString(types) && types->valuestring) {
        strncpy(act->types, types->valuestring, sizeof(act->types) - 1);
    }
}

/** Parse a `then` value (legacy flat action array / single action, or the new
 * steps array) into the rule's step sequence. */
static void parse_then(const cJSON *then, rule_t *rule)
{
    if (!cJSON_IsArray(then) && !cJSON_IsObject(then)) {
        rule->step_count = 1; /* empty placeholder step */
        return;
    }

    /* New steps form: an array whose first element has an "actions" key. */
    bool is_steps = false;
    if (cJSON_IsArray(then) && cJSON_GetArraySize(then) > 0) {
        const cJSON *first = cJSON_GetArrayItem(then, 0);
        if (cJSON_IsObject(first) && cJSON_GetObjectItem(first, "actions")) {
            is_steps = true;
        }
    }

    if (is_steps) {
        size_t n = (size_t)cJSON_GetArraySize(then);
        if (n > MAX_STEPS) {
            ESP_LOGW(TAG, "rule '%s' then steps truncated to %d", rule->id, MAX_STEPS);
            n = MAX_STEPS;
        }
        for (size_t s = 0; s < n; s++) {
            const cJSON *step = cJSON_GetArrayItem(then, (int)s);
            rule_step_t *st = &rule->steps[rule->step_count];
            const cJSON *acts = cJSON_GetObjectItem(step, "actions");
            if (cJSON_IsArray(acts)) {
                size_t an = (size_t)cJSON_GetArraySize(acts);
                if (an > MAX_ACTIONS_PER_STEP) {
                    an = MAX_ACTIONS_PER_STEP;
                }
                for (size_t a = 0; a < an; a++) {
                    parse_then_action(cJSON_GetArrayItem(acts, (int)a), &st->actions[st->action_count]);
                    st->action_count++;
                }
            }
            const cJSON *da = cJSON_GetObjectItem(step, "delay_after");
            if (!cJSON_IsNumber(da)) {
                da = cJSON_GetObjectItem(step, "delay");
            }
            if (cJSON_IsNumber(da)) {
                st->delay_after_sec = da->valueint;
            }
            rule->step_count++;
        }
        return;
    }

    /* Legacy flat form: a single action object or an array of action objects,
     * where a `delay` action becomes the preceding step's delay_after and opens
     * a new step for the following actions. */
    if (cJSON_IsObject(then)) {
        then_action_t act;
        parse_then_action(then, &act);
        rule->steps[0].actions[0] = act;
        rule->steps[0].action_count = 1;
        rule->step_count = 1;
        return;
    }

    cJSON *el = NULL;
    size_t cur = 0;
    cJSON_ArrayForEach(el, then) {
        if (!cJSON_IsObject(el)) {
            continue;
        }
        then_action_t act;
        parse_then_action(el, &act);
        if (strcmp(act.type, "delay") == 0) {
            if (rule->step_count <= cur) {
                if (rule->step_count < MAX_STEPS) {
                    rule->step_count++;
                } else {
                    continue;
                }
            }
            rule->steps[cur].delay_after_sec = act.delay_sec;
            cur++;
            if (cur >= MAX_STEPS) {
                cur = MAX_STEPS - 1;
            }
        } else {
            if (rule->step_count <= cur) {
                if (rule->step_count < MAX_STEPS) {
                    rule->step_count++;
                } else {
                    continue;
                }
            }
            rule_step_t *st = &rule->steps[cur];
            if (st->action_count < MAX_ACTIONS_PER_STEP) {
                st->actions[st->action_count++] = act;
            } else {
                ESP_LOGW(TAG, "rule '%s' step actions truncated", rule->id);
            }
        }
    }
    if (rule->step_count == 0) {
        rule->step_count = 1;
    }
}

static void parse_rule(const cJSON *item, rule_t *rule)
{
    memset(rule, 0, sizeof(*rule));
    rule->last_fired_yday = -1;
    rule->step_count = 0; /* parse_then fills steps; guarantees >=1 on success */
    const cJSON *id = cJSON_GetObjectItem(item, "id");
    const cJSON *name = cJSON_GetObjectItem(item, "name");
    const cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
    const cJSON *when = cJSON_GetObjectItem(item, "when");
    const cJSON *if_arr = cJSON_GetObjectItem(item, "if");
    const cJSON *then = cJSON_GetObjectItem(item, "then");

    if (cJSON_IsString(id)) {
        strncpy(rule->id, id->valuestring, sizeof(rule->id) - 1);
    }
    if (cJSON_IsString(name)) {
        strncpy(rule->name, name->valuestring, sizeof(rule->name) - 1);
    }
    rule->enabled = !cJSON_IsBool(enabled) || cJSON_IsTrue(enabled);

    if (cJSON_IsObject(when)) {
        const cJSON *type = cJSON_GetObjectItem(when, "type");
        if (cJSON_IsString(type)) {
            strncpy(rule->when_type, type->valuestring, sizeof(rule->when_type) - 1);
        }
        const cJSON *pid = cJSON_GetObjectItem(when, "point_id");
        if (cJSON_IsString(pid)) {
            strncpy(rule->point_id, pid->valuestring, sizeof(rule->point_id) - 1);
        }
        const cJSON *param = cJSON_GetObjectItem(when, "parameter");
        if (cJSON_IsString(param) && param->valuestring) {
            strncpy(rule->parameter, param->valuestring, sizeof(rule->parameter) - 1);
        }
        const cJSON *op = cJSON_GetObjectItem(when, "op");
        if (cJSON_IsString(op) && op->valuestring) {
            normalize_op(op->valuestring, rule->op, sizeof(rule->op));
        } else {
            normalize_op("eq", rule->op, sizeof(rule->op));
        }
        /* Prefer "value"; fall back to legacy "equals". */
        const cJSON *val = cJSON_GetObjectItem(when, "value");
        if (!cJSON_IsString(val) || !val->valuestring) {
            val = cJSON_GetObjectItem(when, "equals");
        }
        if (cJSON_IsString(val) && val->valuestring) {
            strncpy(rule->equals, val->valuestring, sizeof(rule->equals) - 1);
        } else if (cJSON_IsNumber(val) || cJSON_IsBool(val)) {
            json_value_to_str(val, rule->equals, sizeof(rule->equals));
        }
        const cJSON *at = cJSON_GetObjectItem(when, "at");
        if (cJSON_IsString(at)) {
            strncpy(rule->time_at, at->valuestring, sizeof(rule->time_at) - 1);
        }
        /* when.delay_sec: delay between trigger and first step. */
        const cJSON *wd = cJSON_GetObjectItem(when, "delay_sec");
        if (!cJSON_IsNumber(wd)) {
            wd = cJSON_GetObjectItem(when, "delay");
        }
        if (cJSON_IsNumber(wd)) {
            rule->when_delay_sec = wd->valueint;
        }
    }

    if (cJSON_IsArray(if_arr)) {
        cJSON *cond = NULL;
        cJSON_ArrayForEach(cond, if_arr) {
            if (rule->if_count >= MAX_IF_CONDITIONS) {
                break;
            }
            if_condition_t *ic = &rule->if_cond[rule->if_count];
            const cJSON *pid = cJSON_GetObjectItem(cond, "point_id");
            const cJSON *param = cJSON_GetObjectItem(cond, "parameter");
            const cJSON *op = cJSON_GetObjectItem(cond, "op");
            const cJSON *val = cJSON_GetObjectItem(cond, "value");
            if (!cJSON_IsString(val) || !val->valuestring) {
                val = cJSON_GetObjectItem(cond, "equals");
            }
            if (cJSON_IsString(pid)) {
                strncpy(ic->point_id, pid->valuestring, sizeof(ic->point_id) - 1);
            }
            if (cJSON_IsString(param) && param->valuestring) {
                strncpy(ic->parameter, param->valuestring, sizeof(ic->parameter) - 1);
            }
            if (cJSON_IsString(op) && op->valuestring) {
                normalize_op(op->valuestring, ic->op, sizeof(ic->op));
            } else {
                normalize_op("eq", ic->op, sizeof(ic->op));
            }
            if (cJSON_IsString(val) && val->valuestring) {
                strncpy(ic->equals, val->valuestring, sizeof(ic->equals) - 1);
            } else if (cJSON_IsNumber(val) || cJSON_IsBool(val)) {
                json_value_to_str(val, ic->equals, sizeof(ic->equals));
            }
            rule->if_count++;
        }
    }

    parse_then(then, rule);
}

static esp_err_t ensure_timers(void)
{
    if (s_timers[0].handle) {
        return ESP_OK;
    }
    s_exec_queue = xQueueCreate(RULE_EXEC_QUEUE_LEN, sizeof(exec_job_t));
    if (!s_exec_queue) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < RULE_TIMER_POOL; i++) {
        esp_timer_handle_t h = NULL;
        const esp_timer_create_args_t args = {
            .callback = &rule_timer_cb,
            .arg = (void *)(intptr_t)i,
            .name = "rule_step"
        };
        if (esp_timer_create(&args, &h) != ESP_OK) {
            return ESP_ERR_NO_MEM;
        }
        s_timers[i].handle = h;
        s_timers[i].pending = false;
        s_timers[i].gen = 0;
        s_timers[i].rule_idx = -1;
        s_timers[i].step = -1;
    }
    return ESP_OK;
}

static esp_err_t start_time_task(void)
{
    if (s_time_task != NULL) {
        return ESP_OK;
    }
    s_time_task_run = true;
    BaseType_t ok = minihub_task_create_psram(time_rule_task, "rules_time", 3072, NULL, 4, &s_time_task);
    if (ok != pdPASS) {
        s_time_task_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t start_exec_task(void)
{
    if (s_exec_task != NULL) {
        return ESP_OK;
    }
    s_exec_run = true;
    BaseType_t ok = minihub_task_create_psram(rules_exec_task, "rules_exec", 4096, NULL, 3, &s_exec_task);
    if (ok != pdPASS) {
        s_exec_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t load_rules(const cJSON *rules_cfg)
{
    /* Invalidate any delayed executions from the previous generation: bump the
     * generation and stop all pending timers. Jobs that were already queued or
     * fire afterwards carry the old generation and are dropped by the executor. */
    s_rules_gen++;
    for (int i = 0; i < RULE_TIMER_POOL; i++) {
        if (s_timers[i].handle) {
            esp_timer_stop(s_timers[i].handle);
        }
        s_timers[i].pending = false;
    }

    for (size_t i = 0; i < s_rule_count; i++) {
        for (size_t s = 0; s < MAX_STEPS; s++) {
            for (size_t k = 0; k < MAX_ACTIONS_PER_STEP; k++) {
                if (s_rules[i].steps[s].actions[k].params) {
                    cJSON_Delete(s_rules[i].steps[s].actions[k].params);
                    s_rules[i].steps[s].actions[k].params = NULL;
                }
            }
        }
    }
    s_rule_count = 0;

    if (!rules_cfg) {
        rebuild_rule_index();
        return ESP_OK;
    }

    const cJSON *rules = cJSON_GetObjectItem(rules_cfg, "rules");
    if (!cJSON_IsArray(rules)) {
        rebuild_rule_index();
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, rules) {
        if (s_rule_count >= MAX_RULES) {
            break;
        }
        parse_rule(item, &s_rules[s_rule_count]);
        ESP_LOGI(TAG, "loaded rule '%s' (%s), %u steps", s_rules[s_rule_count].name,
                 s_rules[s_rule_count].id, (unsigned)s_rules[s_rule_count].step_count);
        if (s_rules[s_rule_count].enabled &&
            strcmp(s_rules[s_rule_count].when_type, "delay") == 0) {
            /* Delay trigger: fire once this many seconds after the ruleset loads
             * (re-arms on each import). Carries the fresh generation so a later
             * reload invalidates it. */
            arm_step((int)s_rule_count, 0, 0, s_rules_gen, s_rules[s_rule_count].when_delay_sec);
        }
        s_rule_count++;
    }
    rebuild_rule_index();
    seed_edge_cache();
    return ESP_OK;
}

esp_err_t minihub_rules_init(const cJSON *rules_cfg)
{
    esp_err_t err = ensure_timers();
    if (err != ESP_OK) {
        return err;
    }
    err = load_rules(rules_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.updated", on_point_updated, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = start_time_task();
    if (err != ESP_OK) {
        return err;
    }
    return start_exec_task();
}

esp_err_t minihub_rules_reload(const cJSON *rules_cfg)
{
    for (size_t i = 0; i < s_rule_count; i++) {
        s_rules[i].last_fired_yday = -1;
        s_rules[i].last_fired_at[0] = '\0';
    }
    return load_rules(rules_cfg);
}

esp_err_t minihub_rules_stop(void)
{
    s_time_task_run = false;
    s_exec_run = false;
    for (int i = 0; i < RULE_TIMER_POOL; i++) {
        if (s_timers[i].handle) {
            esp_timer_stop(s_timers[i].handle);
        }
        s_timers[i].pending = false;
    }
    if (s_exec_task != NULL) {
        /* Allow the executor to observe s_exec_run and exit (it polls every 200ms). */
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    return ESP_OK;
}
