/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_adapter.h"
#include "minihub_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "mqtt_client.h"

/* Logs as "mqtt_client": components/minihub_mqtt_broker owns the "mqtt_broker"
 * tag, and both run at the same time. */
static const char *TAG = "mqtt_client";

/* There is exactly one broker this client can talk to — the Hub's own listener
 * on loopback — so its port and credentials are read out of adapters.mqtt_broker
 * instead of a second config block. Buffer sizes match the broker's own
 * MH_BROKER_USER_MAX / MH_BROKER_PASS_MAX so both sides truncate identically. */
#define MQTT_LOOPBACK_HOST "127.0.0.1"
#define MQTT_DEFAULT_PORT  1883
#define MQTT_USER_MAX      32
#define MQTT_PASS_MAX      32

typedef struct {
    char filter[MINIHUB_MQTT_FILTER_MAX];
    int qos;
    /* Which L2 plugin holds this subscription, so one plugin's unsubscribe
     * cannot drop another plugin's identical filter, and a message can be
     * delivered to every owner that asked for it. */
    uint8_t owner;
} mqtt_filter_t;

static esp_mqtt_client_handle_t s_client;
static int s_port = MQTT_DEFAULT_PORT;
static char s_username[MQTT_USER_MAX];
static char s_password[MQTT_PASS_MAX];

/* Wildcard subscriptions owned by the JS bridge (host.mqtt). Protected by a
 * spinlock because add/remove run on the JS worker while the event handler
 * reads them on the esp-mqtt task. */
static mqtt_filter_t s_filters[MINIHUB_MQTT_FILTERS_MAX];
static size_t s_filter_count;
static minihub_mqtt_hook_t s_hook;
static void *s_hook_arg;
static bool s_connected;
static portMUX_TYPE s_mqtt_mux = portMUX_INITIALIZER_UNLOCKED;

/* MQTT 3.1.1 topic-filter match, level by level. '+' matches exactly one
 * level (including an empty one), '#' matches the remaining levels including
 * zero and is only valid as the final level. The topic is NOT NUL-terminated
 * (esp-mqtt hands us topic + topic_len), so every read is length-bounded. */
static bool mqtt_filter_matches(const char *filter, const char *topic, int topic_len)
{
    if (!filter || !*filter || !topic || topic_len <= 0) {
        return false;
    }
    if (topic[0] == '$' && filter[0] != '$') {
        return false; /* wildcards never match $-system topics */
    }

    const char *t = topic;
    const char *end = topic + topic_len;
    const char *f = filter;

    while (*f) {
        if (*f == '#') {
            return f[1] == '\0';
        }
        const char *f_end = strchr(f, '/');
        size_t f_len = f_end ? (size_t)(f_end - f) : strlen(f);
        const char *t_end = memchr(t, '/', (size_t)(end - t));
        if (!t_end) {
            t_end = end;
        }
        size_t t_len = (size_t)(t_end - t);

        if (f_len == 1 && f[0] == '+') {
            if (!f_end) {
                return t_end == end;
            }
        } else {
            if (f_len && f[f_len - 1] == '#') {
                return false; /* '#' must be a whole level */
            }
            if (f_len != t_len || strncmp(f, t, f_len) != 0) {
                return false;
            }
        }

        if (!f_end) {
            return t_end == end;
        }
        if (t_end == end) {
            return strcmp(f_end, "/#") == 0; /* "a/#" also matches "a" */
        }
        f = f_end + 1;
        t = t_end + 1;
    }
    return t == end;
}

static uint32_t matching_owners(const char *topic, int topic_len)
{
    uint32_t mask = 0;
    portENTER_CRITICAL(&s_mqtt_mux);
    for (size_t i = 0; i < s_filter_count; i++) {
        if (mqtt_filter_matches(s_filters[i].filter, topic, topic_len)) {
            mask |= 1u << s_filters[i].owner;
        }
    }
    portEXIT_CRITICAL(&s_mqtt_mux);
    return mask;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to broker");
        portENTER_CRITICAL(&s_mqtt_mux);
        s_connected = true;
        portEXIT_CRITICAL(&s_mqtt_mux);
        /* JS filters are not persisted by esp-mqtt; re-subscribe after every
         * reconnect so L2 discovery keeps working across broker restarts.
         * esp_mqtt_client_subscribe() may block, so it must never run inside
         * the spinlock; copy one filter out at a time (keeps stack use small
         * even with MINIHUB_MQTT_FILTERS_MAX = 32). */
        for (size_t i = 0; i < MINIHUB_MQTT_FILTERS_MAX; i++) {
            char filter[MINIHUB_MQTT_FILTER_MAX];
            int qos;
            bool duplicate = false;
            portENTER_CRITICAL(&s_mqtt_mux);
            if (i >= s_filter_count) {
                portEXIT_CRITICAL(&s_mqtt_mux);
                break;
            }
            memcpy(filter, s_filters[i].filter, sizeof(filter));
            qos = s_filters[i].qos;
            /* Owners of the same filter string share one broker subscription. */
            for (size_t j = 0; j < i; j++) {
                if (strcmp(s_filters[j].filter, filter) == 0) {
                    duplicate = true;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_mqtt_mux);
            if (!duplicate) {
                esp_mqtt_client_subscribe(s_client, filter, qos);
            }
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from broker");
        portENTER_CRITICAL(&s_mqtt_mux);
        s_connected = false;
        portEXIT_CRITICAL(&s_mqtt_mux);
        break;
    case MQTT_EVENT_DATA: {
        /* Only messages matching a registered filter reach the JS ring buffer. */
        minihub_mqtt_hook_t hook;
        void *arg;
        portENTER_CRITICAL(&s_mqtt_mux);
        hook = s_hook;
        arg = s_hook_arg;
        portEXIT_CRITICAL(&s_mqtt_mux);
        /* esp-mqtt splits large payloads into several MQTT_EVENT_DATA events.
         * A fragment is not valid JSON, so do not let it occupy a ring slot. */
        bool fragmented = event->total_data_len > event->data_len ||
                          event->current_data_offset > 0;
        uint32_t owners = matching_owners(event->topic, event->topic_len);
        if (hook && !fragmented && owners != 0) {
            hook(event->topic, event->topic_len, event->data, event->data_len, owners, arg);
        }
        break;
    }
    default:
        break;
    }
}

/* adapters.mqtt_broker is the only config this adapter reads: the port the
 * broker listens on, plus the credentials it demands once anonymous connects
 * are refused. Both sides read the same subtree, so they cannot drift. */
static esp_err_t mqtt_adapter_init(const cJSON *config)
{
    s_port = MQTT_DEFAULT_PORT;
    s_username[0] = '\0';
    s_password[0] = '\0';
    if (!cJSON_IsObject(config)) {
        return ESP_OK;
    }

    const cJSON *port = cJSON_GetObjectItem(config, "port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint < 65536) {
        s_port = port->valueint;
    }
    if (!cJSON_IsFalse(cJSON_GetObjectItem(config, "allow_anonymous"))) {
        return ESP_OK;
    }

    /* handle_connect_cb() is the broker's only auth gate and it gets no peer
     * address, so it cannot tell the Hub's own loopback client from a remote
     * one: with anonymous refused, this client has to present a listed user. */
    const cJSON *users = cJSON_GetObjectItem(config, "users");
    const cJSON *first = cJSON_IsArray(users) ? cJSON_GetArrayItem(users, 0) : NULL;
    const cJSON *name = first ? cJSON_GetObjectItem(first, "name") : NULL;
    const cJSON *pass = first ? cJSON_GetObjectItem(first, "pass") : NULL;
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "allow_anonymous is false and users[] carries no name: the broker "
                      "will refuse the Hub's own client, so host.mqtt stays disconnected");
        return ESP_OK;
    }
    strlcpy(s_username, name->valuestring, sizeof(s_username));
    strlcpy(s_password, cJSON_IsString(pass) ? pass->valuestring : "", sizeof(s_password));
    return ESP_OK;
}

static esp_err_t mqtt_adapter_start(void)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_LOOPBACK_HOST, s_port);
    ESP_LOGI(TAG, "connecting to %s as %s", uri, s_username[0] ? s_username : "anonymous");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = "minihub_host",
    };
    if (s_username[0]) {
        mqtt_cfg.credentials.username = s_username;
        mqtt_cfg.credentials.authentication.password = s_password;
    }
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    return esp_mqtt_client_start(s_client);
}

static esp_err_t mqtt_adapter_stop(void)
{
    portENTER_CRITICAL(&s_mqtt_mux);
    s_connected = false;
    portEXIT_CRITICAL(&s_mqtt_mux);
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    return ESP_OK;
}

/* ---- Public surface for host.mqtt (called from the JS worker) ----------- */

esp_err_t minihub_mqtt_add_filter(const char *filter, int qos, int owner)
{
    if (!filter || !filter[0] || strlen(filter) >= sizeof(s_filters[0].filter) ||
        owner < 0 || owner >= MINIHUB_MQTT_OWNERS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    bool own_it = false; /* this call is the first owner of that string */
    portENTER_CRITICAL(&s_mqtt_mux);
    size_t i;
    bool held_by_other = false;
    for (i = 0; i < s_filter_count; i++) {
        if (strcmp(s_filters[i].filter, filter) == 0) {
            if (s_filters[i].owner == (uint8_t)owner) {
                break;
            }
            held_by_other = true;
        }
    }
    if (i < s_filter_count) {
        s_filters[i].qos = qos; /* this owner already subscribes it, refresh qos */
    } else if (s_filter_count >= MINIHUB_MQTT_FILTERS_MAX) {
        err = ESP_ERR_NO_MEM;
    } else {
        strncpy(s_filters[s_filter_count].filter, filter, sizeof(s_filters[0].filter) - 1);
        s_filters[s_filter_count].qos = qos;
        s_filters[s_filter_count].owner = (uint8_t)owner;
        s_filter_count++;
        own_it = !held_by_other;
    }
    portEXIT_CRITICAL(&s_mqtt_mux);

    /* A filter another owner already subscribes needs no second SUBSCRIBE. */
    if (err == ESP_OK && own_it && minihub_mqtt_is_connected()) {
        int msg_id = esp_mqtt_client_subscribe(s_client, filter, qos);
        if (msg_id < 0) {
            ESP_LOGW(TAG, "subscribe %s failed", filter);
            err = ESP_FAIL;
        }
    }
    return err;
}

esp_err_t minihub_mqtt_remove_filter(const char *filter, int owner)
{
    if (!filter || !filter[0] || owner < 0 || owner >= MINIHUB_MQTT_OWNERS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    bool removed = false;
    bool last_owner = true;
    portENTER_CRITICAL(&s_mqtt_mux);
    for (size_t i = 0; i < s_filter_count; i++) {
        if (strcmp(s_filters[i].filter, filter) == 0 && s_filters[i].owner == (uint8_t)owner) {
            memmove(&s_filters[i], &s_filters[i + 1],
                    (s_filter_count - i - 1) * sizeof(s_filters[0]));
            s_filter_count--;
            removed = true;
            break;
        }
    }
    if (removed) {
        for (size_t i = 0; i < s_filter_count; i++) {
            if (strcmp(s_filters[i].filter, filter) == 0) {
                last_owner = false;
                break;
            }
            last_owner = true;
        }
    }
    portEXIT_CRITICAL(&s_mqtt_mux);
    /* Only the last owner's unsubscribe reaches the broker, so one plugin
     * dropping a filter another plugin still subscribes stays subscribed. */
    if (removed && last_owner && minihub_mqtt_is_connected()) {
        esp_mqtt_client_unsubscribe(s_client, filter);
    }
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_mqtt_clear_owner(int owner)
{
    if (owner < 0 || owner >= MINIHUB_MQTT_OWNERS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Distinct filter strings this owner held, gathered before we mutate the
     * table. Worst case every filter entry is unique to this owner. */
    char owned[MINIHUB_MQTT_FILTERS_MAX][MINIHUB_MQTT_FILTER_MAX];
    size_t owned_count = 0;

    portENTER_CRITICAL(&s_mqtt_mux);
    for (size_t i = 0; i < s_filter_count; ) {
        if (s_filters[i].owner != (uint8_t)owner) {
            i++;
            continue;
        }
        bool seen = false;
        for (size_t k = 0; k < owned_count; k++) {
            if (strcmp(owned[k], s_filters[i].filter) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen && owned_count < MINIHUB_MQTT_FILTERS_MAX) {
            strncpy(owned[owned_count], s_filters[i].filter, MINIHUB_MQTT_FILTER_MAX - 1);
            owned[owned_count][MINIHUB_MQTT_FILTER_MAX - 1] = '\0';
            owned_count++;
        }
        memmove(&s_filters[i], &s_filters[i + 1],
                (s_filter_count - i - 1) * sizeof(s_filters[0]));
        s_filter_count--;
    }
    portEXIT_CRITICAL(&s_mqtt_mux);

    /* Unsubscribe a filter only if no remaining owner still holds it. */
    for (size_t j = 0; j < owned_count; j++) {
        bool still_held = false;
        portENTER_CRITICAL(&s_mqtt_mux);
        for (size_t i = 0; i < s_filter_count; i++) {
            if (strcmp(s_filters[i].filter, owned[j]) == 0) {
                still_held = true;
                break;
            }
        }
        portEXIT_CRITICAL(&s_mqtt_mux);
        if (!still_held && minihub_mqtt_is_connected()) {
            esp_mqtt_client_unsubscribe(s_client, owned[j]);
        }
    }
    return ESP_OK;
}

void minihub_mqtt_clear_filters(void)
{
    if (!minihub_mqtt_is_connected()) {
        portENTER_CRITICAL(&s_mqtt_mux);
        s_filter_count = 0;
        portEXIT_CRITICAL(&s_mqtt_mux);
        return;
    }
    /* Pop one at a time: unsubscribe() may block and must stay outside the
     * spinlock, and a 32-filter local array would cost 3 KB of JS-worker stack. */
    for (;;) {
        char filter[MINIHUB_MQTT_FILTER_MAX] = {0};
        portENTER_CRITICAL(&s_mqtt_mux);
        if (s_filter_count == 0) {
            portEXIT_CRITICAL(&s_mqtt_mux);
            break;
        }
        s_filter_count--;
        memcpy(filter, s_filters[s_filter_count].filter, sizeof(filter));
        portEXIT_CRITICAL(&s_mqtt_mux);
        esp_mqtt_client_unsubscribe(s_client, filter);
    }
}

void minihub_mqtt_set_hook(minihub_mqtt_hook_t hook, void *user_data)
{
    portENTER_CRITICAL(&s_mqtt_mux);
    s_hook = hook;
    s_hook_arg = user_data;
    portEXIT_CRITICAL(&s_mqtt_mux);
}

esp_err_t minihub_mqtt_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!topic || !topic[0] || !payload) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!minihub_mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    return msg_id < 0 ? ESP_FAIL : ESP_OK;
}

bool minihub_mqtt_is_connected(void)
{
    portENTER_CRITICAL(&s_mqtt_mux);
    bool connected = s_connected && s_client != NULL;
    portEXIT_CRITICAL(&s_mqtt_mux);
    return connected;
}

size_t minihub_mqtt_filter_count(void)
{
    portENTER_CRITICAL(&s_mqtt_mux);
    size_t n = s_filter_count;
    portEXIT_CRITICAL(&s_mqtt_mux);
    return n;
}

/* Registered as "mqtt_broker" on purpose: the loader decides whether to init and
 * start an adapter by looking up its own name in adapters.json, so this name is
 * what makes the client follow the broker's enabled flag. A separate "mqtt"
 * block would be a second switch for one service. */
const minihub_adapter_t mqtt_adapter = {
    .name = "mqtt_broker",
    .version = "0.1.0",
    .init = mqtt_adapter_init,
    .start = mqtt_adapter_start,
    .stop = mqtt_adapter_stop,
    .deinit = mqtt_adapter_stop,
};

REGISTER_ADAPTER(mqtt_adapter);
