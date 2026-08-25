/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal RTSP server: one TCP interleaved viewer on :8554 /cam0.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_RTSP_CAM

#include "rtsp_cam_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#ifndef CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT
#define CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT 8554
#endif

static const char *TAG = "rtsp_relay";

static int read_line(int fd, char *buf, size_t buflen)
{
    size_t n = 0;
    while (n + 1 < buflen) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        buf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[n] = '\0';
    return (int)n;
}

static int read_request(int fd, char *buf, size_t buflen)
{
    size_t total = 0;
    buf[0] = '\0';
    for (;;) {
        if (total + 1 >= buflen) {
            return -1;
        }
        int n = read_line(fd, buf + total, buflen - total);
        if (n <= 0) {
            return n;
        }
        total += (size_t)n;
        if (n == 2 && buf[total - 2] == '\r' && buf[total - 1] == '\n') {
            /* empty line after headers — but first empty? check \r\n\r\n */
        }
        if (total >= 4 && strcmp(buf + total - 4, "\r\n\r\n") == 0) {
            break;
        }
        /* Also accept ending with blank line built gradually */
        if (strstr(buf, "\r\n\r\n")) {
            break;
        }
    }
    return (int)total;
}

static void send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        int n = send(fd, data, len, 0);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

static void reply(int fd, int code, const char *reason, int cseq, const char *extra_hdr,
                  const char *body)
{
    char hdr[512];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %d\r\n"
                     "%s"
                     "Content-Length: %u\r\n"
                     "\r\n",
                     code, reason ? reason : "", cseq, extra_hdr ? extra_hdr : "",
                     (unsigned)blen);
    if (n > 0) {
        send_all(fd, hdr, (size_t)n);
    }
    if (body && blen) {
        send_all(fd, body, blen);
    }
}

static const char *strcasestr_local(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) {
        return hay;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == nlen) {
            return p;
        }
    }
    return NULL;
}

static int parse_cseq(const char *req)
{
    const char *p = strcasestr_local(req, "\nCSeq:");
    if (!p) {
        p = strcasestr_local(req, "CSeq:");
    }
    if (!p) {
        return 1;
    }
    p = strchr(p, ':');
    if (!p) {
        return 1;
    }
    return atoi(p + 1);
}

static bool path_ok(const char *req_line)
{
    /* e.g. DESCRIBE rtsp://x:8554/cam0 RTSP/1.0 */
    return strstr(req_line, "/cam0") != NULL;
}

static void host_from_req(const char *req, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *p = strcasestr_local(req, "\nHost:");
    if (!p) {
        p = strcasestr_local(req, "Host:");
    }
    if (!p) {
        snprintf(out, out_len, "127.0.0.1:%d", minihub_rtsp_cam_relay_port());
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        snprintf(out, out_len, "127.0.0.1:%d", minihub_rtsp_cam_relay_port());
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < out_len) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    if (!out[0]) {
        snprintf(out, out_len, "127.0.0.1:%d", minihub_rtsp_cam_relay_port());
    }
}

static bool transport_wants_tcp(const char *req)
{
    const char *t = strcasestr_local(req, "\nTransport:");
    if (!t) {
        t = strcasestr_local(req, "Transport:");
    }
    if (!t) {
        return false;
    }
    /* interleaved= or RTP/AVP/TCP */
    return strcasestr_local(t, "interleaved=") != NULL ||
           strcasestr_local(t, "RTP/AVP/TCP") != NULL;
}

static void handle_client(int fd)
{
    struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char session[32] = "1";
    bool playing = false;
    char req[2048];

    while (1) {
        int n = read_request(fd, req, sizeof(req));
        if (n <= 0) {
            break;
        }

        char method[16] = {0};
        sscanf(req, "%15s", method);
        int cseq = parse_cseq(req);
        ESP_LOGI(TAG, "client %s cseq=%d", method, cseq);

        if (strcasecmp(method, "OPTIONS") == 0) {
            reply(fd, 200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n",
                  NULL);
            continue;
        }

        if (strcasecmp(method, "DESCRIBE") == 0) {
            if (!path_ok(req)) {
                reply(fd, 404, "Not Found", cseq, NULL, NULL);
                break;
            }
            char host[80];
            host_from_req(req, host, sizeof(host));
            char ip_only[64];
            snprintf(ip_only, sizeof(ip_only), "%s", host);
            char *colon = strchr(ip_only, ':');
            if (colon) {
                *colon = '\0';
            }
            /* Pull sprop from upstream SDP cache when available (helps VLC/live555). */
            char sprop[256] = {0};
            const char *cached = minihub_rtsp_cam_cached_sdp();
            if (cached && cached[0]) {
                const char *sp = strstr(cached, "sprop-parameter-sets=");
                if (!sp) {
                    sp = strstr(cached, "sprop-vps=");
                }
                if (sp) {
                    const char *line = sp;
                    while (line > cached && line[-1] != '\n') {
                        line--;
                    }
                    size_t i = 0;
                    while (line[i] && line[i] != '\r' && line[i] != '\n' && i + 1 < sizeof(sprop)) {
                        sprop[i] = line[i];
                        i++;
                    }
                    sprop[i] = '\0';
                }
            }
            char sdp[768];
            if (sprop[0] && strncmp(sprop, "a=fmtp:", 7) == 0) {
                snprintf(sdp, sizeof(sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 %s\r\n"
                         "s=MiniHub %s\r\n"
                         "t=0 0\r\n"
                         "a=control:*\r\n"
                         "m=video 0 RTP/AVP 96\r\n"
                         "a=rtpmap:96 H265/90000\r\n"
                         "%s\r\n"
                         "a=control:track1\r\n",
                         ip_only[0] ? ip_only : "0.0.0.0", minihub_rtsp_cam_id(), sprop);
            } else if (sprop[0]) {
                snprintf(sdp, sizeof(sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 %s\r\n"
                         "s=MiniHub %s\r\n"
                         "t=0 0\r\n"
                         "a=control:*\r\n"
                         "m=video 0 RTP/AVP 96\r\n"
                         "a=rtpmap:96 H265/90000\r\n"
                         "a=fmtp:96 %s\r\n"
                         "a=control:track1\r\n",
                         ip_only[0] ? ip_only : "0.0.0.0", minihub_rtsp_cam_id(), sprop);
            } else {
                snprintf(sdp, sizeof(sdp),
                         "v=0\r\n"
                         "o=- 0 0 IN IP4 %s\r\n"
                         "s=MiniHub %s\r\n"
                         "t=0 0\r\n"
                         "a=control:*\r\n"
                         "m=video 0 RTP/AVP 96\r\n"
                         "a=rtpmap:96 H265/90000\r\n"
                         "a=control:track1\r\n",
                         ip_only[0] ? ip_only : "0.0.0.0", minihub_rtsp_cam_id());
            }
            char extra[192];
            snprintf(extra, sizeof(extra),
                     "Content-Type: application/sdp\r\n"
                     "Content-Base: rtsp://%s/cam0/\r\n",
                     host);
            reply(fd, 200, "OK", cseq, extra, sdp);
            continue;
        }

        if (strcasecmp(method, "SETUP") == 0) {
            if (minihub_rtsp_cam_viewer_busy()) {
                reply(fd, 453, "Not Enough Bandwidth", cseq, NULL, NULL);
                break;
            }
            if (!transport_wants_tcp(req)) {
                /* Force VLC/live555 to retry with TCP interleaved. */
                reply(fd, 461, "Unsupported Transport", cseq,
                      "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", NULL);
                continue;
            }
            char extra[192];
            snprintf(extra, sizeof(extra),
                     "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                     "Session: %s;timeout=60\r\n",
                     session);
            reply(fd, 200, "OK", cseq, extra, NULL);
            continue;
        }

        if (strcasecmp(method, "PLAY") == 0) {
            if (minihub_rtsp_cam_viewer_busy()) {
                reply(fd, 453, "Not Enough Bandwidth", cseq, NULL, NULL);
                break;
            }
            if (minihub_rtsp_cam_ensure_upstream() != ESP_OK) {
                reply(fd, 454, "Session Not Found", cseq, NULL, NULL);
                break;
            }
            for (int i = 0; i < 80 && !minihub_rtsp_cam_upstream_streaming(); i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!minihub_rtsp_cam_upstream_streaming()) {
                minihub_rtsp_cam_viewer_detach(-1);
                reply(fd, 454, "Session Not Found", cseq, NULL, NULL);
                break;
            }
            char extra[128];
            snprintf(extra, sizeof(extra), "Session: %s\r\nRange: npt=0.000-\r\n", session);
            reply(fd, 200, "OK", cseq, extra, NULL);
            /* RTP only after PLAY response — otherwise live555 desyncs. */
            minihub_rtsp_cam_viewer_attach(fd);
            playing = true;
            ESP_LOGI(TAG, "viewer PLAY attached");

            /* Wait until client closes or sends TEARDOWN; drain any RTCP/RTSP. */
            while (playing) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(fd, &rfds);
                struct timeval tvo = { .tv_sec = 1, .tv_usec = 0 };
                int sel = select(fd + 1, &rfds, NULL, NULL, &tvo);
                if (sel < 0) {
                    break;
                }
                if (sel == 0) {
                    continue;
                }
                char peek[8];
                int r = recv(fd, peek, sizeof(peek), MSG_PEEK);
                if (r <= 0) {
                    break;
                }
                if (peek[0] == '$') {
                    /* Discard interleaved RTCP from client. */
                    uint8_t hdr4[4];
                    size_t got = 0;
                    while (got < 4) {
                        int r2 = recv(fd, hdr4 + got, 4 - got, 0);
                        if (r2 <= 0) {
                            playing = false;
                            break;
                        }
                        got += (size_t)r2;
                    }
                    if (!playing) {
                        break;
                    }
                    uint16_t sz = ((uint16_t)hdr4[2] << 8) | hdr4[3];
                    char dump[512];
                    while (sz > 0) {
                        size_t chunk = sz > sizeof(dump) ? sizeof(dump) : sz;
                        int g = recv(fd, dump, chunk, 0);
                        if (g <= 0) {
                            playing = false;
                            break;
                        }
                        sz -= (uint16_t)g;
                    }
                    if (!playing) {
                        break;
                    }
                    continue;
                }
                /* RTSP command */
                n = read_request(fd, req, sizeof(req));
                if (n <= 0) {
                    break;
                }
                method[0] = '\0';
                sscanf(req, "%15s", method);
                cseq = parse_cseq(req);
                if (strcasecmp(method, "TEARDOWN") == 0) {
                    reply(fd, 200, "OK", cseq, NULL, NULL);
                    playing = false;
                    break;
                }
                if (strcasecmp(method, "OPTIONS") == 0) {
                    reply(fd, 200, "OK", cseq,
                          "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n", NULL);
                    continue;
                }
                /* ignore others */
            }
            minihub_rtsp_cam_viewer_detach(fd);
            ESP_LOGI(TAG, "viewer detached");
            break;
        }

        if (strcasecmp(method, "TEARDOWN") == 0) {
            reply(fd, 200, "OK", cseq, NULL, NULL);
            break;
        }

        reply(fd, 501, "Not Implemented", cseq, NULL, NULL);
    }

    minihub_rtsp_cam_viewer_detach(fd);
    close(fd);
}

static void relay_task(void *arg)
{
    (void)arg;
    int port = minihub_rtsp_cam_relay_port();
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket: %s", strerror(errno));
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d: %s", port, strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_fd, 1) != 0) {
        ESP_LOGE(TAG, "listen: %s", strerror(errno));
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening rtsp://0.0.0.0:%d/cam0", port);

    while (1) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        ESP_LOGI(TAG, "accept viewer");
        handle_client(fd);
    }
}

esp_err_t minihub_rtsp_cam_relay_start(void)
{
    if (xTaskCreatePinnedToCore(relay_task, "rtsp_relay", 8192, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else

#include "esp_err.h"
esp_err_t minihub_rtsp_cam_relay_start(void)
{
    return ESP_OK;
}

#endif
