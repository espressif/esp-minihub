/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * miIO LAN protocol (UDP 54321, AES-128-CBC). Compatible with MIoT get_properties/set_properties.
 * Reference: python-miio, Home Assistant xiaomi_miio.
 */

#include "miio_lan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"

static const char *TAG = "miio_lan";

static SemaphoreHandle_t s_miio_lock;
static uint32_t s_msg_id;

static esp_err_t miio_lock_init(void)
{
    if (!s_miio_lock) {
        s_miio_lock = xSemaphoreCreateMutex();
        if (!s_miio_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t miio_token_from_hex(const char *hex, uint8_t token[16])
{
    if (!hex || !token) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(hex) != 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < 16; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        token[i] = (uint8_t)v;
    }
    return ESP_OK;
}

static void miio_derive_key_iv(const uint8_t token[16], uint8_t key[16], uint8_t iv[16])
{
    /* python-miio: key = md5(token); iv = md5(key + token) */
    uint8_t buf[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), token, 16, key);
    memcpy(buf, key, 16);
    memcpy(buf + 16, token, 16);
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), buf, 32, iv);
}

static const uint8_t MIIO_HELLO_PKT[32] = {
    0x21, 0x31, 0x00, 0x20,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
};

static uint16_t miio_read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t miio_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void miio_write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void miio_write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xff);
}

static esp_err_t miio_parse_hello_reply(const uint8_t *pkt, size_t len, uint32_t *device_id,
                                        uint32_t *device_ts)
{
    if (!pkt || len != 32 || pkt[0] != 0x21 || pkt[1] != 0x31) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t total = miio_read_be16(pkt + 2);
    if (total != 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (device_id) {
        *device_id = miio_read_be32(pkt + 8);
    }
    if (device_ts) {
        *device_ts = miio_read_be32(pkt + 12);
    }
    return ESP_OK;
}

static esp_err_t miio_aes_cbc(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, size_t in_len,
                              uint8_t *out, size_t out_cap, size_t *out_len, bool encrypt)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_CBC_PKCS7);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 128);
    if (psa_import_key(&attributes, key, 16, &key_id) != PSA_SUCCESS) {
        psa_reset_key_attributes(&attributes);
        return ESP_FAIL;
    }
    psa_reset_key_attributes(&attributes);

    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status;
    size_t chunk = 0;
    size_t total = 0;

    if (encrypt) {
        if (in_len + 16 > out_cap) {
            psa_destroy_key(key_id);
            return ESP_ERR_NO_MEM;
        }
        status = psa_cipher_encrypt_setup(&op, key_id, PSA_ALG_CBC_PKCS7);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        status = psa_cipher_set_iv(&op, iv, 16);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        status = psa_cipher_update(&op, in, in_len, out, out_cap, &chunk);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        total = chunk;
        status = psa_cipher_finish(&op, out + total, out_cap - total, &chunk);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        total += chunk;
    } else {
        if (in_len == 0 || (in_len % 16) != 0 || in_len > out_cap) {
            psa_destroy_key(key_id);
            return ESP_ERR_INVALID_SIZE;
        }
        status = psa_cipher_decrypt_setup(&op, key_id, PSA_ALG_CBC_PKCS7);
        if (status != PSA_SUCCESS) {
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        status = psa_cipher_set_iv(&op, iv, 16);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        status = psa_cipher_update(&op, in, in_len, out, out_cap, &chunk);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_FAIL;
        }
        total = chunk;
        status = psa_cipher_finish(&op, out + total, out_cap - total, &chunk);
        if (status != PSA_SUCCESS) {
            psa_cipher_abort(&op);
            psa_destroy_key(key_id);
            return ESP_ERR_INVALID_RESPONSE;
        }
        total += chunk;
    }

    psa_destroy_key(key_id);
    *out_len = total;
    return ESP_OK;
}

static esp_err_t miio_build_packet(const uint8_t token[16], uint32_t device_id, uint32_t stamp,
                                   const char *json_plain, uint8_t **out_pkt, size_t *out_len)
{
    uint8_t key[16], iv[16];
    miio_derive_key_iv(token, key, iv);

    /* python-miio encrypts JSON + trailing NUL */
    size_t plain_len = strlen(json_plain) + 1;
    size_t enc_cap = plain_len + 32;
    uint8_t *encrypted = malloc(enc_cap);
    if (!encrypted) {
        return ESP_ERR_NO_MEM;
    }
    size_t enc_len = 0;
    esp_err_t err = miio_aes_cbc(key, iv, (const uint8_t *)json_plain, plain_len,
                                 encrypted, enc_cap, &enc_len, true);
    if (err != ESP_OK) {
        free(encrypted);
        return err;
    }

    size_t total = 32 + enc_len;
    uint8_t *pkt = malloc(total);
    if (!pkt) {
        free(encrypted);
        return ESP_ERR_NO_MEM;
    }

    pkt[0] = 0x21;
    pkt[1] = 0x31;
    miio_write_be16(pkt + 2, (uint16_t)total);
    memset(pkt + 4, 0, 4);
    miio_write_be32(pkt + 8, device_id);
    miio_write_be32(pkt + 12, stamp);

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_MD5), 0) != 0) {
        free(encrypted);
        return ESP_FAIL;
    }
    mbedtls_md_starts(&md);
    mbedtls_md_update(&md, pkt, 16);
    mbedtls_md_update(&md, token, 16);
    mbedtls_md_update(&md, encrypted, enc_len);
    mbedtls_md_finish(&md, pkt + 16);
    mbedtls_md_free(&md);

    memcpy(pkt + 32, encrypted, enc_len);
    free(encrypted);

    *out_pkt = pkt;
    *out_len = total;
    return ESP_OK;
}

static esp_err_t miio_parse_reply(const uint8_t token[16], const uint8_t *pkt, size_t pkt_len,
                                    uint32_t *device_id, char *json_out, size_t json_cap)
{
    if (pkt_len < 32 || pkt[0] != 0x21 || pkt[1] != 0x31) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t total = miio_read_be16(pkt + 2);
    if (total != pkt_len || total <= 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (device_id) {
        *device_id = miio_read_be32(pkt + 8);
    }

    uint8_t expect[16];
    uint8_t hdr[16];
    memcpy(hdr, pkt, 16);
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_MD5), 0) != 0) {
        mbedtls_md_free(&md);
        return ESP_FAIL;
    }
    mbedtls_md_starts(&md);
    mbedtls_md_update(&md, hdr, 16);
    mbedtls_md_update(&md, token, 16);
    mbedtls_md_update(&md, pkt + 32, total - 32);
    mbedtls_md_finish(&md, expect);
    mbedtls_md_free(&md);
    if (memcmp(expect, pkt + 16, 16) != 0) {
        ESP_LOGW(TAG, "miIO checksum mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t key[16], iv[16];
    miio_derive_key_iv(token, key, iv);
    uint8_t plain_buf[512];
    size_t plain_len = 0;
    esp_err_t err = miio_aes_cbc(key, iv, pkt + 32, total - 32,
                                 plain_buf, sizeof(plain_buf), &plain_len, false);
    if (err != ESP_OK) {
        return err;
    }
    while (plain_len > 0 && plain_buf[plain_len - 1] == '\0') {
        plain_len--;
    }
    if (plain_len >= json_cap) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(json_out, plain_buf, plain_len);
    json_out[plain_len] = '\0';
    return ESP_OK;
}

static esp_err_t miio_udp_exchange(const char *ip, const uint8_t *pkt, size_t pkt_len,
                                   uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MIIO_LAN_PORT);
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(sock, resp, resp_cap, 0, (struct sockaddr *)&from, &from_len);
    close(sock);
    if (n <= 0) {
        return ESP_ERR_TIMEOUT;
    }
    *resp_len = (size_t)n;
    return ESP_OK;
}

static esp_err_t miio_handshake(const char *ip, uint32_t *device_id, uint32_t *device_ts)
{
    if (!ip || !device_id || !device_ts) {
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MIIO_LAN_PORT);
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        close(sock);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 3; i++) {
        if (sendto(sock, MIIO_HELLO_PKT, sizeof(MIIO_HELLO_PKT), 0,
                   (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            close(sock);
            return ESP_FAIL;
        }
    }

    uint8_t resp[64];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(sock, resp, sizeof(resp), 0, (struct sockaddr *)&from, &from_len);
    close(sock);
    if (n <= 0) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = miio_parse_hello_reply(resp, (size_t)n, device_id, device_ts);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hello reply from %s invalid (%d bytes)", ip, (int)n);
    }
    return err;
}

static esp_err_t miio_lan_call(const char *ip, const uint8_t token[16], uint32_t *miio_id,
                               const char *method, cJSON *params, cJSON **out_result)
{
    if (!ip || !token || !method || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = miio_lock_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_miio_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t dev_id = 0;
    uint32_t dev_ts = 0;
    err = miio_handshake(ip, &dev_id, &dev_ts);
    if (err != ESP_OK) {
        xSemaphoreGive(s_miio_lock);
        return err;
    }
    if (miio_id && dev_id != 0 && dev_id != 0xFFFFFFFFU) {
        *miio_id = dev_id;
    }

    uint32_t msg_id = ++s_msg_id;
    if (msg_id == 0) {
        msg_id = (uint32_t)esp_random();
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddNumberToObject(req, "id", msg_id);
    cJSON_AddStringToObject(req, "method", method);
    if (params) {
        cJSON_AddItemToObject(req, "params", cJSON_Duplicate(params, 1));
    } else {
        cJSON_AddArrayToObject(req, "params");
    }

    char *json_plain = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json_plain) {
        xSemaphoreGive(s_miio_lock);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *pkt = NULL;
    size_t pkt_len = 0;
    uint32_t stamp = dev_ts + 1;
    if (stamp == 0) {
        stamp = (uint32_t)time(NULL);
    }
    err = miio_build_packet(token, dev_id, stamp, json_plain, &pkt, &pkt_len);
    free(json_plain);
    if (err != ESP_OK) {
        free(pkt);
        xSemaphoreGive(s_miio_lock);
        return err;
    }

    uint8_t resp_buf[1024];
    size_t resp_len = 0;
    err = miio_udp_exchange(ip, pkt, pkt_len, resp_buf, sizeof(resp_buf), &resp_len);
    free(pkt);
    if (err != ESP_OK) {
        xSemaphoreGive(s_miio_lock);
        return err;
    }

    char json_resp[768];
    uint32_t new_id = 0;
    err = miio_parse_reply(token, resp_buf, resp_len, &new_id, json_resp, sizeof(json_resp));
    if (err != ESP_OK) {
        xSemaphoreGive(s_miio_lock);
        return err;
    }
    if (miio_id && new_id != 0 && new_id != 0xFFFFFFFFU) {
        *miio_id = new_id;
    }

    cJSON *root = cJSON_Parse(json_resp);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON: %.120s", json_resp);
        xSemaphoreGive(s_miio_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *err_j = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(err_j)) {
        const cJSON *msg = cJSON_GetObjectItem(err_j, "message");
        ESP_LOGW(TAG, "miIO error: %s", cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        xSemaphoreGive(s_miio_lock);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result) {
        cJSON_Delete(root);
        xSemaphoreGive(s_miio_lock);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_result = cJSON_Duplicate(result, 1);
    cJSON_Delete(root);
    xSemaphoreGive(s_miio_lock);
    return *out_result ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miio_lan_get_property(const char *ip, const uint8_t token[16], uint32_t *miio_id,
                                int siid, int piid, cJSON **out_value)
{
    if (!out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = NULL;

    cJSON *params = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "siid", siid);
    cJSON_AddNumberToObject(item, "piid", piid);
    cJSON_AddItemToArray(params, item);

    cJSON *result = NULL;
    esp_err_t err = miio_lan_call(ip, token, miio_id, "get_properties", params, &result);
    cJSON_Delete(params);
    if (err != ESP_OK) {
        return err;
    }

    if (!cJSON_IsArray(result) || cJSON_GetArraySize(result) < 1) {
        cJSON_Delete(result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *first = cJSON_GetArrayItem(result, 0);
    const cJSON *code = cJSON_GetObjectItem(first, "code");
    if (cJSON_IsNumber(code) && code->valueint != 0) {
        ESP_LOGW(TAG, "get_properties code=%d siid=%d piid=%d", code->valueint, siid, piid);
        cJSON_Delete(result);
        return ESP_FAIL;
    }
    cJSON *value = cJSON_GetObjectItem(first, "value");
    if (!value) {
        cJSON_Delete(result);
        return ESP_ERR_NOT_FOUND;
    }
    *out_value = cJSON_Duplicate(value, 1);
    cJSON_Delete(result);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t miio_lan_set_property(const char *ip, const uint8_t token[16], uint32_t *miio_id,
                                int siid, int piid, const cJSON *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *params = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "siid", siid);
    cJSON_AddNumberToObject(item, "piid", piid);
    cJSON_AddItemToObject(item, "value", cJSON_Duplicate(value, 1));
    cJSON_AddItemToArray(params, item);

    cJSON *result = NULL;
    esp_err_t err = miio_lan_call(ip, token, miio_id, "set_properties", params, &result);
    cJSON_Delete(params);
    if (err != ESP_OK) {
        return err;
    }

    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        cJSON *first = cJSON_GetArrayItem(result, 0);
        const cJSON *code = cJSON_GetObjectItem(first, "code");
        if (cJSON_IsNumber(code) && code->valueint != 0) {
            ESP_LOGW(TAG, "set_properties code=%d siid=%d piid=%d", code->valueint, siid, piid);
            cJSON_Delete(result);
            return ESP_FAIL;
        }
    }
    cJSON_Delete(result);
    return ESP_OK;
}

static bool mac_equal(const char *a, const char *b)
{
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca == ':' || ca == '-') {
            a++;
            continue;
        }
        if (cb == ':' || cb == '-') {
            b++;
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool model_matches(const char *want, const char *seen)
{
    if (!want || !want[0] || !seen || !seen[0]) {
        return true;
    }
    return strcmp(want, seen) == 0 || strstr(seen, want) != NULL || strstr(want, seen) != NULL;
}

static bool discover_add_ip(miio_discovered_t *out, size_t max_out, size_t *count, const char *ip)
{
    if (!ip || !out || !count || *count >= max_out) {
        return false;
    }
    for (size_t i = 0; i < *count; i++) {
        if (strcmp(out[i].ip, ip) == 0) {
            return true;
        }
    }
    miio_discovered_t *slot = &out[*count];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->ip, ip, sizeof(slot->ip) - 1);
    (*count)++;
    return true;
}

esp_err_t miio_lan_discover(miio_discovered_t *out, size_t max_out, size_t *found,
                            uint32_t timeout_ms)
{
    if (!out || !found || max_out == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *found = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return ESP_FAIL;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bcast = {0};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(MIIO_LAN_PORT);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (int i = 0; i < 3; i++) {
        if (sendto(sock, MIIO_HELLO_PKT, sizeof(MIIO_HELLO_PKT), 0,
                   (struct sockaddr *)&bcast, sizeof(bcast)) < 0) {
            close(sock);
            return ESP_FAIL;
        }
    }

    uint32_t deadline = (uint32_t)esp_log_timestamp() + timeout_ms;
    uint8_t buf[64];

    while ((int32_t)(deadline - (uint32_t)esp_log_timestamp()) > 0) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }

        char ip[48];
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip))) {
            continue;
        }
        uint32_t dev_id = 0;
        uint32_t dev_ts = 0;
        if (miio_parse_hello_reply(buf, (size_t)n, &dev_id, &dev_ts) != ESP_OK) {
            continue;
        }
        discover_add_ip(out, max_out, found, ip);
    }

    close(sock);
    ESP_LOGI(TAG, "discover found %u miIO device(s)", (unsigned)*found);
    return ESP_OK;
}

esp_err_t miio_lan_probe(const char *ip, const uint8_t token[16], uint32_t *miio_id)
{
    if (!ip || !token) {
        return ESP_ERR_INVALID_ARG;
    }
    /* MIoT Spec devices (e.g. cuco.plug.v3) reject miio.info; use get_properties instead. */
    cJSON *params = cJSON_CreateArray();
    cJSON *prop = cJSON_CreateObject();
    if (!params || !prop) {
        cJSON_Delete(params);
        cJSON_Delete(prop);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(prop, "siid", 2);
    cJSON_AddNumberToObject(prop, "piid", 1);
    cJSON_AddItemToArray(params, prop);

    cJSON *result = NULL;
    esp_err_t err = miio_lan_call(ip, token, miio_id, "get_properties", params, &result);
    cJSON_Delete(params);
    if (result) {
        cJSON_Delete(result);
    }
    return err;
}

esp_err_t miio_lan_bind_ip(const uint8_t token[16], const char *model, const char *mac,
                           const miio_discovered_t *list, size_t list_count,
                           char *out_ip, size_t ip_len, uint32_t *miio_id)
{
    if (!token || !list || list_count == 0 || !out_ip || ip_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const miio_discovered_t *candidates[MIIO_DISCOVER_MAX];
    size_t cand_count = 0;
    const bool have_model = model && model[0];
    const bool have_mac = mac && mac[0];

    for (size_t i = 0; i < list_count && cand_count < MIIO_DISCOVER_MAX; i++) {
        const miio_discovered_t *d = &list[i];
        /* Only filter when discovery actually reported model/mac. */
        if (have_model && d->model[0] && !model_matches(model, d->model)) {
            continue;
        }
        if (have_mac && d->mac[0] && !mac_equal(mac, d->mac)) {
            continue;
        }
        candidates[cand_count++] = d;
    }

    if (cand_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Cap probes: each miss is ~3–6s of UDP timeout; never walk the whole LAN. */
    const size_t max_probes = 4;
    size_t probes = cand_count < max_probes ? cand_count : max_probes;
    if (probes < cand_count) {
        ESP_LOGW(TAG, "bind: probing %u/%u candidates (set ip= in adapters.json to skip)",
                 (unsigned)probes, (unsigned)cand_count);
    }
    for (size_t i = 0; i < probes; i++) {
        uint32_t id = miio_id ? *miio_id : 0xFFFFFFFFU;
        if (miio_lan_probe(candidates[i]->ip, token, &id) == ESP_OK) {
            strncpy(out_ip, candidates[i]->ip, ip_len - 1);
            out_ip[ip_len - 1] = '\0';
            if (miio_id) {
                *miio_id = id;
            }
            ESP_LOGI(TAG, "bound token to %s model=%s mac=%s",
                     out_ip,
                     candidates[i]->model[0] ? candidates[i]->model : "-",
                     candidates[i]->mac[0] ? candidates[i]->mac : "-");
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
