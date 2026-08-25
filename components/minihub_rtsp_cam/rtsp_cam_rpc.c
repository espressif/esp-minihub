/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_rtsp_cam.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "minihub_module.h"
#include "minihub_auth.h"
#include "minihub_usb_msc.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_RTSP_CAM

static const char *TAG = "rtsp_cam_rpc";

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool url_decode_name(const char *in, char *out, size_t out_len)
{
    size_t oi = 0;
    for (size_t i = 0; in[i] && oi + 1 < out_len; i++) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            int hi = hex_nibble(in[i + 1]);
            int lo = hex_nibble(in[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out[oi++] = (char)((hi << 4) | lo);
            i += 2;
        } else if (in[i] == '+') {
            out[oi++] = ' ';
        } else {
            out[oi++] = in[i];
        }
    }
    out[oi] = '\0';
    return oi > 0;
}

static esp_err_t handle_rtsp_cam_file_get(httpd_req_t *req)
{
    if (minihub_auth_check(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "unauthorized");
    }

    const char *prefix = "/api/modules/rtsp_cam/files/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    const char *enc = req->uri + strlen(prefix);
    /* Strip query string if present. */
    char enc_buf[96];
    size_t n = 0;
    while (enc[n] && enc[n] != '?' && n + 1 < sizeof(enc_buf)) {
        enc_buf[n] = enc[n];
        n++;
    }
    enc_buf[n] = '\0';
    if (!enc_buf[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }

    char name[64];
    if (!url_decode_name(enc_buf, name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }

    char path[384];
    esp_err_t err = minihub_rtsp_cam_resolve_file(name, path, sizeof(path));
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "USB not mounted");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    struct stat st;
    minihub_usb_msc_io_lock();
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        minihub_usb_msc_io_unlock();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        minihub_usb_msc_io_unlock();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[128];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buf[512];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
            fclose(f);
            minihub_usb_msc_io_unlock();
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    minihub_usb_msc_io_unlock();
    return httpd_resp_send_chunk(req, NULL, 0);
}

static cJSON *ok_wrap(cJSON *payload)
{
    if (!payload) {
        payload = cJSON_CreateObject();
    }
    if (payload) {
        cJSON_AddStringToObject(payload, "result", "ok");
    }
    return payload;
}

static cJSON *fail_msg(const char *err)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "result", "fail");
    cJSON_AddStringToObject(o, "error", err ? err : "error");
    return o;
}

esp_err_t minihub_rtsp_cam_rpc(const char *method, const cJSON *params, cJSON **out)
{
    if (!method || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(method, "status") == 0) {
        *out = minihub_rtsp_cam_status_json();
        if (*out) {
            cJSON_AddStringToObject(*out, "result", "ok");
        }
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }

    if (strcmp(method, "config") == 0) {
        if (params) {
            const cJSON *url = cJSON_GetObjectItem(params, "url");
            const cJSON *cam = cJSON_GetObjectItem(params, "cam_id");
            const cJSON *seg = cJSON_GetObjectItem(params, "segment_sec");
            esp_err_t err = minihub_rtsp_cam_config_set(
                cJSON_IsString(url) ? url->valuestring : NULL,
                cJSON_IsString(cam) ? cam->valuestring : NULL,
                cJSON_IsNumber(seg) ? seg->valueint : 0);
            if (err != ESP_OK) {
                const char *msg = "config failed";
                if (err == ESP_ERR_INVALID_STATE) {
                    msg = "busy recording";
                } else if (err == ESP_ERR_INVALID_ARG) {
                    msg = "segment_sec must be 60..3600 (1..60 min)";
                } else {
                    msg = esp_err_to_name(err);
                }
                *out = fail_msg(msg);
                return ESP_OK;
            }
        }
        *out = minihub_rtsp_cam_status_json();
        if (*out) {
            cJSON_AddStringToObject(*out, "result", "ok");
        }
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }

    if (strcmp(method, "start") == 0) {
        if (params) {
            const cJSON *url = cJSON_GetObjectItem(params, "url");
            const cJSON *cam = cJSON_GetObjectItem(params, "cam_id");
            const cJSON *seg = cJSON_GetObjectItem(params, "segment_sec");
            if (cJSON_IsString(url) || cJSON_IsString(cam) || cJSON_IsNumber(seg)) {
                (void)minihub_rtsp_cam_config_set(
                    cJSON_IsString(url) ? url->valuestring : NULL,
                    cJSON_IsString(cam) ? cam->valuestring : NULL,
                    cJSON_IsNumber(seg) ? seg->valueint : 0);
            }
        }
        esp_err_t err = minihub_rtsp_cam_start();
        if (err == ESP_ERR_INVALID_STATE && minihub_rtsp_cam_recording()) {
            /* Already recording (e.g. AUTO_START) — treat as success. */
            *out = ok_wrap(minihub_rtsp_cam_status_json());
            return ESP_OK;
        }
        if (err != ESP_OK) {
            const char *msg = minihub_rtsp_cam_last_error();
            if (!msg || !msg[0]) {
                msg = err == ESP_ERR_INVALID_STATE ? "busy or USB missing" : esp_err_to_name(err);
            }
            *out = fail_msg(msg);
            return ESP_OK;
        }
        *out = ok_wrap(minihub_rtsp_cam_status_json());
        return ESP_OK;
    }

    if (strcmp(method, "stop") == 0) {
        (void)minihub_rtsp_cam_stop();
        *out = ok_wrap(minihub_rtsp_cam_status_json());
        return ESP_OK;
    }

    if (strcmp(method, "list_files") == 0) {
        int limit = 20;
        if (params) {
            const cJSON *lim = cJSON_GetObjectItem(params, "limit");
            if (cJSON_IsNumber(lim)) {
                limit = lim->valueint;
            }
        }
        cJSON *files = minihub_rtsp_cam_list_files_json(limit);
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            cJSON_Delete(files);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(o, "result", "ok");
        cJSON_AddItemToObject(o, "files", files ? files : cJSON_CreateArray());
        *out = o;
        return ESP_OK;
    }

    if (strcmp(method, "delete_files") == 0) {
        const cJSON *names = params ? cJSON_GetObjectItem(params, "names") : NULL;
        *out = minihub_rtsp_cam_delete_files_json(names);
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "unknown method %s", method);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t minihub_rtsp_cam_register_rpc(void)
{
    return minihub_module_register_rpc("rtsp_cam", minihub_rtsp_cam_rpc);
}

esp_err_t minihub_rtsp_cam_register_http(void *httpd_handle)
{
    httpd_handle_t server = (httpd_handle_t)httpd_handle;
    if (!server) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_uri_t uri = {
        .uri = "/api/modules/rtsp_cam/files/*",
        .method = HTTP_GET,
        .handler = handle_rtsp_cam_file_get,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register download URI failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "download URI ready: GET /api/modules/rtsp_cam/files/<name>");
    }
    return err;
}

#else

esp_err_t minihub_rtsp_cam_rpc(const char *method, const cJSON *params, cJSON **out)
{
    (void)method;
    (void)params;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_rtsp_cam_register_rpc(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t minihub_rtsp_cam_register_http(void *httpd_handle)
{
    (void)httpd_handle;
    return ESP_OK;
}

#endif
