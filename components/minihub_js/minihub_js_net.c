/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host.net — IPv4 UDP datagrams (host.net.udp) and blocking-on-select TCP
 * streams (host.net.tcp, only when CONFIG_MINIHUB_JS_TCP). Both use base64
 * payloads; the TCP half is a small primitive for protocols such as RTSP
 * that are not HTTP.
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_JS_PLUGINS

#include "minihub_js_ext.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define JS_UDP_MAX_FDS    4
#define JS_UDP_RECV_MAX   2048
#define JS_TCP_MAX_FDS    4
#define JS_TCP_RECV_MAX   2048

/* --- host.net.udp ----------------------------------------------------- */

static int s_udp_fds[JS_UDP_MAX_FDS];
static bool s_udp_inited;

static void udp_table_init(void)
{
    if (s_udp_inited) {
        return;
    }
    for (int i = 0; i < JS_UDP_MAX_FDS; i++) {
        s_udp_fds[i] = -1;
    }
    s_udp_inited = true;
}

static int udp_slot_alloc(int fd)
{
    udp_table_init();
    for (int i = 0; i < JS_UDP_MAX_FDS; i++) {
        if (s_udp_fds[i] < 0) {
            s_udp_fds[i] = fd;
            return i;
        }
    }
    return -1;
}

static int udp_slot_fd(int slot)
{
    udp_table_init();
    if (slot < 0 || slot >= JS_UDP_MAX_FDS) {
        return -1;
    }
    return s_udp_fds[slot];
}

static void udp_slot_clear(int slot)
{
    if (slot >= 0 && slot < JS_UDP_MAX_FDS) {
        s_udp_fds[slot] = -1;
    }
}

static JSValue js_udp_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    bool broadcast = false;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue b = JS_GetPropertyStr(ctx, argv[0], "broadcast");
        broadcast = JS_ToBool(ctx, b);
        JS_FreeValue(ctx, b);
    }

    int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return JS_ThrowInternalError(ctx, "udp socket failed");
    }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (broadcast) {
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(0);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(fd);
        return JS_ThrowInternalError(ctx, "udp bind failed");
    }
    int slot = udp_slot_alloc(fd);
    if (slot < 0) {
        close(fd);
        return JS_ThrowInternalError(ctx, "too many udp sockets");
    }
    return JS_NewInt32(ctx, slot);
}

static JSValue js_udp_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "close(fd)");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd >= 0) {
        close(fd);
        udp_slot_clear(slot);
    }
    return JS_UNDEFINED;
}

static JSValue js_udp_sendto(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "sendto(fd, {ip,port,data_b64})");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowTypeError(ctx, "invalid udp fd");
    }
    const char *ip = js_opt_str(ctx, argv[1], "ip");
    const char *data_b64 = js_opt_str(ctx, argv[1], "data_b64");
    JSValue port_v = JS_GetPropertyStr(ctx, argv[1], "port");
    int32_t port = 0;
    JS_ToInt32(ctx, &port, port_v);
    JS_FreeValue(ctx, port_v);
    if (!ip || !data_b64 || port <= 0 || port > 65535) {
        if (ip) {
            JS_FreeCString(ctx, ip);
        }
        if (data_b64) {
            JS_FreeCString(ctx, data_b64);
        }
        return JS_ThrowTypeError(ctx, "ip/port/data_b64 required");
    }
    size_t raw_len = 0;
    uint8_t *raw = b64_decode_alloc(data_b64, &raw_len);
    JS_FreeCString(ctx, data_b64);
    if (!raw) {
        JS_FreeCString(ctx, ip);
        return JS_ThrowTypeError(ctx, "bad data_b64");
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_aton(ip, &to.sin_addr) == 0) {
        free(raw);
        JS_FreeCString(ctx, ip);
        return JS_ThrowTypeError(ctx, "bad ip");
    }
    JS_FreeCString(ctx, ip);
    ssize_t n = sendto(fd, raw, raw_len, 0, (struct sockaddr *)&to, sizeof(to));
    free(raw);
    if (n < 0) {
        return JS_ThrowInternalError(ctx, "sendto failed");
    }
    return JS_NewInt32(ctx, (int)n);
}

static JSValue js_udp_recvfrom(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[1])) {
        return JS_ThrowTypeError(ctx, "recvfrom(fd, {timeout_ms,max_len?})");
    }
    int32_t slot = 0;
    JS_ToInt32(ctx, &slot, argv[0]);
    int fd = udp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowTypeError(ctx, "invalid udp fd");
    }
    int32_t timeout_ms = 2000;
    int32_t max_len = JS_UDP_RECV_MAX;
    JSValue t = JS_GetPropertyStr(ctx, argv[1], "timeout_ms");
    if (JS_IsNumber(t)) {
        JS_ToInt32(ctx, &timeout_ms, t);
    }
    JS_FreeValue(ctx, t);
    JSValue m = JS_GetPropertyStr(ctx, argv[1], "max_len");
    if (JS_IsNumber(m)) {
        JS_ToInt32(ctx, &max_len, m);
        if (max_len <= 0 || max_len > JS_UDP_RECV_MAX) {
            max_len = JS_UDP_RECV_MAX;
        }
    }
    JS_FreeValue(ctx, m);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t *buf = malloc((size_t)max_len);
    if (!buf) {
        return JS_ThrowOutOfMemory(ctx);
    }
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, buf, (size_t)max_len, 0, (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        free(buf);
        return JS_NULL;
    }
    char *b64 = b64_encode_alloc(buf, (size_t)n);
    free(buf);
    if (!b64) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ip", JS_NewString(ctx, inet_ntoa(from.sin_addr)));
    JS_SetPropertyStr(ctx, obj, "port", JS_NewInt32(ctx, (int)ntohs(from.sin_port)));
    JS_SetPropertyStr(ctx, obj, "data_b64", JS_NewString(ctx, b64));
    free(b64);
    return obj;
}

/* --- host.net.tcp ----------------------------------------------------- */

#if !CONFIG_MINIHUB_JS_TCP
static JSValue js_tcp_disabled(JSContext *ctx)
{
    return JS_ThrowInternalError(ctx, "host.net.tcp disabled (CONFIG_MINIHUB_JS_TCP)");
}
#endif

static int s_tcp_fds[JS_TCP_MAX_FDS];
static bool s_tcp_inited;

static void tcp_table_init(void)
{
    if (!s_tcp_inited) {
        for (int i = 0; i < JS_TCP_MAX_FDS; i++) {
            s_tcp_fds[i] = -1;
        }
        s_tcp_inited = true;
    }
}

static int tcp_slot_alloc(int fd)
{
    tcp_table_init();
    for (int i = 0; i < JS_TCP_MAX_FDS; i++) {
        if (s_tcp_fds[i] < 0) {
            s_tcp_fds[i] = fd;
            return i;
        }
    }
    return -1;
}

static int tcp_slot_fd(int slot)
{
    tcp_table_init();
    return (slot >= 0 && slot < JS_TCP_MAX_FDS) ? s_tcp_fds[slot] : -1;
}

static void tcp_slot_clear(int slot)
{
    if (slot >= 0 && slot < JS_TCP_MAX_FDS) {
        s_tcp_fds[slot] = -1;
    }
}

static JSValue js_tcp_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_JS_TCP
    (void)argc;
    (void)argv;
    return js_tcp_disabled(ctx);
#else
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "tcp.connect({ host, port })");
    }
    char host[64] = "";
    js_opt_str_buf(ctx, argv[0], "host", host, sizeof(host));
    int port = js_opt_int(ctx, argv[0], "port", 554);
    int timeout_ms = js_opt_int(ctx, argv[0], "timeout_ms", 3000);
    if (host[0] == '\0' || port <= 0 || port > 65535) {
        return JS_ThrowTypeError(ctx, "tcp.connect: host/port invalid");
    }
    if (timeout_ms < 100) {
        timeout_ms = 100;
    }
    if (timeout_ms > 30000) {
        timeout_ms = 30000;
    }

    int fd = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return JS_ThrowInternalError(ctx, "tcp.connect: socket failed");
    }
    int slot = tcp_slot_alloc(fd);
    if (slot < 0) {
        close(fd);
        return JS_ThrowInternalError(ctx, "tcp.connect: no free slot");
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    dst.sin_addr.s_addr = inet_addr(host);

    int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        tcp_slot_clear(slot);
        return JS_ThrowInternalError(ctx, "tcp.connect: connect failed (%d)", errno);
    }
    if (rc != 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(fd + 1, NULL, &wset, NULL, &tv);
        int soerr = 0;
        socklen_t slen = sizeof(soerr);
        if (rc > 0) {
            (void)getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen);
        }
        if (rc <= 0 || soerr != 0) {
            close(fd);
            tcp_slot_clear(slot);
            return JS_ThrowInternalError(ctx, "tcp.connect: %s",
                                         rc <= 0 ? "timeout" : strerror(soerr));
        }
    }
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags);
    }
    struct timeval io_tv = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    return JS_NewInt32(ctx, slot);
#endif
}

static JSValue js_tcp_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_JS_TCP
    (void)argc;
    (void)argv;
    return js_tcp_disabled(ctx);
#else
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "tcp.send(fd, { data_b64 })");
    }
    int32_t slot = -1;
    if (JS_ToInt32(ctx, &slot, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    int fd = tcp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowRangeError(ctx, "tcp.send: bad fd");
    }
    const char *b64 = js_opt_str(ctx, argv[1], "data_b64");
    size_t len = 0;
    uint8_t *data = b64_decode_alloc(b64, &len);
    if (b64) {
        JS_FreeCString(ctx, b64);
    }
    if (!data) {
        return JS_ThrowTypeError(ctx, "tcp.send: data_b64 invalid");
    }
    ssize_t sent = send(fd, data, len, 0);
    free(data);
    if (sent < 0) {
        return JS_ThrowInternalError(ctx, "tcp.send: send failed (%d)", errno);
    }
    return JS_NewInt32(ctx, (int)sent);
#endif
}

static JSValue js_tcp_recv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_JS_TCP
    (void)argc;
    (void)argv;
    return js_tcp_disabled(ctx);
#else
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "tcp.recv(fd, { timeout_ms, max_len? })");
    }
    int32_t slot = -1;
    if (JS_ToInt32(ctx, &slot, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    int fd = tcp_slot_fd(slot);
    if (fd < 0) {
        return JS_ThrowRangeError(ctx, "tcp.recv: bad fd");
    }
    JSValueConst opts = argc >= 2 ? argv[1] : JS_UNDEFINED;
    int max_len = js_opt_int(ctx, opts, "max_len", JS_TCP_RECV_MAX);
    int timeout_ms = js_opt_int(ctx, opts, "timeout_ms", 0);
    if (max_len < 1) {
        max_len = 1;
    }
    if (max_len > JS_TCP_RECV_MAX) {
        max_len = JS_TCP_RECV_MAX;
    }
    if (timeout_ms > 0) {
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    uint8_t *buf = malloc((size_t)max_len);
    if (!buf) {
        return JS_ThrowOutOfMemory(ctx);
    }
    ssize_t n = recv(fd, buf, (size_t)max_len, 0);
    if (n <= 0) {
        free(buf);
        return n == 0 ? JS_NULL : JS_UNDEFINED;  /* closed / timeout or error */
    }
    char *b64 = b64_encode_alloc(buf, (size_t)n);
    free(buf);
    if (!b64) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "data_b64", JS_NewString(ctx, b64));
    JS_SetPropertyStr(ctx, out, "len", JS_NewInt32(ctx, (int)n));
    free(b64);
    return out;
#endif
}

static JSValue js_tcp_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
#if !CONFIG_MINIHUB_JS_TCP
    (void)argc;
    (void)argv;
    return js_tcp_disabled(ctx);
#else
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    int32_t slot = -1;
    if (JS_ToInt32(ctx, &slot, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    int fd = tcp_slot_fd(slot);
    if (fd >= 0) {
        close(fd);
        tcp_slot_clear(slot);
    }
    return JS_UNDEFINED;
#endif
}

void minihub_js_attach_net(JSContext *ctx, JSValue host)
{
    JSValue net = JS_NewObject(ctx);

    JSValue udp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, udp, "open", JS_NewCFunction(ctx, js_udp_open, "open", 1));
    JS_SetPropertyStr(ctx, udp, "close", JS_NewCFunction(ctx, js_udp_close, "close", 1));
    JS_SetPropertyStr(ctx, udp, "sendto", JS_NewCFunction(ctx, js_udp_sendto, "sendto", 2));
    JS_SetPropertyStr(ctx, udp, "recvfrom", JS_NewCFunction(ctx, js_udp_recvfrom, "recvfrom", 2));
    JS_SetPropertyStr(ctx, net, "udp", udp);

    JSValue tcp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, tcp, "connect", JS_NewCFunction(ctx, js_tcp_connect, "connect", 1));
    JS_SetPropertyStr(ctx, tcp, "send", JS_NewCFunction(ctx, js_tcp_send, "send", 2));
    JS_SetPropertyStr(ctx, tcp, "recv", JS_NewCFunction(ctx, js_tcp_recv, "recv", 2));
    JS_SetPropertyStr(ctx, tcp, "close", JS_NewCFunction(ctx, js_tcp_close, "close", 1));
    JS_SetPropertyStr(ctx, net, "tcp", tcp);

    JS_SetPropertyStr(ctx, host, "net", net);
}

#endif /* CONFIG_MINIHUB_JS_PLUGINS */
