/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "minihub_core.h"
#include "minihub_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "minihub_rules";

#define MAX_RULES 16
#define RULE_BUCKETS 16
#define MAX_RULES_PER_BUCKET 8
#define TIME_CHECK_INTERVAL_MS 30000

#define MAX_IF_CONDITIONS 4
#define MAX_THEN_ACTIONS 4

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char equals[MINIHUB_VALUE_MAX];
} if_condition_t;

typedef struct {
    char type[32];
    char point_id[MINIHUB_POINT_ID_MAX];
    char cmd[MINIHUB_CMD_MAX];
    char types[48];
    char message[128];
    int delay_sec;
} then_action_t;

typedef struct {
    char id[32];
    char name[64];
    bool enabled;
    char when_type[16];
    char point_id[MINIHUB_POINT_ID_MAX];
    char equals[MINIHUB_VALUE_MAX];
    char time_at[8];
    if_condition_t if_cond[MAX_IF_CONDITIONS];
    size_t if_count;
    then_action_t then[MAX_THEN_ACTIONS];
    size_t then_count;
    int last_fired_yday;
    char last_fired_at[8];
} rule_t;

EXT_RAM_BSS_ATTR static rule_t s_rules[MAX_RULES];
static size_t s_rule_count;
static uint8_t s_rule_buckets[RULE_BUCKETS][MAX_RULES_PER_BUCKET];
static uint8_t s_rule_bucket_len[RULE_BUCKETS];
static TaskHandle_t s_time_task;
static volatile bool s_time_task_run;

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

static bool if_conditions_match(const rule_t *rule)
{
    for (size_t i = 0; i < rule->if_count; i++) {
        minihub_point_t snap = {0};
        if (minihub_point_get_snapshot(rule->if_cond[i].point_id, &snap) != ESP_OK) {
            return false;
        }
        if (strcmp(snap.value, rule->if_cond[i].equals) != 0) {
            if (snap.props) {
                cJSON_Delete(snap.props);
            }
            return false;
        }
        if (snap.props) {
            cJSON_Delete(snap.props);
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

    if (strcmp(act->type, "delay") == 0) {
        vTaskDelay(pdMS_TO_TICKS(act->delay_sec * 1000));
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
    esp_err_t err = minihub_command_invoke(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rule '%s' command failed: %s", rule->id, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "rule '%s' executed: %s -> %s", rule->name, act->point_id, act->cmd);
    }
}

static void execute_then(const rule_t *rule, const char *point_id, const char *new_value)
{
    for (size_t i = 0; i < rule->then_count; i++) {
        execute_one_then(rule, &rule->then[i], point_id, new_value);
    }
}

static void match_rules_for_point(const char *point_id, const char *new_value)
{
    uint32_t bucket = hash_point_id(point_id) % RULE_BUCKETS;
    for (uint8_t i = 0; i < s_rule_bucket_len[bucket]; i++) {
        const rule_t *rule = &s_rules[s_rule_buckets[bucket][i]];
        if (strcmp(rule->point_id, point_id) == 0 && strcmp(rule->equals, new_value) == 0) {
            if (if_conditions_match(rule)) {
                execute_then(rule, point_id, new_value);
            }
        }
    }
}

static bool time_synced(void)
{
    return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

static void check_time_rules(void)
{
    if (!time_synced()) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_tm;
    if (!localtime_r(&now, &local_tm)) {
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
            execute_then(rule, rule->point_id, rule->equals);
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

static void parse_rule(const cJSON *item, rule_t *rule)
{
    memset(rule, 0, sizeof(*rule));
    rule->last_fired_yday = -1;
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
        const cJSON *eq = cJSON_GetObjectItem(when, "equals");
        if (cJSON_IsString(eq)) {
            strncpy(rule->equals, eq->valuestring, sizeof(rule->equals) - 1);
        }
        const cJSON *at = cJSON_GetObjectItem(when, "at");
        if (cJSON_IsString(at)) {
            strncpy(rule->time_at, at->valuestring, sizeof(rule->time_at) - 1);
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
            const cJSON *eq = cJSON_GetObjectItem(cond, "equals");
            if (cJSON_IsString(pid)) {
                strncpy(ic->point_id, pid->valuestring, sizeof(ic->point_id) - 1);
            }
            if (cJSON_IsString(eq)) {
                strncpy(ic->equals, eq->valuestring, sizeof(ic->equals) - 1);
            }
            rule->if_count++;
        }
    }

    if (cJSON_IsObject(then) || cJSON_IsArray(then)) {
        const cJSON *items[MAX_THEN_ACTIONS];
        size_t n = 0;
        if (cJSON_IsObject(then)) {
            items[n++] = then;
        } else {
            cJSON *el = NULL;
            cJSON_ArrayForEach(el, then) {
                if (n >= MAX_THEN_ACTIONS) {
                    ESP_LOGW(TAG, "rule '%s' then[] truncated to %d", rule->id, MAX_THEN_ACTIONS);
                    break;
                }
                if (cJSON_IsObject(el)) {
                    items[n++] = el;
                }
            }
        }
        for (size_t i = 0; i < n; i++) {
            then_action_t *act = &rule->then[rule->then_count];
            const cJSON *type = cJSON_GetObjectItem(items[i], "type");
            if (cJSON_IsString(type)) {
                strncpy(act->type, type->valuestring, sizeof(act->type) - 1);
            }
            const cJSON *pid = cJSON_GetObjectItem(items[i], "point_id");
            if (cJSON_IsString(pid)) {
                strncpy(act->point_id, pid->valuestring, sizeof(act->point_id) - 1);
            }
            const cJSON *cmd = cJSON_GetObjectItem(items[i], "cmd");
            if (cJSON_IsString(cmd)) {
                strncpy(act->cmd, cmd->valuestring, sizeof(act->cmd) - 1);
            }
            const cJSON *delay = cJSON_GetObjectItem(items[i], "seconds");
            if (cJSON_IsNumber(delay)) {
                act->delay_sec = delay->valueint;
            }
            const cJSON *msg = cJSON_GetObjectItem(items[i], "message");
            if (cJSON_IsString(msg) && msg->valuestring) {
                strncpy(act->message, msg->valuestring, sizeof(act->message) - 1);
            }
            const cJSON *types = cJSON_GetObjectItem(items[i], "types");
            if (cJSON_IsString(types) && types->valuestring) {
                strncpy(act->types, types->valuestring, sizeof(act->types) - 1);
            }
            rule->then_count++;
        }
    }
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

static esp_err_t load_rules(const cJSON *rules_cfg)
{
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
        ESP_LOGI(TAG, "loaded rule '%s' (%s)", s_rules[s_rule_count].name, s_rules[s_rule_count].id);
        s_rule_count++;
    }
    rebuild_rule_index();
    return ESP_OK;
}

esp_err_t minihub_rules_init(const cJSON *rules_cfg)
{
    esp_err_t err = load_rules(rules_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = minihub_event_bus_subscribe("point.updated", on_point_updated, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return start_time_task();
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
    if (s_time_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(TIME_CHECK_INTERVAL_MS + 100));
    }
    return ESP_OK;
}
