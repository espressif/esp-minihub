/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosq_broker.h"

#include "minihub_config.h"
#include "minihub_core.h"
#include "minihub_mqtt_broker.h"
#include "minihub_task.h"

static const char *TAG = "mqtt_broker";

#define MH_BROKER_DEFAULT_PORT   1883
#define MH_BROKER_MAX_USERS      4
#define MH_BROKER_USER_MAX       32
#define MH_BROKER_PASS_MAX       32
#define MH_BROKER_SNAPSHOT_MAX   1024
#define MH_BROKER_STACK_WORDS    8192
#define MH_BROKER_TASK_PRIO      4
#define MH_BROKER_STOP_JOIN_MS   3000
#define MH_BROKER_RETRY_MS       5000
#define MH_BROKER_RATE_WINDOW_S  10
#define MH_BROKER_RATE_DEFAULT   16

typedef struct {
    char name[MH_BROKER_USER_MAX];
    char pass[MH_BROKER_PASS_MAX];
} broker_user_t;

/* Everything below is written by broker_apply() only, which holds s_apply_mu
 * and has already joined the broker task; the connect callback is the only
 * reader and it runs on that same broker task. That ordering is why the
 * callback needs no lock of its own. */
static broker_user_t s_users[MH_BROKER_MAX_USERS];
static size_t s_user_count;
static bool s_enabled;
static bool s_allow_anonymous = true;
static uint32_t s_rate_limit = MH_BROKER_RATE_DEFAULT;
static int64_t s_window_start;
static uint32_t s_window_count;

static struct mosq_broker_config s_broker_cfg = {
    .host = "0.0.0.0",
    .port = MH_BROKER_DEFAULT_PORT,
};

static SemaphoreHandle_t s_apply_mu;
static volatile bool s_task_live;
static volatile bool s_stop_requested;
static bool s_inited;
static char s_snapshot[MH_BROKER_SNAPSHOT_MAX];

static bool user_match(const char *name, const char *pass, int pass_len)
{
    bool matched = false;
    for (size_t i = 0; i < s_user_count; i++) {
        bool ok = (strcmp(s_users[i].name, name) == 0);
        size_t want = strlen(s_users[i].pass);
        if ((size_t)pass_len != want) {
            ok = false;
        } else if (want > 0 && pass) {
            unsigned diff = 0;
            for (size_t k = 0; k < want; k++) {
                diff |= (unsigned char)pass[k] ^ (unsigned char)s_users[i].pass[k];
            }
            ok = ok && (diff == 0);
        }
        matched |= ok;
    }
    return matched;
}

/* The mosquitto port compiles upstream's mosquitto_unpwd_check() into a
 * no-op, so this callback is the only authentication gate the broker has. */
static int handle_connect_cb(const char *client_id, const char *username, const char *password, int password_len)
{
    const char *who = client_id ? client_id : "?";
    int64_t now = esp_timer_get_time();
    if (now - s_window_start >= (int64_t)MH_BROKER_RATE_WINDOW_S * 1000000) {
        s_window_start = now;
        s_window_count = 0;
    }
    if (s_window_count >= s_rate_limit) {
        ESP_LOGW(TAG, "refusing CONNECT from %s: rate limit %u per %d s", who,
                 (unsigned)s_rate_limit, MH_BROKER_RATE_WINDOW_S);
        return 1;
    }
    s_window_count++;

    if (!username) {
        if (s_allow_anonymous) {
            return 0;
        }
        ESP_LOGW(TAG, "refusing anonymous CONNECT from %s", who);
        return 1;
    }
    if (user_match(username, password, password_len)) {
        return 0;
    }
    ESP_LOGW(TAG, "refusing CONNECT from %s: no matching user", who);
    return 1;
}

static void broker_task(void *arg)
{
    (void)arg;
    for (;;) {
        int rc = mosq_broker_run(&s_broker_cfg);
        ESP_LOGW(TAG, "mosq_broker_run returned %d", rc);
        if (s_stop_requested) {
            break;
        }
        /* A refused bind is not fatal: the Hub may still be waiting for an IP,
         * or the previous listener may not have released the port yet. */
        vTaskDelay(pdMS_TO_TICKS(MH_BROKER_RETRY_MS));
    }
    s_task_live = false;
    vTaskDelete(NULL);
}

static esp_err_t broker_start(void)
{
    if (s_task_live) {
        return ESP_OK;
    }
    s_broker_cfg.handle_connect_cb = handle_connect_cb;
    s_broker_cfg.handle_message_cb = NULL;
    s_stop_requested = false;
    s_task_live = true;
    if (minihub_task_create_psram(broker_task, "mqtt_broker", MH_BROKER_STACK_WORDS, NULL,
                                  MH_BROKER_TASK_PRIO, NULL) != pdPASS) {
        s_task_live = false;
        ESP_LOGE(TAG, "broker task create failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening on %s:%d, anonymous %s, %u user(s)", s_broker_cfg.host,
             s_broker_cfg.port, s_allow_anonymous ? "allowed" : "refused", (unsigned)s_user_count);
    return ESP_OK;
}

static esp_err_t broker_stop(void)
{
    if (!s_task_live) {
        return ESP_OK;
    }
    s_stop_requested = true;
    mosq_broker_stop();
    /* mosq_broker_stop() only unblocks run(): last-will delivery and the port
     * release still happen on the broker task afterwards, so reconfiguring
     * before it exits would rebind a port that is still held. s_task_live is
     * cleared by that task and never by us, which also keeps a late exit from
     * racing a fresh start. */
    for (int waited = 0; waited < MH_BROKER_STOP_JOIN_MS; waited += 50) {
        if (!s_task_live) {
            s_stop_requested = false;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "broker task did not exit in %d ms; keeping the running config",
             MH_BROKER_STOP_JOIN_MS);
    return ESP_ERR_TIMEOUT;
}

static const cJSON *broker_cfg_object(void)
{
    cJSON *root = minihub_config_get_adapters();
    if (!root) {
        return NULL;
    }
    const cJSON *adapters = cJSON_GetObjectItemCaseSensitive(root, "adapters");
    if (!cJSON_IsObject(adapters)) {
        return NULL;
    }
    const cJSON *sub = cJSON_GetObjectItemCaseSensitive(adapters, "mqtt_broker");
    return cJSON_IsObject(sub) ? sub : NULL;
}

static void parse_cfg(const cJSON *sub)
{
    s_enabled = sub && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sub, "enabled"));
    s_user_count = 0;
    if (!s_enabled) {
        return;
    }

    const cJSON *port = cJSON_GetObjectItemCaseSensitive(sub, "port");
    s_broker_cfg.port = (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536)
                            ? port->valueint
                            : MH_BROKER_DEFAULT_PORT;

    s_allow_anonymous = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(sub, "allow_anonymous"));

    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(sub, "connect_rate_limit");
    s_rate_limit = (cJSON_IsNumber(rate) && rate->valueint > 0) ? (uint32_t)rate->valueint
                                                                : MH_BROKER_RATE_DEFAULT;

    const cJSON *users = cJSON_GetObjectItemCaseSensitive(sub, "users");
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, users) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(item, "pass");
        if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
            continue;
        }
        if (s_user_count >= MH_BROKER_MAX_USERS) {
            ESP_LOGW(TAG, "ignoring users past the first %d", MH_BROKER_MAX_USERS);
            break;
        }
        broker_user_t *u = &s_users[s_user_count++];
        strlcpy(u->name, name->valuestring, sizeof(u->name));
        strlcpy(u->pass, cJSON_IsString(pass) ? pass->valuestring : "", sizeof(u->pass));
    }
}

static void broker_apply(void)
{
    const cJSON *sub = broker_cfg_object();
    char *printed = sub ? cJSON_PrintUnformatted(sub) : NULL;
    const char *snap = printed ? printed : "";

    if (xSemaphoreTake(s_apply_mu, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "another apply is running; keeping the current listener");
        free(printed);
        return;
    }
    bool snapshot_fits = strlen(snap) < sizeof(s_snapshot);
    bool changed = !snapshot_fits || strcmp(snap, s_snapshot) != 0;
    if (!snapshot_fits) {
        ESP_LOGW(TAG, "mqtt_broker config is %u bytes, too long to diff: restarting on every reload",
                 (unsigned)strlen(snap));
    }
    if (changed && broker_stop() == ESP_OK) {
        parse_cfg(sub);
        if (snapshot_fits) {
            strlcpy(s_snapshot, snap, sizeof(s_snapshot));
        }
        if (s_enabled && broker_start() != ESP_OK) {
            s_snapshot[0] = '\0';
        }
    }
    xSemaphoreGive(s_apply_mu);
    free(printed);
}

static void on_hub_reloaded(const minihub_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    broker_apply();
}

esp_err_t minihub_mqtt_broker_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_apply_mu = xSemaphoreCreateMutex();
    if (!s_apply_mu) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    esp_err_t err = minihub_event_bus_subscribe("hub.reloaded", on_hub_reloaded, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hub.reloaded subscribe: %s", esp_err_to_name(err));
    }
    broker_apply();
    return ESP_OK;
}
