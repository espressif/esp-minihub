/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif
#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file esp_idf.h
 * @brief Plugin SDK — ESP-IDF export subset (err, log, HTTP client, NVS, heap, timer).
 *
 * Layout of ``esp_http_client_config_t`` matches MiniHub Host ESP-IDF ~6.2.
 * Each API carries an ``@note`` with its IDF header origin.
 * Full IDF documentation: ESP-IDF programming guide for the Host IDF version.
 */

/** @brief ESP-IDF error code (signed integer).
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
typedef int esp_err_t;

/** @brief Operation succeeded.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_OK                  0
/** @brief Generic failure.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_FAIL                (-1)
/** @brief Out of memory.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_NO_MEM          0x101
/** @brief Invalid argument.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_ARG     0x102
/** @brief Invalid state for operation.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_STATE   0x103
/** @brief Invalid size.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_SIZE    0x104
/** @brief Resource not found.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_NOT_FOUND       0x105
/** @brief Operation not supported.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_NOT_SUPPORTED   0x106
/** @brief Operation timed out.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_TIMEOUT         0x107
/** @brief Received response was invalid.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_RESPONSE 0x108
/** @brief CRC or checksum was invalid.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_CRC     0x109
/** @brief Version was invalid.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
#define ESP_ERR_INVALID_VERSION 0x10A

/** @brief NVS error base.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
#define ESP_ERR_NVS_BASE                 0x1100
/** @brief NVS entry / namespace not found.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
#define ESP_ERR_NVS_NOT_FOUND            (ESP_ERR_NVS_BASE + 0x02)
/** @brief NVS storage full.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE     (ESP_ERR_NVS_BASE + 0x05)
/** @brief NVS invalid handle.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
#define ESP_ERR_NVS_INVALID_HANDLE       (ESP_ERR_NVS_BASE + 0x07)
/** @brief NVS string/blob buffer too short.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
#define ESP_ERR_NVS_INVALID_LENGTH       (ESP_ERR_NVS_BASE + 0x0c)

/** @brief Convert ``esp_err_t`` to a string name.
 *  @note IDF: ``components/esp_common/include/esp_err.h`` */
const char *esp_err_to_name(esp_err_t code);
/** @brief Cryptographic-quality random 32-bit value.
 *  @note IDF: ``components/esp_hw_support/include/esp_random.h`` */
uint32_t esp_random(void);
/** @brief Fill buffer with random bytes.
 *  @note IDF: ``components/esp_hw_support/include/esp_random.h`` */
void esp_fill_random(void *buf, size_t len);
/** @brief Monotonic time in microseconds.
 *  @note IDF: ``components/esp_timer/include/esp_timer.h`` */
int64_t esp_timer_get_time(void);
/** @brief Attach ESP-IDF certificate bundle to an mbedTLS/esp-tls config.
 *  @note IDF: ``components/mbedtls/esp_crt_bundle/include/esp_crt_bundle.h`` */
esp_err_t esp_crt_bundle_attach(void *conf);

/** @brief Heap capability: executable memory.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_EXEC         (1 << 0)
/** @brief Heap capability: 32-bit accessible memory.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_32BIT        (1 << 1)
/** @brief Heap capability: 8-bit accessible memory.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_8BIT         (1 << 2)
/** @brief Heap capability: DMA-capable memory.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_DMA          (1 << 3)
/** @brief Heap capability: internal RAM.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_INTERNAL     (1 << 11)
/** @brief Default heap capability mask.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
#define MALLOC_CAP_DEFAULT      MALLOC_CAP_8BIT

/** @brief Free heap for the given capability mask.
 *  @note IDF: ``components/heap/include/esp_heap_caps.h`` */
size_t heap_caps_get_free_size(uint32_t caps);

/** @brief Log verbosity levels.
 *  @note IDF: ``components/log/include/esp_log.h`` */
typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR = 1,
    ESP_LOG_WARN = 2,
    ESP_LOG_INFO = 3,
    ESP_LOG_DEBUG = 4,
    ESP_LOG_VERBOSE = 5,
    ESP_LOG_MAX = 6,
} esp_log_level_t;

/** @brief Bit width of log level field in config word.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOG_LEVEL_LEN  3

#ifdef DOXYGEN
/** @brief Log config word (Host layout is a bitfield union; use ``ESP_LOG_CONFIG_INIT``).
 *  @note IDF: ``components/log/include/esp_log.h`` */
typedef struct {
    uint32_t data;
} esp_log_config_t;
#else
typedef struct {
    union {
        struct {
            esp_log_level_t log_level : ESP_LOG_LEVEL_LEN;
            uint32_t constrained_env : 1;
            uint32_t require_formatting : 1;
            uint32_t dis_color : 1;
            uint32_t dis_timestamp : 1;
            uint32_t binary_mode : 1;
            uint32_t reserved : 24;
        } opts;
        uint32_t data;
    };
} esp_log_config_t;
#endif

/** @brief Require formatted log output in config word.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOG_CONFIG_REQUIRE_FORMATTING  (1u << 4)
/** @brief Build log config from raw word.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOG_CONFIG_INIT(configs)       ((esp_log_config_t){ .data = (configs) })
/** @brief Log line format template.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define LOG_FORMAT(letter, format)         #letter " (%" "u" ") %s: " format "\n"

/** @brief Low-level log (prefer ``ESP_LOGI`` / ``ESP_LOGE`` macros).
 *  @note IDF: ``components/log/include/esp_log.h`` */
void esp_log(esp_log_config_t config, const char *tag, const char *format, ...);
/** @brief Millisecond timestamp for log prefix.
 *  @note IDF: ``components/log/include/esp_log_timestamp.h`` */
uint32_t esp_log_timestamp(void);

/** @brief Log error level.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOGE(tag, fmt, ...) \
    esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIG_REQUIRE_FORMATTING), \
            tag, LOG_FORMAT(E, fmt), (unsigned)esp_log_timestamp(), tag, ##__VA_ARGS__)
/** @brief Log warning level.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOGW(tag, fmt, ...) \
    esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_WARN | ESP_LOG_CONFIG_REQUIRE_FORMATTING), \
            tag, LOG_FORMAT(W, fmt), (unsigned)esp_log_timestamp(), tag, ##__VA_ARGS__)
/** @brief Log info level.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOGI(tag, fmt, ...) \
    esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_INFO | ESP_LOG_CONFIG_REQUIRE_FORMATTING), \
            tag, LOG_FORMAT(I, fmt), (unsigned)esp_log_timestamp(), tag, ##__VA_ARGS__)
/** @brief Log debug level.
 *  @note IDF: ``components/log/include/esp_log.h`` */
#define ESP_LOGD(tag, fmt, ...) \
    esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_DEBUG | ESP_LOG_CONFIG_REQUIRE_FORMATTING), \
            tag, LOG_FORMAT(D, fmt), (unsigned)esp_log_timestamp(), tag, ##__VA_ARGS__)

/** @brief Opaque NVS handle.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
typedef uint32_t nvs_handle_t;
/** @brief NVS open mode.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE,
    NVS_READWRITE_PURGE,
} nvs_open_mode_t;

/** @brief Open an NVS namespace.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle);
/** @brief Close an NVS handle.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
void nvs_close(nvs_handle_t handle);
/** @brief Commit pending NVS writes.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_commit(nvs_handle_t handle);
/** @brief Erase all keys in namespace.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_erase_all(nvs_handle_t handle);
/** @brief Erase one NVS key.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
/** @brief Read signed 32-bit integer from NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_get_i32(nvs_handle_t handle, const char *key, int32_t *out_value);
/** @brief Write signed 32-bit integer to NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_set_i32(nvs_handle_t handle, const char *key, int32_t value);
/** @brief Read 8-bit unsigned integer from NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value);
/** @brief Write 8-bit unsigned integer to NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value);
/** @brief Read string from NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length);
/** @brief Write string to NVS.
 *  @note IDF: ``components/nvs_flash/include/nvs.h`` */
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value);

/** @brief HTTP client handle (opaque).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef struct esp_http_client *esp_http_client_handle_t;
/** @brief Opaque DS/peripheral key config (forward declaration).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
struct esp_key_config_t;
/** @brief Network interface request (forward declaration).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
struct ifreq;

/** @brief HTTP client event identifiers.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_HEADERS_COMPLETE,
    HTTP_EVENT_ON_STATUS_CODE,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
    HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

/** @brief HTTP client event payload.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

#ifdef DOXYGEN
/** @brief HTTP event callback type.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef esp_err_t http_event_handle_cb(esp_http_client_event_t *evt);
#else
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);
#endif

/** @brief HTTP transport layer type.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    HTTP_TRANSPORT_UNKNOWN = 0,
    HTTP_TRANSPORT_OVER_TCP,
    HTTP_TRANSPORT_OVER_SSL,
} esp_http_client_transport_t;

/** @brief TLS protocol version selection.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    ESP_HTTP_CLIENT_TLS_VER_ANY = 0,
    ESP_HTTP_CLIENT_TLS_VER_TLS_1_2 = 0x1,
    ESP_HTTP_CLIENT_TLS_VER_TLS_1_3 = 0x2,
    ESP_HTTP_CLIENT_TLS_VER_MAX,
} esp_http_client_proto_ver_t;

/** @brief HTTP request method.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_NOTIFY,
    HTTP_METHOD_SUBSCRIBE,
    HTTP_METHOD_UNSUBSCRIBE,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_COPY,
    HTTP_METHOD_MOVE,
    HTTP_METHOD_LOCK,
    HTTP_METHOD_UNLOCK,
    HTTP_METHOD_PROPFIND,
    HTTP_METHOD_PROPPATCH,
    HTTP_METHOD_MKCOL,
    HTTP_METHOD_REPORT,
    HTTP_METHOD_MAX,
} esp_http_client_method_t;

/** @brief HTTP authentication scheme.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    HTTP_AUTH_TYPE_NONE = 0,
    HTTP_AUTH_TYPE_BASIC,
    HTTP_AUTH_TYPE_DIGEST,
} esp_http_client_auth_type_t;

/** @brief Preferred address family for HTTP client.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
typedef enum {
    HTTP_ADDR_TYPE_UNSPEC = AF_UNSPEC,
    HTTP_ADDR_TYPE_INET = AF_INET,
    HTTP_ADDR_TYPE_INET6 = AF_INET6,
} esp_http_client_addr_type_t;

/**
 * @brief HTTP client configuration.
 *
 * Layout frozen to MiniHub Host IDF 6.2 (HTTPS=y, DS peripheral=y).
 * @note IDF: ``components/esp_http_client/include/esp_http_client.h``
 */
typedef struct {
    const char *url;
    const char *host;
    int port;
    const char *username;
    const char *password;
    esp_http_client_auth_type_t auth_type;
    const char *path;
    const char *query;
#ifdef DOXYGEN
    const char *cert_pem; /**< PEM or DER via Host union; prefer PEM string. */
    const char *cert_der;
#else
    union {
        const char *cert_pem;
        const char *cert_der;
    };
#endif
    size_t cert_len;
#ifdef DOXYGEN
    const char *client_cert_pem;
    const char *client_cert_der;
#else
    union {
        const char *client_cert_pem;
        const char *client_cert_der;
    };
#endif
    size_t client_cert_len;
    const char *client_key_pem;
    size_t client_key_len;
    const struct esp_key_config_t *client_key;
    const char *client_key_password;
    size_t client_key_password_len;
    esp_http_client_proto_ver_t tls_version;
    const char *user_agent;
    esp_http_client_method_t method;
    int timeout_ms;
    bool disable_auto_redirect;
    int max_redirection_count;
    int max_authorization_retries;
    http_event_handle_cb event_handler;
    esp_http_client_transport_t transport_type;
    int buffer_size;
    int buffer_size_tx;
    void *user_data;
    bool is_async;
    bool use_global_ca_store;
    bool skip_cert_common_name_check;
    const char *common_name;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
    struct ifreq *if_name;
    const char **alpn_protos;
    void *ds_data;
    esp_http_client_addr_type_t addr_type;
} esp_http_client_config_t;

/** @brief Create HTTP client from config.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
/** @brief Perform HTTP request (blocking).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
/** @brief Destroy HTTP client and free resources.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
/** @brief Set request header.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
/** @brief Get response header value.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char *key, char **value);
/** @brief Set HTTP method.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
/** @brief Set POST body buffer.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
/** @brief Get HTTP response status code.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
int esp_http_client_get_status_code(esp_http_client_handle_t client);
/** @brief Content-Length, or -1 when unknown.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
/** @brief Open connection (``write_len`` is POST body size; 0 for GET).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
/** @brief Fetch response headers after ``open`` (content length, or -1).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
/** @brief Read a chunk of the response body (streaming; not ``perform``).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
/** @brief Close connection (handle remains usable until ``cleanup``).
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
/** @brief Get underlying socket errno after error.
 *  @note IDF: ``components/esp_http_client/include/esp_http_client.h`` */
int esp_http_client_get_errno(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
