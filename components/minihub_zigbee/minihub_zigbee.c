/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_zigbee.h"
#include "zigbee_nvs.h"
#include "zigbee_stack.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "minihub_core.h"
#include "minihub_task.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_zigbee";

#define ZB_DEVICE_Q_LEN 4

static minihub_zigbee_device_t s_devices[MINIHUB_ZB_MAX_DEVICES];
static size_t s_device_count;
static minihub_zigbee_device_cb_t s_user_device_cb;
static minihub_zigbee_attr_cb_t s_user_attr_cb;
static void *s_user_ctx;
static bool s_inited;
static QueueHandle_t s_device_q;
static TaskHandle_t s_device_task;

static void apply_device(const minihub_zigbee_device_t *dev)
{
    esp_err_t err = minihub_zb_nvs_upsert(dev, s_devices, &s_device_count, MINIHUB_ZB_MAX_DEVICES);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "upsert failed: %s", esp_err_to_name(err));
    }
    if (s_user_device_cb) {
        s_user_device_cb(dev, s_user_ctx);
    }
}

static void device_worker(void *arg)
{
    (void)arg;
    minihub_zigbee_device_t dev;
    for (;;) {
        if (xQueueReceive(s_device_q, &dev, portMAX_DELAY) == pdTRUE) {
            apply_device(&dev);
        }
    }
}

static void on_stack_device(const minihub_zigbee_device_t *dev, void *ctx)
{
    (void)ctx;
    if (!dev) {
        return;
    }
    /* NVS + RainMaker must not run inside Zigbee ZDO callbacks (flash cache assert). */
    if (s_device_q && xQueueSend(s_device_q, dev, 0) == pdTRUE) {
        return;
    }
    ESP_LOGW(TAG, "device queue full/unavailable — applying inline");
    apply_device(dev);
}

static void on_stack_attr(const minihub_zigbee_attr_evt_t *evt, void *ctx)
{
    (void)ctx;
    if (s_user_attr_cb && evt) {
        s_user_attr_cb(evt, s_user_ctx);
    }
}

static void on_stack_ready(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stack ready — resubscribe/rediscover %u mapped devices", (unsigned)s_device_count);
    for (size_t i = 0; i < s_device_count; i++) {
        uint8_t caps = s_devices[i].caps;
        if ((caps & MINIHUB_ZB_CAP_SENSOR_MASK) || (caps & MINIHUB_ZB_CAP_METERING_MASK)) {
            (void)zigbee_stack_subscribe(s_devices[i].short_addr, s_devices[i].endpoint,
                                         s_devices[i].ieee, caps);
        } else if (caps & MINIHUB_ZB_CAP_ONOFF) {
            /* Firmware may have added metering after first join — refresh Simple Desc. */
            ESP_LOGI(TAG, "rediscover 0x%04x for metering caps refresh", s_devices[i].short_addr);
            zigbee_stack_rediscover(s_devices[i].short_addr, s_devices[i].ieee);
        }
        zigbee_stack_subscribe_product_params(s_devices[i].short_addr, s_devices[i].endpoint,
                                              s_devices[i].ieee);
    }
}

esp_err_t minihub_zigbee_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    esp_err_t err = minihub_zb_nvs_load(s_devices, MINIHUB_ZB_MAX_DEVICES, &s_device_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS load: %s", esp_err_to_name(err));
        s_device_count = 0;
    }

    s_device_q = xQueueCreate(ZB_DEVICE_Q_LEN, sizeof(minihub_zigbee_device_t));
    if (s_device_q && !s_device_task) {
        /* NVS cJSON_Print + point.registered handlers need headroom beyond 4K. */
        BaseType_t ok = minihub_task_create_psram(device_worker, "zb_dev", 6144, NULL, 5, &s_device_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create zb_dev worker");
            s_device_task = NULL;
        }
    }

    zigbee_stack_set_hooks(on_stack_device, on_stack_attr, on_stack_ready, NULL);
    s_inited = true;
    return ESP_OK;
}

esp_err_t minihub_zigbee_start(void)
{
    esp_err_t err = minihub_zigbee_init();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_MINIHUB_ZIGBEE_SDK
    return zigbee_stack_start();
#else
    ESP_LOGI(TAG, "SDK disabled (CONFIG_MINIHUB_ZIGBEE_SDK=n)");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_zigbee_stop(void)
{
    return zigbee_stack_stop();
}

esp_err_t minihub_zigbee_permit_join(int seconds)
{
#if CONFIG_MINIHUB_ZIGBEE_SDK
    return zigbee_stack_permit_join(seconds);
#else
    (void)seconds;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool minihub_zigbee_permit_join_active(void)
{
    return zigbee_stack_permit_join_active();
}

static int parse_install_code_hex(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!hex || !out || !out_len) {
        return -1;
    }
    const char *p = hex;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    size_t n = 0;
    while (*p) {
        if (*p == ':' || *p == '-' || *p == ' ' || *p == '_') {
            p++;
            continue;
        }
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return -1;
        }
        if (n >= out_cap) {
            return -1;
        }
        char b[3] = {p[0], p[1], '\0'};
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        p += 2;
    }
    if (n == 0) {
        return -1;
    }
    *out_len = n;
    return 0;
}

esp_err_t minihub_zigbee_add_install_code(uint64_t ieee, const char *ic_hex)
{
#if CONFIG_MINIHUB_ZIGBEE_SDK
    uint8_t ic[18];
    size_t ic_len = 0;
    if (parse_install_code_hex(ic_hex, ic, sizeof(ic), &ic_len) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return zigbee_stack_add_install_code(ieee, ic, ic_len);
#else
    (void)ieee;
    (void)ic_hex;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t minihub_zigbee_onoff(uint16_t short_addr, uint8_t endpoint,
                               minihub_zigbee_onoff_cmd_t cmd)
{
    return zigbee_stack_onoff(short_addr, endpoint, cmd);
}

esp_err_t minihub_zigbee_set_callbacks(minihub_zigbee_device_cb_t on_device,
                                       minihub_zigbee_attr_cb_t on_attr,
                                       void *ctx)
{
    s_user_device_cb = on_device;
    s_user_attr_cb = on_attr;
    s_user_ctx = ctx;
    return ESP_OK;
}

esp_err_t minihub_zigbee_list_devices(minihub_zigbee_device_t *out, size_t cap, size_t *count)
{
    if (!out || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = s_device_count < cap ? s_device_count : cap;
    memcpy(out, s_devices, n * sizeof(minihub_zigbee_device_t));
    *count = n;
    return ESP_OK;
}

bool minihub_zigbee_running(void)
{
    return zigbee_stack_running();
}

esp_err_t minihub_zigbee_wait_ready(uint32_t timeout_ms)
{
#if !CONFIG_MINIHUB_ZIGBEE_SDK
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (zigbee_stack_running()) {
        return ESP_OK;
    }
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms == 0 ? 1 : timeout_ms);
    ESP_LOGI(TAG, "waiting for Zigbee coordinator ready (timeout %u ms)", (unsigned)timeout_ms);
    while (!zigbee_stack_running()) {
        if ((xTaskGetTickCount() - start) >= limit) {
            ESP_LOGW(TAG, "Zigbee ready wait timed out");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
#endif
}

esp_err_t minihub_zigbee_upsert_device(const minihub_zigbee_device_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)minihub_zigbee_init();
    return minihub_zb_nvs_upsert(dev, s_devices, &s_device_count, MINIHUB_ZB_MAX_DEVICES);
}

esp_err_t minihub_zigbee_remove_device(const char *point_id)
{
    if (!point_id || !point_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)minihub_zigbee_init();
    esp_err_t err = minihub_zb_nvs_remove(point_id, s_devices, &s_device_count);
    if (err != ESP_OK) {
        return err;
    }
    esp_err_t ureg = minihub_point_unregister(point_id);
    if (ureg != ESP_OK && ureg != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "point unregister %s: %s", point_id, esp_err_to_name(ureg));
    }
    ESP_LOGI(TAG, "removed device %s", point_id);
    return ESP_OK;
}

const minihub_zigbee_device_t *minihub_zigbee_find_by_point(const char *point_id)
{
    if (!point_id) {
        return NULL;
    }
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].point_id, point_id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

const minihub_zigbee_device_t *minihub_zigbee_find_by_addr(uint16_t short_addr, uint8_t endpoint)
{
    for (size_t i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr &&
            (endpoint == 0 || s_devices[i].endpoint == endpoint)) {
            return &s_devices[i];
        }
    }
    return NULL;
}
