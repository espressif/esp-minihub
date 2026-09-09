/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_web.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "minihub_core.h"
#include "minihub_auth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "minihub_ws";

#define WS_MAX_CLIENTS 8

typedef struct {
    int fd;
    bool active;
} ws_client_t;

static httpd_handle_t s_ws_server;
static ws_client_t s_clients[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mutex;

static void ws_clients_init(void)
{
    if (!s_clients_mutex) {
        s_clients_mutex = xSemaphoreCreateMutex();
    }
    memset(s_clients, 0, sizeof(s_clients));
}

#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
static void ws_client_add(int fd)
{
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            s_clients[i].active = true;
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}
#endif

static void ws_client_remove(int fd)
{
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            s_clients[i].active = false;
            s_clients[i].fd = -1;
            break;
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

typedef struct {
    httpd_handle_t hd;
    int fd;
    char *payload;
    size_t len;
} ws_send_arg_t;

static void ws_async_send_cb(void *arg)
{
    ws_send_arg_t *send_arg = arg;
    if (!send_arg) {
        return;
    }

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)send_arg->payload,
        .len = send_arg->len,
    };
    httpd_ws_send_frame_async(send_arg->hd, send_arg->fd, &frame);
    free(send_arg->payload);
    free(send_arg);
}

static void ws_broadcast_json(const char *json)
{
    if (!s_ws_server || !json) {
        return;
    }

    size_t len = strlen(json);
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            continue;
        }
        ws_send_arg_t *arg = calloc(1, sizeof(ws_send_arg_t));
        if (!arg) {
            continue;
        }
        arg->hd = s_ws_server;
        arg->fd = s_clients[i].fd;
        arg->payload = strdup(json);
        if (!arg->payload) {
            free(arg);
            continue;
        }
        arg->len = len;
        if (httpd_queue_work(s_ws_server, ws_async_send_cb, arg) != ESP_OK) {
            free(arg->payload);
            free(arg);
        }
    }
    xSemaphoreGive(s_clients_mutex);
}

static cJSON *event_to_json(const minihub_event_t *event)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "event_type", event->event_type);
    cJSON_AddNumberToObject(obj, "timestamp_ms", (double)event->timestamp_ms);
    if (strcmp(event->event_type, "point.updated") == 0) {
        cJSON_AddNumberToObject(obj, "point_index", event->point_index);
        cJSON_AddStringToObject(obj, "old_value", event->old_value);
        cJSON_AddStringToObject(obj, "new_value", event->new_value);
    }
    if (event->data) {
        cJSON_AddItemToObject(obj, "data", cJSON_Duplicate(event->data, true));
    }
    return obj;
}

static void ws_event_handler(const minihub_event_t *event, void *user_data)
{
    (void)user_data;
    cJSON *obj = event_to_json(event);
    if (!obj) {
        return;
    }
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (json) {
        ws_broadcast_json(json);
        free(json);
    }
}

#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
static esp_err_t ws_auth_from_query(httpd_req_t *req)
{
    if (minihub_auth_check(req) == ESP_OK) {
        return ESP_OK;
    }

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    char token[65];
    if (httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return minihub_auth_check_token(token);
}

static esp_err_t ws_on_connect(httpd_req_t *req)
{
    if (ws_auth_from_query(req) != ESP_OK) {
        return ESP_FAIL;
    }
    ws_client_add(httpd_req_to_sockfd(req));
    return ESP_OK;
}
#endif

static esp_err_t handle_ws_events(httpd_req_t *req)
{
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_client_remove(httpd_req_to_sockfd(req));
    } else if (frame.type == HTTPD_WS_TYPE_PING) {
        frame.type = HTTPD_WS_TYPE_PONG;
        httpd_ws_send_frame(req, &frame);
    }
    return ESP_OK;
}

esp_err_t minihub_web_ws_start(httpd_handle_t server)
{
    s_ws_server = server;
    ws_clients_init();

    httpd_uri_t ws_events = {
        .uri = "/ws/events",
        .method = HTTP_GET,
        .handler = handle_ws_events,
        .is_websocket = true,
#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
        .ws_post_handshake_cb = ws_on_connect,
#endif
    };

    esp_err_t err = httpd_register_uri_handler(server, &ws_events);
    if (err != ESP_OK) {
        return err;
    }

    err = minihub_event_bus_subscribe("*", ws_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event subscribe failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "WebSocket /ws/events ready");
    return ESP_OK;
}
