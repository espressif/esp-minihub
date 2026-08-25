/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-slot RTSP (TCP interleaved) → Annex-B H.265 writer on USB.
 */

#include "minihub_rtsp_cam.h"

#include "sdkconfig.h"

#if !CONFIG_MINIHUB_RTSP_CAM

esp_err_t minihub_rtsp_cam_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t minihub_rtsp_cam_config_set(const char *url, const char *cam_id, int segment_sec)
{
    (void)url; (void)cam_id; (void)segment_sec;
    return ESP_ERR_NOT_SUPPORTED;
}
void minihub_rtsp_cam_config_get(char *url, size_t url_len, char *cam_id, size_t cam_len,
                                 int *segment_sec)
{
    if (url && url_len) url[0] = '\0';
    if (cam_id && cam_len) cam_id[0] = '\0';
    if (segment_sec) *segment_sec = 0;
}
esp_err_t minihub_rtsp_cam_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t minihub_rtsp_cam_stop(void) { return ESP_OK; }
bool minihub_rtsp_cam_recording(void) { return false; }
const char *minihub_rtsp_cam_last_error(void) { return "disabled"; }
cJSON *minihub_rtsp_cam_status_json(void) { return cJSON_CreateObject(); }
cJSON *minihub_rtsp_cam_list_files_json(int limit)
{
    (void)limit;
    return cJSON_CreateArray();
}
cJSON *minihub_rtsp_cam_delete_files_json(const cJSON *names)
{
    (void)names;
    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "result", "fail");
        cJSON_AddStringToObject(o, "error", "disabled");
    }
    return o;
}
esp_err_t minihub_rtsp_cam_resolve_file(const char *name, char *path_out, size_t path_len)
{
    (void)name;
    if (path_out && path_len) {
        path_out[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t minihub_rtsp_cam_register_http(void *httpd_handle)
{
    (void)httpd_handle;
    return ESP_OK;
}

#else

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "minihub_core.h"
#include "minihub_usb_msc.h"
#include "rtsp_cam_internal.h"

static const char *TAG = "rtsp_cam";
static const char *NVS_NS = "rtsp_cam";

#define URL_MAX 192
#define CAM_ID_MAX 32
#define PATH_MAX_MH 384
#define RTSP_BUF 8192
#define FU_MAX (512 * 1024)
#define USB_IO_CHUNK 512
#define MH_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Prefer PSRAM; fall back to internal so boards without SPIRAM still work. */
static void *mh_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MH_PSRAM_CAPS);
    if (!p) {
        p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void *mh_realloc(void *ptr, size_t n)
{
    void *p = heap_caps_realloc(ptr, n, MH_PSRAM_CAPS);
    if (!p && n) {
        p = heap_caps_realloc(ptr, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

/* Small FatFs writes avoid multi-sector MSC URB growth under tight DMA heap. */
static int usb_fwrite_all(FILE *fp, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t left = len;
    while (left > 0) {
        size_t n = left > USB_IO_CHUNK ? USB_IO_CHUNK : left;
        if (fwrite(p, 1, n, fp) != n) {
            return -1;
        }
        p += n;
        left -= n;
    }
    return 0;
}
#define FAT32_SOFT_LIMIT (3ULL * 1024 * 1024 * 1024)
/* Keep at least this much free on the stick; delete oldest .h265 when below. */
#define USB_MIN_FREE_BYTES (256ULL * 1024 * 1024)
#define USB_RECLAIM_MAX_PASSES 64

#if CONFIG_MINIHUB_RTSP_CAM_AUTO_START
/* Defined later in this file; needed for unattended bring-up task. */
esp_err_t minihub_rtsp_cam_start(void);
bool minihub_rtsp_cam_recording(void);
const char *minihub_rtsp_cam_last_error(void);

static bool net_has_ipv4(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

static void autostart_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "autostart: waiting for USB + IPv4…");
    for (int i = 0; i < 180; i++) { /* ~3 min */
        bool usb = minihub_usb_msc_mounted();
        bool net = net_has_ipv4();
        if (usb && net) {
            esp_err_t err = minihub_rtsp_cam_start();
            if (err == ESP_OK) {
                /* Task may fail after start() returns — wait and verify. */
                for (int j = 0; j < 15; j++) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (minihub_rtsp_cam_recording()) {
                        ESP_LOGI(TAG, "autostart: recording confirmed");
                        vTaskDelete(NULL);
                        return;
                    }
                }
                ESP_LOGW(TAG, "autostart: start returned ok but not recording (%s); retry",
                         minihub_rtsp_cam_last_error());
            } else if (err == ESP_ERR_INVALID_STATE && minihub_rtsp_cam_recording()) {
                ESP_LOGI(TAG, "autostart: already recording");
                vTaskDelete(NULL);
                return;
            } else {
                ESP_LOGW(TAG, "autostart: start failed: %s (%s)",
                         esp_err_to_name(err), minihub_rtsp_cam_last_error());
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if ((i % 10) == 0) {
            ESP_LOGI(TAG, "autostart: usb=%d net=%d", (int)usb, (int)net);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "autostart: gave up waiting for USB/net/recording");
    vTaskDelete(NULL);
}
#endif

static void path_join2(char *out, size_t out_len, const char *a, const char *b)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool need_slash = (la > 0 && a[la - 1] != '/' && lb > 0 && b[0] != '/');
    size_t need = la + lb + (need_slash ? 1 : 0);
    if (need + 1 > out_len) {
        /* truncate safely */
        snprintf(out, out_len, "%s%s%s", a, need_slash ? "/" : "", b);
        return;
    }
    memcpy(out, a, la);
    size_t o = la;
    if (need_slash) {
        out[o++] = '/';
    }
    memcpy(out + o, b, lb);
    out[o + lb] = '\0';
}

typedef struct {
    char url[URL_MAX];
    char cam_id[CAM_ID_MAX];
    int segment_sec;
} rtsp_cfg_t;

typedef struct {
    volatile bool run;
    volatile bool want_record;
    volatile bool relay_want;
    volatile bool recording;
    volatile bool streaming;
    TaskHandle_t task;
    int sock;
    int viewer_sock;
    FILE *fp;
    uint64_t bytes_written;
    uint64_t segment_bytes;
    int64_t segment_start_ms;
    char current_file[PATH_MAX_MH];
    char last_error[128];
    char session[64];
    char sdp_cache[2048];
    int cseq;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    uint8_t *fu;
    size_t fu_len;
    uint8_t param_sets[512];
    size_t param_sets_len;
} rtsp_session_t;

static SemaphoreHandle_t s_lock;
static rtsp_cfg_t s_cfg;
static rtsp_session_t s_sess;

static int64_t now_ms(void)
{
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static const char *find_hdr_ci(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return NULL;
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

static void set_err(const char *msg)
{
    if (!msg) {
        s_sess.last_error[0] = '\0';
        return;
    }
    strncpy(s_sess.last_error, msg, sizeof(s_sess.last_error) - 1);
    s_sess.last_error[sizeof(s_sess.last_error) - 1] = '\0';
    ESP_LOGW(TAG, "%s", s_sess.last_error);
}

static void camera_point_id(char *out, size_t out_len)
{
    snprintf(out, out_len, "camera/%s", s_cfg.cam_id[0] ? s_cfg.cam_id : "cam0");
}

/** Publish Power (on=want_record) and Recording props for RainMaker/web. */
static void sync_camera_point(void)
{
    char pid[MINIHUB_POINT_ID_MAX];
    camera_point_id(pid, sizeof(pid));
    cJSON *props = cJSON_CreateObject();
    if (props) {
        cJSON_AddBoolToObject(props, "recording", s_sess.recording);
    }
    const char *val = (s_sess.want_record || s_sess.recording) ? "on" : "off";
    (void)minihub_point_set(pid, val, props);
    cJSON_Delete(props);
}

static esp_err_t register_camera_point(void)
{
    minihub_point_t pt = {0};
    camera_point_id(pt.point_id, sizeof(pt.point_id));
    strncpy(pt.type, "camera", sizeof(pt.type) - 1);
    strncpy(pt.adapter, "rtsp_cam", sizeof(pt.adapter) - 1);
    strncpy(pt.value, "off", sizeof(pt.value) - 1);
    pt.online = true;
    pt.props = cJSON_CreateObject();
    if (pt.props) {
        cJSON_AddBoolToObject(pt.props, "recording", false);
    }
    esp_err_t err = minihub_point_register(&pt);
    cJSON_Delete(pt.props);
    return err;
}

static esp_err_t rtsp_cam_command_handler(const minihub_command_t *cmd)
{
    if (!cmd || !cmd->cmd[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char pid[MINIHUB_POINT_ID_MAX];
    camera_point_id(pid, sizeof(pid));
    if (strcmp(cmd->point_id, pid) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (strcmp(cmd->cmd, "turn_on") == 0 || strcmp(cmd->cmd, "start") == 0) {
        err = minihub_rtsp_cam_start();
        if (err == ESP_ERR_INVALID_STATE && minihub_rtsp_cam_recording()) {
            err = ESP_OK;
        }
    } else if (strcmp(cmd->cmd, "turn_off") == 0 || strcmp(cmd->cmd, "stop") == 0) {
        err = minihub_rtsp_cam_stop();
    }
    if (err == ESP_OK) {
        sync_camera_point();
    }
    return err;
}

static esp_err_t cfg_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(s_cfg.url);
    (void)nvs_get_str(h, "url", s_cfg.url, &len);
    len = sizeof(s_cfg.cam_id);
    (void)nvs_get_str(h, "cam_id", s_cfg.cam_id, &len);
    int32_t seg = s_cfg.segment_sec;
    if (nvs_get_i32(h, "seg_sec", &seg) == ESP_OK) {
        if (seg >= 60 && seg <= 3600) {
            s_cfg.segment_sec = (int)seg;
        } else {
            /* Old lab values were ~20–30s; migrate to default 10 min. */
            s_cfg.segment_sec = CONFIG_MINIHUB_RTSP_CAM_DEFAULT_SEGMENT_SEC;
            ESP_LOGW(TAG, "NVS seg_sec=%ld out of range; using %d", (long)seg, s_cfg.segment_sec);
        }
    }
    nvs_close(h);
    return ESP_OK;
}

static esp_err_t cfg_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "url", s_cfg.url);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "cam_id", s_cfg.cam_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_i32(h, "seg_sec", s_cfg.segment_sec);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool parse_rtsp_url(const char *url, char *host, size_t host_len, int *port, char *path,
                           size_t path_len)
{
    if (!url || strncmp(url, "rtsp://", 7) != 0) {
        return false;
    }
    const char *p = url + 7;
    /* skip userinfo if present */
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) {
        p = at + 1;
    }
    const char *host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') {
        host_end++;
    }
    size_t hl = (size_t)(host_end - p);
    if (hl == 0 || hl >= host_len) {
        return false;
    }
    memcpy(host, p, hl);
    host[hl] = '\0';
    *port = 554;
    if (*host_end == ':') {
        *port = atoi(host_end + 1);
        while (*host_end && *host_end != '/') {
            host_end++;
        }
    }
    if (*host_end == '/') {
        strncpy(path, host_end, path_len - 1);
    } else {
        strncpy(path, "/", path_len - 1);
    }
    path[path_len - 1] = '\0';
    return true;
}

static int tcp_connect(const char *host, int port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sock;
}

static int rx_ensure(size_t need)
{
    if (s_sess.rx_cap >= need) {
        return 0;
    }
    size_t ncap = s_sess.rx_cap ? s_sess.rx_cap * 2 : RTSP_BUF;
    while (ncap < need) {
        ncap *= 2;
    }
    uint8_t *n = mh_realloc(s_sess.rx, ncap);
    if (!n) {
        return -1;
    }
    s_sess.rx = n;
    s_sess.rx_cap = ncap;
    return 0;
}

static int rx_append(const void *data, size_t len)
{
    if (rx_ensure(s_sess.rx_len + len + 1) != 0) {
        return -1;
    }
    memcpy(s_sess.rx + s_sess.rx_len, data, len);
    s_sess.rx_len += len;
    s_sess.rx[s_sess.rx_len] = 0;
    return 0;
}

static int rx_read_more(void)
{
    uint8_t tmp[2048];
    int n = recv(s_sess.sock, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        return n;
    }
    if (rx_append(tmp, (size_t)n) != 0) {
        return -1;
    }
    return n;
}

static void rx_consume(size_t n)
{
    if (n >= s_sess.rx_len) {
        s_sess.rx_len = 0;
        return;
    }
    memmove(s_sess.rx, s_sess.rx + n, s_sess.rx_len - n);
    s_sess.rx_len -= n;
    s_sess.rx[s_sess.rx_len] = 0;
}

static int wait_rtsp_response(char *hdr_out, size_t hdr_len, char *body_out, size_t body_len,
                              size_t *body_got)
{
    if (body_got) {
        *body_got = 0;
    }
    for (;;) {
        /* Skip interleaved frames while waiting for text */
        while (s_sess.rx_len >= 1 && s_sess.rx[0] == '$') {
            if (s_sess.rx_len < 4) {
                if (rx_read_more() <= 0) {
                    return -1;
                }
                continue;
            }
            uint16_t sz = ((uint16_t)s_sess.rx[2] << 8) | s_sess.rx[3];
            size_t need = 4u + sz;
            while (s_sess.rx_len < need) {
                if (rx_read_more() <= 0) {
                    return -1;
                }
            }
            rx_consume(need);
        }
        uint8_t *p = (uint8_t *)strstr((char *)s_sess.rx, "\r\n\r\n");
        if (!p) {
            if (rx_read_more() <= 0) {
                return -1;
            }
            continue;
        }
        size_t hlen = (size_t)(p - s_sess.rx) + 4;
        if (hlen >= hdr_len) {
            return -1;
        }
        memcpy(hdr_out, s_sess.rx, hlen);
        hdr_out[hlen] = '\0';
        rx_consume(hlen);

        size_t clen = 0;
        const char *cl = find_hdr_ci(hdr_out, "Content-Length:");
        if (cl) {
            clen = (size_t)atoi(cl + 15);
        }
        while (s_sess.rx_len < clen) {
            if (rx_read_more() <= 0) {
                return -1;
            }
        }
        if (body_out && body_len) {
            size_t copy = clen < body_len - 1 ? clen : body_len - 1;
            memcpy(body_out, s_sess.rx, copy);
            body_out[copy] = '\0';
            if (body_got) {
                *body_got = copy;
            }
        }
        rx_consume(clen);
        return 0;
    }
}

static int rtsp_request(const char *cmd_line, const char *extra_hdrs, char *hdr_out, size_t hdr_len,
                        char *body_out, size_t body_len)
{
    char req[640];
    int n = snprintf(req, sizeof(req),
                     "%s\r\nCSeq: %d\r\nUser-Agent: MiniHub-RTSP\r\n%s\r\n",
                     cmd_line, s_sess.cseq++,
                     extra_hdrs ? extra_hdrs : "");
    if (n <= 0 || n >= (int)sizeof(req)) {
        return -1;
    }
    if (send(s_sess.sock, req, (size_t)n, 0) < 0) {
        return -1;
    }
    size_t got = 0;
    if (wait_rtsp_response(hdr_out, hdr_len, body_out, body_len, &got) != 0) {
        return -1;
    }
    if (strncmp(hdr_out, "RTSP/1.0 200", 12) != 0) {
        ESP_LOGW(TAG, "RTSP fail: %.80s", hdr_out);
        return -1;
    }
    return 0;
}

static int b64_decode_append(uint8_t *dst, size_t dst_cap, size_t *dst_len, const char *b64)
{
    size_t out_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &out_len, (const unsigned char *)b64, strlen(b64));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && ret != 0) {
        return -1;
    }
    if (*dst_len + 4 + out_len > dst_cap) {
        return -1;
    }
    dst[(*dst_len)++] = 0x00;
    dst[(*dst_len)++] = 0x00;
    dst[(*dst_len)++] = 0x00;
    dst[(*dst_len)++] = 0x01;
    ret = mbedtls_base64_decode(dst + *dst_len, dst_cap - *dst_len, &out_len,
                                (const unsigned char *)b64, strlen(b64));
    if (ret != 0) {
        return -1;
    }
    *dst_len += out_len;
    return 0;
}

static void parse_sprop(const char *sdp)
{
    s_sess.param_sets_len = 0;
    const char *fmtp = strstr(sdp, "a=fmtp:96");
    if (!fmtp) {
        return;
    }
    const char *line_end = strstr(fmtp, "\r\n");
    char line[512];
    size_t ll = line_end ? (size_t)(line_end - fmtp) : strlen(fmtp);
    if (ll >= sizeof(line)) {
        ll = sizeof(line) - 1;
    }
    memcpy(line, fmtp, ll);
    line[ll] = '\0';

    static const char *keys[] = { "sprop-vps=", "sprop-sps=", "sprop-pps=" };
    for (size_t i = 0; i < 3; i++) {
        char *p = strstr(line, keys[i]);
        if (!p) {
            continue;
        }
        p += strlen(keys[i]);
        char *end = p;
        while (*end && *end != ';' && *end != ' ') {
            end++;
        }
        char tmp[256];
        size_t tl = (size_t)(end - p);
        if (tl >= sizeof(tmp)) {
            tl = sizeof(tmp) - 1;
        }
        memcpy(tmp, p, tl);
        tmp[tl] = '\0';
        /* may be comma-separated */
        char *tok = strtok(tmp, ",");
        while (tok) {
            (void)b64_decode_append(s_sess.param_sets, sizeof(s_sess.param_sets),
                                    &s_sess.param_sets_len, tok);
            tok = strtok(NULL, ",");
        }
    }
}

static void extract_header_value(const char *hdr, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    const char *p = find_hdr_ci(hdr, key);
    if (!p) {
        return;
    }
    p += strlen(key);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && *p != ';' && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

static esp_err_t mkdir_p(const char *path)
{
    char tmp[PATH_MAX_MH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                struct stat st;
                /* Mount roots (e.g. /usb0) often reject mkdir with EPERM/EINVAL. */
                if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    ESP_LOGW(TAG, "mkdir %s: %s", tmp, strerror(errno));
                    return ESP_FAIL;
                }
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "mkdir %s: %s", tmp, strerror(errno));
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool is_recording_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    return strcasecmp(dot, ".h265") == 0 || strcasecmp(dot, ".265") == 0 ||
           strcasecmp(dot, ".h26") == 0;
}

static bool is_safe_recording_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= 64) {
        return false;
    }
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        return false;
    }
    return is_recording_ext(name);
}

static esp_err_t cam_record_dir(char *dir, size_t dir_len)
{
    const char *root = minihub_usb_msc_mount_path();
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }
    char prefix[PATH_MAX_MH];
    path_join2(prefix, sizeof(prefix), root, "minihub");
    path_join2(dir, dir_len, prefix, s_cfg.cam_id);
    return ESP_OK;
}

esp_err_t minihub_rtsp_cam_resolve_file(const char *name, char *path_out, size_t path_len)
{
    if (!name || !path_out || path_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    path_out[0] = '\0';
    if (!is_safe_recording_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!minihub_usb_msc_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char dir[PATH_MAX_MH];
    if (cam_record_dir(dir, sizeof(dir)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    path_join2(path_out, path_len, dir, name);
    minihub_usb_msc_io_lock();
    struct stat st;
    int sr = stat(path_out, &st);
    minihub_usb_msc_io_unlock();
    if (sr != 0 || !S_ISREG(st.st_mode)) {
        path_out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

/* Returns free bytes, or -1 if unknown (FAT/USB VFS often lacks statvfs). */
static int64_t usb_free_bytes(const char *mount)
{
    struct statvfs st;
    if (!mount || statvfs(mount, &st) != 0) {
        return -1;
    }
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}

/* Delete one oldest recording under cam dir. Never deletes current_file.
 * Returns ESP_OK if deleted, ESP_ERR_NOT_FOUND if none left, else ESP_FAIL. */
static esp_err_t reclaim_oldest_one(void)
{
    const char *root = minihub_usb_msc_mount_path();
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }
    char dir[PATH_MAX_MH];
    char prefix[PATH_MAX_MH];
    path_join2(prefix, sizeof(prefix), root, "minihub");
    path_join2(dir, sizeof(dir), prefix, s_cfg.cam_id);

    minihub_usb_msc_io_lock();
    DIR *d = opendir(dir);
    if (!d) {
        minihub_usb_msc_io_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    char oldest_name[64] = {0};
    time_t oldest_mtime = 0;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' || !is_recording_ext(ent->d_name)) {
            continue;
        }
        char path[PATH_MAX_MH];
        path_join2(path, sizeof(path), dir, ent->d_name);
        if (s_sess.current_file[0] && strcmp(path, s_sess.current_file) == 0) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!found || st.st_mtime < oldest_mtime) {
            found = true;
            oldest_mtime = st.st_mtime;
            strncpy(oldest_name, ent->d_name, sizeof(oldest_name) - 1);
            oldest_name[sizeof(oldest_name) - 1] = '\0';
        }
    }
    closedir(d);

    if (!found) {
        minihub_usb_msc_io_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    char victim[PATH_MAX_MH];
    path_join2(victim, sizeof(victim), dir, oldest_name);
    if (unlink(victim) != 0) {
        ESP_LOGW(TAG, "unlink %s: %s", victim, strerror(errno));
        minihub_usb_msc_io_unlock();
        return ESP_FAIL;
    }
    minihub_usb_msc_io_unlock();
    ESP_LOGI(TAG, "USB reclaim: deleted %s", oldest_name);
    return ESP_OK;
}

/* Proactive: if free space known and below need_free, delete oldest until OK.
 * If free space unknown (no statvfs), skip — reclaim only on write/fopen failure. */
static esp_err_t ensure_usb_free(uint64_t need_free)
{
    const char *root = minihub_usb_msc_mount_path();
    if (!root) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t freeb = usb_free_bytes(root);
    if (freeb < 0) {
        return ESP_OK;
    }
    if ((uint64_t)freeb >= need_free) {
        return ESP_OK;
    }

    for (int pass = 0; pass < USB_RECLAIM_MAX_PASSES; pass++) {
        freeb = usb_free_bytes(root);
        if (freeb >= 0 && (uint64_t)freeb >= need_free) {
            return ESP_OK;
        }
        esp_err_t del = reclaim_oldest_one();
        if (del == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "USB low (%lld B free), no old recordings to delete", (long long)freeb);
            set_err("USB full");
            return ESP_ERR_NO_MEM;
        }
        if (del != ESP_OK) {
            return ESP_FAIL;
        }
    }

    freeb = usb_free_bytes(root);
    if (freeb >= 0 && (uint64_t)freeb >= need_free) {
        return ESP_OK;
    }
    set_err("USB full");
    return ESP_ERR_NO_MEM;
}

/* After write/fopen failure: delete oldest files (even without free-space API). */
static esp_err_t reclaim_after_write_fail(void)
{
    for (int i = 0; i < 8; i++) {
        esp_err_t del = reclaim_oldest_one();
        if (del == ESP_ERR_NOT_FOUND) {
            set_err("USB full");
            return ESP_ERR_NO_MEM;
        }
        if (del != ESP_OK) {
            return del;
        }
        int64_t freeb = usb_free_bytes(minihub_usb_msc_mount_path());
        if (freeb < 0 || (uint64_t)freeb >= USB_MIN_FREE_BYTES) {
            return ESP_OK;
        }
    }
    return ESP_OK;
}

static esp_err_t open_new_segment(void)
{
    const char *root = minihub_usb_msc_mount_path();
    if (!root) {
        set_err("USB not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    if (ensure_usb_free(USB_MIN_FREE_BYTES) != ESP_OK) {
        return ESP_FAIL;
    }
    char dir[PATH_MAX_MH];
    char prefix[PATH_MAX_MH];
    path_join2(prefix, sizeof(prefix), root, "minihub");
    path_join2(dir, sizeof(dir), prefix, s_cfg.cam_id);
    if (mkdir_p(dir) != ESP_OK) {
        set_err("mkdir failed");
        return ESP_FAIL;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char name[32];
    strftime(name, sizeof(name), "%Y%m%d_%H%M%S.h265", &tm);
    path_join2(s_sess.current_file, sizeof(s_sess.current_file), dir, name);

    minihub_usb_msc_io_lock();
    if (s_sess.fp) {
        fflush(s_sess.fp);
        fclose(s_sess.fp);
        s_sess.fp = NULL;
    }
    s_sess.fp = fopen(s_sess.current_file, "wb");
    if (!s_sess.fp) {
        /* ENOSPC or similar — reclaim and retry once. */
        ESP_LOGW(TAG, "fopen %s: %s", s_sess.current_file, strerror(errno));
        if (reclaim_after_write_fail() == ESP_OK) {
            s_sess.fp = fopen(s_sess.current_file, "wb");
        }
        if (!s_sess.fp) {
            ESP_LOGW(TAG, "fopen retry %s: %s", s_sess.current_file, strerror(errno));
            minihub_usb_msc_io_unlock();
            set_err("fopen failed");
            s_sess.current_file[0] = '\0';
            return ESP_FAIL;
        }
    }
    setvbuf(s_sess.fp, NULL, _IONBF, 0);
    if (s_sess.param_sets_len > 0) {
        if (usb_fwrite_all(s_sess.fp, s_sess.param_sets, s_sess.param_sets_len) != 0) {
            minihub_usb_msc_io_unlock();
            set_err("write param sets failed");
            return ESP_FAIL;
        }
        s_sess.bytes_written += s_sess.param_sets_len;
        s_sess.segment_bytes = s_sess.param_sets_len;
    } else {
        s_sess.segment_bytes = 0;
    }
    minihub_usb_msc_io_unlock();
    s_sess.segment_start_ms = now_ms();
    ESP_LOGI(TAG, "segment %s", s_sess.current_file);
    return ESP_OK;
}

static void close_segment(void)
{
    char path[PATH_MAX_MH];
    path[0] = '\0';
    uint64_t seg_bytes = s_sess.segment_bytes;
    minihub_usb_msc_io_lock();
    if (s_sess.fp) {
        fflush(s_sess.fp);
        fclose(s_sess.fp);
        s_sess.fp = NULL;
        if (s_sess.current_file[0]) {
            strncpy(path, s_sess.current_file, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        }
    }
    minihub_usb_msc_io_unlock();
    /* Drop empty leftovers (open then rotate/stop before any NAL). */
    if (path[0] && seg_bytes == 0) {
        minihub_usb_msc_io_lock();
        if (unlink(path) == 0) {
            ESP_LOGI(TAG, "removed empty segment %s", path);
        }
        minihub_usb_msc_io_unlock();
    }
}

static int write_nal(const uint8_t *nal, size_t len)
{
    if (!s_sess.want_record || !s_sess.fp || !nal || len == 0) {
        return 0;
    }
    static const uint8_t sc[4] = { 0x00, 0x00, 0x00, 0x01 };
    minihub_usb_msc_io_lock();
    if (usb_fwrite_all(s_sess.fp, sc, 4) == 0 && usb_fwrite_all(s_sess.fp, nal, len) == 0) {
        s_sess.bytes_written += 4 + len;
        s_sess.segment_bytes += 4 + len;
        minihub_usb_msc_io_unlock();
        return 0;
    }

    int e = errno;
    clearerr(s_sess.fp);
    minihub_usb_msc_io_unlock();

    /* Only reclaim on genuine no-space; transient USB I/O must not delete files. */
    if (e == ENOSPC) {
        ESP_LOGW(TAG, "write ENOSPC; reclaim + new segment");
        if (reclaim_after_write_fail() != ESP_OK) {
            set_err("USB full");
            return -1;
        }
        if (open_new_segment() != ESP_OK) {
            return -1;
        }
        minihub_usb_msc_io_lock();
        if (usb_fwrite_all(s_sess.fp, sc, 4) != 0 || usb_fwrite_all(s_sess.fp, nal, len) != 0) {
            minihub_usb_msc_io_unlock();
            set_err("USB write failed");
            return -1;
        }
        s_sess.bytes_written += 4 + len;
        s_sess.segment_bytes += 4 + len;
        minihub_usb_msc_io_unlock();
        return 0;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(30 + attempt * 40));
        minihub_usb_msc_io_lock();
        clearerr(s_sess.fp);
        if (usb_fwrite_all(s_sess.fp, sc, 4) == 0 && usb_fwrite_all(s_sess.fp, nal, len) == 0) {
            s_sess.bytes_written += 4 + len;
            s_sess.segment_bytes += 4 + len;
            minihub_usb_msc_io_unlock();
            ESP_LOGW(TAG, "USB write recovered after retry %d (%s)", attempt + 1, strerror(e));
            return 0;
        }
        e = errno;
        clearerr(s_sess.fp);
        minihub_usb_msc_io_unlock();
    }
    ESP_LOGW(TAG, "USB write failed (%s)", strerror(e));
    set_err("USB write failed");
    return -1;
}

static int handle_h265_payload(const uint8_t *payload, size_t len)
{
    if (len < 2) {
        return 0;
    }
    uint8_t nal_type = (payload[0] >> 1) & 0x3f;
    if (nal_type == 48) {
        /* Aggregation packet — skip v1 */
        return 0;
    }
    if (nal_type == 49) {
        if (len < 3) {
            return 0;
        }
        uint8_t fu = payload[2];
        bool start = (fu & 0x80) != 0;
        bool end = (fu & 0x40) != 0;
        uint8_t fu_type = fu & 0x3f;
        uint8_t hdr0 = (uint8_t)((payload[0] & 0x81) | (fu_type << 1));
        uint8_t hdr1 = payload[1];
        if (start) {
            s_sess.fu_len = 0;
            if (!s_sess.fu) {
                s_sess.fu = mh_malloc(FU_MAX);
                if (!s_sess.fu) {
                    ESP_LOGE(TAG, "FU buffer alloc %u failed", (unsigned)FU_MAX);
                    return -1;
                }
                ESP_LOGI(TAG, "FU buffer %u B (%s)", (unsigned)FU_MAX,
                         esp_ptr_external_ram(s_sess.fu) ? "PSRAM" : "internal");
            }
            if (2 + (len - 3) > FU_MAX) {
                return -1;
            }
            s_sess.fu[0] = hdr0;
            s_sess.fu[1] = hdr1;
            memcpy(s_sess.fu + 2, payload + 3, len - 3);
            s_sess.fu_len = 2 + (len - 3);
        } else if (s_sess.fu && s_sess.fu_len > 0) {
            if (s_sess.fu_len + (len - 3) > FU_MAX) {
                s_sess.fu_len = 0;
                return -1;
            }
            memcpy(s_sess.fu + s_sess.fu_len, payload + 3, len - 3);
            s_sess.fu_len += len - 3;
        }
        if (end && s_sess.fu_len > 0) {
            if (write_nal(s_sess.fu, s_sess.fu_len) != 0) {
                return -1;
            }
            s_sess.fu_len = 0;
        }
        return 0;
    }
    return write_nal(payload, len);
}

static uint16_t s_relay_seq;
static uint32_t s_relay_ts = 0;
static const uint32_t s_relay_ssrc = 0x4D484251; /* 'MHBQ' */
static bool s_viewer_wait_key; /* drop non-IDR until first keyframe after attach */

/* Returns true if the full interleaved frame was queued to TCP. */
static bool send_viewer_raw(const uint8_t *pkt, size_t need)
{
    int vs = s_sess.viewer_sock;
    if (vs < 0 || !pkt || need == 0) {
        return false;
    }
    size_t off = 0;
    for (int attempt = 0; attempt < 64 && off < need; attempt++) {
        int w = send(vs, pkt + off, need - off, MSG_DONTWAIT);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)) {
            vTaskDelay(1);
            continue;
        }
        ESP_LOGW(TAG, "viewer send failed: %s", strerror(errno));
        s_sess.viewer_sock = -1;
        return false;
    }
    if (off != need) {
        ESP_LOGW(TAG, "viewer send stalled (%u/%u)", (unsigned)off, (unsigned)need);
        s_sess.viewer_sock = -1;
        return false;
    }
    return true;
}

static bool send_rtp_nal_to_viewer(const uint8_t *nal, size_t nal_len, bool marker)
{
    if (!nal || nal_len == 0 || nal_len > 1400 || s_sess.viewer_sock < 0) {
        return false;
    }
    uint8_t pkt[4 + 12 + 1400];
    size_t rtp_len = 12 + nal_len;
    pkt[0] = '$';
    pkt[1] = 0;
    pkt[2] = (uint8_t)((rtp_len >> 8) & 0xff);
    pkt[3] = (uint8_t)(rtp_len & 0xff);
    pkt[4] = 0x80; /* V=2 */
    pkt[5] = (uint8_t)(0x60 | (marker ? 0x80 : 0)); /* M | PT=96 */
    pkt[6] = (uint8_t)((s_relay_seq >> 8) & 0xff);
    pkt[7] = (uint8_t)(s_relay_seq & 0xff);
    pkt[8] = (uint8_t)((s_relay_ts >> 24) & 0xff);
    pkt[9] = (uint8_t)((s_relay_ts >> 16) & 0xff);
    pkt[10] = (uint8_t)((s_relay_ts >> 8) & 0xff);
    pkt[11] = (uint8_t)(s_relay_ts & 0xff);
    pkt[12] = (uint8_t)((s_relay_ssrc >> 24) & 0xff);
    pkt[13] = (uint8_t)((s_relay_ssrc >> 16) & 0xff);
    pkt[14] = (uint8_t)((s_relay_ssrc >> 8) & 0xff);
    pkt[15] = (uint8_t)(s_relay_ssrc & 0xff);
    memcpy(pkt + 16, nal, nal_len);
    if (!send_viewer_raw(pkt, 4 + rtp_len)) {
        return false;
    }
    s_relay_seq++;
    return true;
}

static void inject_param_sets_to_viewer(void)
{
    if (s_sess.param_sets_len < 5 || s_sess.viewer_sock < 0) {
        return;
    }
    const uint8_t *p = s_sess.param_sets;
    size_t left = s_sess.param_sets_len;
    /* Collect NAL pointers first so only the last gets the RTP marker bit. */
    const uint8_t *nals[8];
    size_t nal_lens[8];
    size_t n_nals = 0;
    while (left >= 4 && n_nals < 8) {
        size_t sc = 0;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            sc = 4;
        } else if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            sc = 3;
        } else {
            p++;
            left--;
            continue;
        }
        p += sc;
        left -= sc;
        size_t nal_len = 0;
        while (nal_len + 3 < left) {
            if (p[nal_len] == 0 && p[nal_len + 1] == 0 &&
                (p[nal_len + 2] == 1 ||
                 (p[nal_len + 2] == 0 && nal_len + 3 < left && p[nal_len + 3] == 1))) {
                break;
            }
            nal_len++;
        }
        if (nal_len + 3 >= left) {
            nal_len = left;
        }
        if (nal_len > 0) {
            nals[n_nals] = p;
            nal_lens[n_nals] = nal_len;
            n_nals++;
        }
        p += nal_len;
        left -= nal_len;
    }
    for (size_t i = 0; i < n_nals; i++) {
        /* Marker on last param-set NAL so live555 flushes the AU to the packetizer. */
        if (!send_rtp_nal_to_viewer(nals[i], nal_lens[i], i + 1 == n_nals)) {
            return;
        }
    }
    ESP_LOGI(TAG, "injected param sets to viewer (%u B annex-B, %u NALs)",
             (unsigned)s_sess.param_sets_len, (unsigned)n_nals);
}

/* True if this RTP payload starts an IDR / CRA / BLA access unit. */
static bool h265_payload_is_key_start(const uint8_t *pay, size_t len)
{
    if (!pay || len < 2) {
        return false;
    }
    uint8_t nt = (uint8_t)((pay[0] >> 1) & 0x3f);
    if (nt == 19 || nt == 20 || nt == 21) {
        return true; /* IDR_W_RADL / IDR_N_LP / CRA */
    }
    if (nt == 49 && len >= 3) { /* FU-A: FuType in low 6 bits, S bit */
        uint8_t fu_nt = (uint8_t)(pay[2] & 0x3f);
        return (pay[2] & 0x80) != 0 && (fu_nt == 19 || fu_nt == 20 || fu_nt == 21);
    }
    return false;
}

/* Normalize camera RTP SSRC/seq so they match injected param-set packets. */
static void tee_interleaved_to_viewer(const uint8_t *pkt, size_t need)
{
    if (need < 16 || pkt[1] != 0) {
        return;
    }
    const uint8_t *rtp = pkt + 4;
    size_t rtp_len = need - 4;
    if (rtp_len < 12) {
        return;
    }
    uint8_t cc = rtp[0] & 0x0f;
    size_t hdr_len = 12u + 4u * cc;
    if (rtp[0] & 0x10) {
        if (rtp_len >= hdr_len + 4) {
            uint16_t ext_len = ((uint16_t)rtp[hdr_len + 2] << 8) | rtp[hdr_len + 3];
            hdr_len += 4u + 4u * ext_len;
        }
    }
    const uint8_t *pay = (rtp_len > hdr_len) ? (rtp + hdr_len) : NULL;
    size_t pay_len = (rtp_len > hdr_len) ? (rtp_len - hdr_len) : 0;

    if (s_viewer_wait_key) {
        if (!h265_payload_is_key_start(pay, pay_len)) {
            return; /* skip mid-GOP until IDR so decoder can start */
        }
        /* Align param-set timestamps with the keyframe access unit. */
        s_relay_ts = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) | ((uint32_t)rtp[6] << 8) |
                     (uint32_t)rtp[7];
        inject_param_sets_to_viewer();
        s_viewer_wait_key = false;
        ESP_LOGI(TAG, "viewer: first keyframe, start tee");
    }

    uint8_t tmp[4 + 12 + 2048];
    if (need > sizeof(tmp)) {
        return; /* drop oversized */
    }
    memcpy(tmp, pkt, need);
    tmp[12] = (uint8_t)((s_relay_ssrc >> 24) & 0xff);
    tmp[13] = (uint8_t)((s_relay_ssrc >> 16) & 0xff);
    tmp[14] = (uint8_t)((s_relay_ssrc >> 8) & 0xff);
    tmp[15] = (uint8_t)(s_relay_ssrc & 0xff);
    tmp[6] = (uint8_t)((s_relay_seq >> 8) & 0xff);
    tmp[7] = (uint8_t)(s_relay_seq & 0xff);
    s_relay_ts = ((uint32_t)tmp[8] << 24) | ((uint32_t)tmp[9] << 16) | ((uint32_t)tmp[10] << 8) |
                 (uint32_t)tmp[11];
    if (!send_viewer_raw(tmp, need)) {
        return;
    }
    s_relay_seq++;
}

static int process_interleaved(void)
{
    while (s_sess.rx_len >= 4 && s_sess.rx[0] == '$') {
        uint8_t ch = s_sess.rx[1];
        uint16_t sz = ((uint16_t)s_sess.rx[2] << 8) | s_sess.rx[3];
        size_t need = 4u + sz;
        if (s_sess.rx_len < need) {
            return 0;
        }
        tee_interleaved_to_viewer(s_sess.rx, need);
        const uint8_t *frame = s_sess.rx + 4;
        if (ch == 0 && sz >= 12 && s_sess.want_record) {
            uint8_t cc = frame[0] & 0x0f;
            size_t hdr_len = 12u + 4u * cc;
            if (frame[0] & 0x10) {
                if (sz >= hdr_len + 4) {
                    uint16_t ext_len = ((uint16_t)frame[hdr_len + 2] << 8) | frame[hdr_len + 3];
                    hdr_len += 4u + 4u * ext_len;
                }
            }
            if (sz > hdr_len) {
                if (handle_h265_payload(frame + hdr_len, sz - hdr_len) != 0) {
                    rx_consume(need);
                    return -1;
                }
            }
        }
        rx_consume(need);
    }
    /* discard stray non-$ text during PLAY */
    if (s_sess.rx_len > 0 && s_sess.rx[0] != '$') {
        uint8_t *d = memchr(s_sess.rx, '$', s_sess.rx_len);
        if (!d) {
            s_sess.rx_len = 0;
        } else {
            rx_consume((size_t)(d - s_sess.rx));
        }
    }
    return 0;
}

static esp_err_t maybe_rotate(void)
{
    if (!s_sess.fp) {
        return ESP_OK;
    }
    int64_t elapsed = now_ms() - s_sess.segment_start_ms;
    bool by_time = elapsed >= (int64_t)s_cfg.segment_sec * 1000;
    bool by_size = s_sess.segment_bytes >= FAT32_SOFT_LIMIT;
    if (!by_time && !by_size) {
        return ESP_OK;
    }
    return open_new_segment();
}

static void session_cleanup(void)
{
    if (s_sess.sock >= 0 && s_sess.session[0] && s_sess.run) {
        /* Only TEARDOWN if we were not force-stopped (socket may already be shut down). */
        char hdr[512];
        char line[256];
        snprintf(line, sizeof(line), "TEARDOWN %s RTSP/1.0", s_cfg.url);
        char extra[96];
        snprintf(extra, sizeof(extra), "Session: %s\r\n", s_sess.session);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(s_sess.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s_sess.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        (void)rtsp_request(line, extra, hdr, sizeof(hdr), NULL, 0);
    }
    if (s_sess.sock >= 0) {
        close(s_sess.sock);
        s_sess.sock = -1;
    }
    close_segment();
    s_sess.recording = false;
    s_sess.want_record = false;
    s_sess.streaming = false;
    s_sess.run = false;
    s_sess.session[0] = '\0';
}

static void record_task(void *arg)
{
    (void)arg;
    char host[64];
    char path[96];
    int port = 554;
    char hdr[1024];
    char body[2048];

    set_err(NULL);
    s_sess.bytes_written = 0;
    s_sess.current_file[0] = '\0';
    s_sess.session[0] = '\0';
    s_sess.sdp_cache[0] = '\0';
    s_sess.cseq = 1;
    s_sess.rx_len = 0;
    s_sess.fu_len = 0;
    s_sess.sock = -1;
    s_sess.streaming = false;
    s_sess.recording = false;

    if (s_sess.want_record && !minihub_usb_msc_mounted()) {
        set_err("USB not mounted");
        goto done;
    }
    if (!parse_rtsp_url(s_cfg.url, host, sizeof(host), &port, path, sizeof(path))) {
        set_err("bad RTSP URL");
        goto done;
    }

    s_sess.sock = tcp_connect(host, port);
    if (s_sess.sock < 0) {
        set_err("TCP connect failed");
        goto done;
    }

    char line[320];
    snprintf(line, sizeof(line), "DESCRIBE %s RTSP/1.0", s_cfg.url);
    if (rtsp_request(line, "Accept: application/sdp\r\n", hdr, sizeof(hdr), body, sizeof(body)) != 0) {
        set_err("DESCRIBE failed");
        goto done;
    }
    strncpy(s_sess.sdp_cache, body, sizeof(s_sess.sdp_cache) - 1);
    s_sess.sdp_cache[sizeof(s_sess.sdp_cache) - 1] = '\0';
    parse_sprop(body);
    if (strstr(body, "H265") == NULL && strstr(body, "H.265") == NULL) {
        ESP_LOGW(TAG, "SDP may not be H265; continuing anyway");
    }

    char content_base[200];
    extract_header_value(hdr, "Content-Base:", content_base, sizeof(content_base));
    if (!content_base[0]) {
        snprintf(content_base, sizeof(content_base), "%.190s/", s_cfg.url);
    }
    char track[64] = "track1";
    /* find control under video */
    const char *mvideo = strstr(body, "m=video");
    if (mvideo) {
        const char *c = strstr(mvideo, "a=control:");
        if (c) {
            c += 10;
            size_t i = 0;
            while (c[i] && c[i] != '\r' && c[i] != '\n' && i + 1 < sizeof(track)) {
                track[i] = c[i];
                i++;
            }
            track[i] = '\0';
        }
    }
    char setup_url[320];
    if (strncmp(track, "rtsp://", 7) == 0) {
        snprintf(setup_url, sizeof(setup_url), "%s", track);
    } else {
        path_join2(setup_url, sizeof(setup_url), content_base, track);
    }

    snprintf(line, sizeof(line), "SETUP %.280s RTSP/1.0", setup_url);
    if (rtsp_request(line, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n", hdr, sizeof(hdr),
                     NULL, 0) != 0) {
        set_err("SETUP failed");
        goto done;
    }
    extract_header_value(hdr, "Session:", s_sess.session, sizeof(s_sess.session));
    if (!s_sess.session[0]) {
        set_err("no Session");
        goto done;
    }

    if (s_sess.want_record) {
        if (open_new_segment() != ESP_OK) {
            goto done;
        }
    }

    char extra[128];
    snprintf(extra, sizeof(extra), "Session: %s\r\nRange: npt=0.000-\r\n", s_sess.session);
    snprintf(line, sizeof(line), "PLAY %s RTSP/1.0", s_cfg.url);
    if (rtsp_request(line, extra, hdr, sizeof(hdr), NULL, 0) != 0) {
        set_err("PLAY failed");
        goto done;
    }

    s_sess.streaming = true;
    if (s_sess.want_record) {
        s_sess.recording = true;
        ESP_LOGI(TAG, "recording %s", s_cfg.url);
        sync_camera_point();
    } else {
        ESP_LOGI(TAG, "streaming (relay) %s", s_cfg.url);
    }

    /* Short recv timeout so run=false is observed quickly; stop also shutdown()s the sock. */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s_sess.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (s_sess.run) {
        if (!s_sess.want_record && !s_sess.relay_want && s_sess.viewer_sock < 0) {
            ESP_LOGI(TAG, "upstream idle (no record, no viewer)");
            break;
        }
        /* Late start of recording while already streaming for a viewer. */
        if (s_sess.want_record && !s_sess.recording) {
            if (!minihub_usb_msc_mounted()) {
                set_err("USB not mounted");
                s_sess.want_record = false;
            } else if (open_new_segment() == ESP_OK) {
                s_sess.recording = true;
                ESP_LOGI(TAG, "recording attached to live upstream");
            } else {
                s_sess.want_record = false;
            }
            sync_camera_point();
        }
        if (!s_sess.want_record && s_sess.recording) {
            close_segment();
            s_sess.recording = false;
            ESP_LOGI(TAG, "recording stopped; upstream may continue for relay");
            sync_camera_point();
        }

        int n = rx_read_more();
        if (!s_sess.run) {
            break;
        }
        if (n == 0 || (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
            if (s_sess.run) {
                set_err("RTSP stream ended");
            }
            break;
        }
        if (n < 0) {
            continue;
        }
        if (process_interleaved() != 0) {
            set_err("write/demux error");
            if (s_sess.want_record) {
                break;
            }
            /* Relay-only: keep going after a demux glitch. */
        }
        if (s_sess.want_record && maybe_rotate() != ESP_OK) {
            break;
        }
        if (s_sess.want_record && !minihub_usb_msc_mounted()) {
            set_err("USB removed");
            s_sess.want_record = false;
            close_segment();
            s_sess.recording = false;
            if (s_sess.viewer_sock < 0) {
                break;
            }
        }
    }

done:
    session_cleanup();
    s_sess.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t minihub_rtsp_cam_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_cfg, 0, sizeof(s_cfg));
    strncpy(s_cfg.url, CONFIG_MINIHUB_RTSP_CAM_DEFAULT_URL, sizeof(s_cfg.url) - 1);
    strncpy(s_cfg.cam_id, "cam0", sizeof(s_cfg.cam_id) - 1);
    s_cfg.segment_sec = CONFIG_MINIHUB_RTSP_CAM_DEFAULT_SEGMENT_SEC;
    (void)cfg_load();
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.sock = -1;
    s_sess.viewer_sock = -1;
    s_sess.rx = mh_malloc(RTSP_BUF);
    if (s_sess.rx) {
        s_sess.rx_cap = RTSP_BUF;
        ESP_LOGI(TAG, "RTSP rx %u B (%s)", (unsigned)RTSP_BUF,
                 esp_ptr_external_ram(s_sess.rx) ? "PSRAM" : "internal");
    }
    ESP_LOGI(TAG, "init url=%s cam=%s seg=%ds", s_cfg.url, s_cfg.cam_id, s_cfg.segment_sec);
    if (register_camera_point() != ESP_OK) {
        ESP_LOGW(TAG, "camera point register failed");
    }
    if (minihub_command_register_adapter("rtsp_cam", rtsp_cam_command_handler) != ESP_OK) {
        ESP_LOGW(TAG, "camera command handler register failed");
    }
    if (minihub_rtsp_cam_relay_start() != ESP_OK) {
        ESP_LOGW(TAG, "RTSP relay start failed");
    }
#if CONFIG_MINIHUB_RTSP_CAM_AUTO_START
    if (xTaskCreate(autostart_task, "rtsp_auto", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "autostart task create failed");
    }
#endif
    return ESP_OK;
}

esp_err_t minihub_rtsp_cam_config_set(const char *url, const char *cam_id, int segment_sec)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_sess.recording || s_sess.task) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (url && url[0]) {
        strncpy(s_cfg.url, url, sizeof(s_cfg.url) - 1);
    }
    if (cam_id && cam_id[0]) {
        strncpy(s_cfg.cam_id, cam_id, sizeof(s_cfg.cam_id) - 1);
    }
    if (segment_sec >= 60 && segment_sec <= 3600) {
        s_cfg.segment_sec = segment_sec;
    } else if (segment_sec != 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = cfg_save();
    xSemaphoreGive(s_lock);
    return err;
}

void minihub_rtsp_cam_config_get(char *url, size_t url_len, char *cam_id, size_t cam_len,
                                 int *segment_sec)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (url && url_len) {
        strncpy(url, s_cfg.url, url_len - 1);
        url[url_len - 1] = '\0';
    }
    if (cam_id && cam_len) {
        strncpy(cam_id, s_cfg.cam_id, cam_len - 1);
        cam_id[cam_len - 1] = '\0';
    }
    if (segment_sec) {
        *segment_sec = s_cfg.segment_sec;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t minihub_rtsp_cam_start(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!minihub_usb_msc_mounted()) {
        set_err("USB not mounted");
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_sess.want_record = true;
    if (s_sess.task) {
        /* Upstream already running (e.g. relay); attach recording in task loop. */
        xSemaphoreGive(s_lock);
        sync_camera_point();
        return ESP_OK;
    }
    if (s_sess.recording) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_sess.run = true;
    if (xTaskCreatePinnedToCore(record_task, "rtsp_rec", 12288, NULL, 5, &s_sess.task, 1) !=
        pdPASS) {
        s_sess.run = false;
        s_sess.want_record = false;
        s_sess.task = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    sync_camera_point();
    return ESP_OK;
}

esp_err_t minihub_rtsp_cam_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sess.want_record = false;
    bool keep = (s_sess.viewer_sock >= 0) || s_sess.relay_want;
    if (!keep) {
        s_sess.run = false;
    }
    int sock = s_sess.sock;
    xSemaphoreGive(s_lock);

    if (!keep && sock >= 0) {
        (void)shutdown(sock, SHUT_RDWR);
    }

    if (!keep) {
        for (int i = 0; i < 50; i++) {
            if (!s_sess.task) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_sess.task) {
            ESP_LOGW(TAG, "stop: record task still running");
            return ESP_ERR_TIMEOUT;
        }
    } else {
        /* Wait briefly for recording flag to clear while relay continues. */
        for (int i = 0; i < 30; i++) {
            if (!s_sess.recording) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    sync_camera_point();
    return ESP_OK;
}

bool minihub_rtsp_cam_recording(void)
{
    return s_sess.recording;
}

const char *minihub_rtsp_cam_last_error(void)
{
    return s_sess.last_error[0] ? s_sess.last_error : NULL;
}

cJSON *minihub_rtsp_cam_status_json(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddBoolToObject(o, "usb_mounted", minihub_usb_msc_mounted());
    const char *mp = minihub_usb_msc_mount_path();
    if (mp) {
        cJSON_AddStringToObject(o, "mount_path", mp);
    }
    cJSON_AddBoolToObject(o, "recording", s_sess.recording);
    cJSON_AddBoolToObject(o, "streaming", s_sess.streaming);
    cJSON_AddNumberToObject(o, "relay_port", CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT);
    cJSON_AddNumberToObject(o, "relay_viewers", s_sess.viewer_sock >= 0 ? 1 : 0);
    {
        char relay[96] = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (!netif) {
            netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        }
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(relay, sizeof(relay), "rtsp://" IPSTR ":%d/%s", IP2STR(&ip.ip),
                     CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT, s_cfg.cam_id);
        } else {
            snprintf(relay, sizeof(relay), "rtsp://<hub-ip>:%d/%s",
                     CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT, s_cfg.cam_id);
        }
        cJSON_AddStringToObject(o, "relay_url", relay);
    }
    cJSON_AddStringToObject(o, "url", s_cfg.url);
    cJSON_AddStringToObject(o, "cam_id", s_cfg.cam_id);
    cJSON_AddNumberToObject(o, "segment_sec", s_cfg.segment_sec);
    cJSON_AddNumberToObject(o, "segment_min", s_cfg.segment_sec / 60);
    cJSON_AddStringToObject(o, "current_file", s_sess.current_file);
    cJSON_AddNumberToObject(o, "bytes_written", (double)s_sess.bytes_written);
    cJSON_AddNumberToObject(o, "segment_bytes", (double)s_sess.segment_bytes);
    if (s_sess.last_error[0]) {
        cJSON_AddStringToObject(o, "last_error", s_sess.last_error);
    }
    return o;
}

cJSON *minihub_rtsp_cam_list_files_json(int limit)
{
    if (limit <= 0) {
        limit = 20;
    }
    if (limit > 100) {
        limit = 100;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }
    const char *root = minihub_usb_msc_mount_path();
    if (!root) {
        return arr;
    }
    char dir[PATH_MAX_MH];
    char prefix[PATH_MAX_MH];
    path_join2(prefix, sizeof(prefix), root, "minihub");
    path_join2(dir, sizeof(dir), prefix, s_cfg.cam_id);
    minihub_usb_msc_io_lock();
    DIR *d = opendir(dir);
    if (!d) {
        minihub_usb_msc_io_unlock();
        return arr;
    }

    typedef struct {
        char name[64];
        int64_t size;
        time_t mtime;
    } file_ent_t;

    file_ent_t *ents = NULL;
    int total = 0;
    int cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!is_recording_ext(ent->d_name)) {
            continue;
        }
        char path[PATH_MAX_MH];
        path_join2(path, sizeof(path), dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (total >= cap) {
            int ncap = cap ? cap * 2 : 64;
            if (ncap > 1000) {
                ncap = 1000;
            }
            if (total >= ncap) {
                break;
            }
            file_ent_t *nents = mh_realloc(ents, (size_t)ncap * sizeof(file_ent_t));
            if (!nents) {
                break;
            }
            ents = nents;
            cap = ncap;
        }
        memset(&ents[total], 0, sizeof(ents[total]));
        strncpy(ents[total].name, ent->d_name, sizeof(ents[total].name) - 1);
        ents[total].size = (int64_t)st.st_size;
        ents[total].mtime = st.st_mtime;
        total++;
    }
    closedir(d);
    minihub_usb_msc_io_unlock();

    if (!ents) {
        return arr;
    }

    for (int i = 0; i < total; i++) {
        for (int j = i + 1; j < total; j++) {
            if (ents[j].mtime > ents[i].mtime) {
                file_ent_t tmp = ents[i];
                ents[i] = ents[j];
                ents[j] = tmp;
            }
        }
    }

    int n = total < limit ? total : limit;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", ents[i].name);
        int64_t size = ents[i].size;
        bool is_current = false;
        if (s_sess.recording && s_sess.current_file[0]) {
            char path[PATH_MAX_MH];
            path_join2(path, sizeof(path), dir, ents[i].name);
            if (strcmp(path, s_sess.current_file) == 0) {
                is_current = true;
                /* FatFs often keeps st_size=0 until fclose; use live counter. */
                size = (int64_t)s_sess.segment_bytes;
            }
        }
        cJSON_AddNumberToObject(item, "size", (double)size);
        cJSON_AddNumberToObject(item, "mtime", (double)ents[i].mtime);
        if (is_current) {
            cJSON_AddBoolToObject(item, "recording", true);
        }
        cJSON_AddItemToArray(arr, item);
    }
    free(ents);
    return arr;
}

cJSON *minihub_rtsp_cam_delete_files_json(const cJSON *names)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *deleted = cJSON_CreateArray();
    cJSON *failed = cJSON_CreateArray();
    if (!out || !deleted || !failed) {
        cJSON_Delete(out);
        cJSON_Delete(deleted);
        cJSON_Delete(failed);
        return NULL;
    }
    cJSON_AddItemToObject(out, "deleted", deleted);
    cJSON_AddItemToObject(out, "failed", failed);

    if (!minihub_usb_msc_mounted()) {
        cJSON_AddStringToObject(out, "result", "fail");
        cJSON_AddStringToObject(out, "error", "USB not mounted");
        return out;
    }
    if (!cJSON_IsArray(names)) {
        cJSON_AddStringToObject(out, "result", "fail");
        cJSON_AddStringToObject(out, "error", "names must be array");
        return out;
    }

    char dir[PATH_MAX_MH];
    if (cam_record_dir(dir, sizeof(dir)) != ESP_OK) {
        cJSON_AddStringToObject(out, "result", "fail");
        cJSON_AddStringToObject(out, "error", "USB not mounted");
        return out;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    minihub_usb_msc_io_lock();
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, names) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            continue;
        }
        const char *name = item->valuestring;
        cJSON *fail = cJSON_CreateObject();
        if (!fail) {
            break;
        }
        cJSON_AddStringToObject(fail, "name", name);

        if (!is_safe_recording_name(name)) {
            cJSON_AddStringToObject(fail, "error", "invalid name");
            cJSON_AddItemToArray(failed, fail);
            continue;
        }

        char path[PATH_MAX_MH];
        path_join2(path, sizeof(path), dir, name);
        if (s_sess.recording && s_sess.current_file[0] && strcmp(path, s_sess.current_file) == 0) {
            cJSON_AddStringToObject(fail, "error", "recording");
            cJSON_AddItemToArray(failed, fail);
            continue;
        }

        if (unlink(path) != 0) {
            cJSON_AddStringToObject(fail, "error", strerror(errno));
            cJSON_AddItemToArray(failed, fail);
            continue;
        }

        cJSON_Delete(fail);
        cJSON_AddItemToArray(deleted, cJSON_CreateString(name));
        ESP_LOGI(TAG, "deleted %s", name);
    }
    minihub_usb_msc_io_unlock();
    xSemaphoreGive(s_lock);

    cJSON_AddStringToObject(out, "result", "ok");
    return out;
}

int minihub_rtsp_cam_relay_port(void)
{
    return CONFIG_MINIHUB_RTSP_CAM_RELAY_PORT;
}

const char *minihub_rtsp_cam_id(void)
{
    return s_cfg.cam_id;
}

const char *minihub_rtsp_cam_cached_sdp(void)
{
    return s_sess.sdp_cache;
}

bool minihub_rtsp_cam_upstream_streaming(void)
{
    return s_sess.streaming;
}

bool minihub_rtsp_cam_viewer_busy(void)
{
    return s_sess.viewer_sock >= 0;
}

void minihub_rtsp_cam_viewer_attach(int sock)
{
    s_sess.viewer_sock = sock;
    s_sess.relay_want = true;
    s_viewer_wait_key = true;
    /* Param sets are injected again immediately before the first IDR in tee. */
    inject_param_sets_to_viewer();
}

void minihub_rtsp_cam_viewer_detach(int sock)
{
    if (s_sess.viewer_sock == sock || sock < 0) {
        s_sess.viewer_sock = -1;
    }
    s_sess.relay_want = false;
    if (!s_sess.want_record && s_sess.task) {
        s_sess.run = false;
        int us = s_sess.sock;
        if (us >= 0) {
            (void)shutdown(us, SHUT_RDWR);
        }
    }
}

esp_err_t minihub_rtsp_cam_ensure_upstream(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sess.relay_want = true;
    if (s_sess.task) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    s_sess.run = true;
    if (xTaskCreatePinnedToCore(record_task, "rtsp_rec", 12288, NULL, 5, &s_sess.task, 1) !=
        pdPASS) {
        s_sess.run = false;
        s_sess.relay_want = false;
        s_sess.task = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

#endif /* CONFIG_MINIHUB_RTSP_CAM */
