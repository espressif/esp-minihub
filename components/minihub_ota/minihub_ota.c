/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_ota.h"
#include "minihub_net.h"
#include "minihub_task.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

static const char *TAG = "minihub_ota";

static SemaphoreHandle_t s_lock;
static minihub_ota_state_t s_state = MINIHUB_OTA_IDLE;
static minihub_ota_source_t s_source = MINIHUB_OTA_SRC_NONE;
static char s_error[96];
static int s_progress = -1;
static size_t s_bytes;
static size_t s_expected;

static esp_ota_handle_t s_ota_handle;
static const esp_partition_t *s_update_part;
static bool s_ota_begun;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void set_error(const char *msg)
{
    if (msg) {
        strncpy(s_error, msg, sizeof(s_error) - 1);
        s_error[sizeof(s_error) - 1] = '\0';
    } else {
        s_error[0] = '\0';
    }
}

static void update_progress_locked(void)
{
    if (s_expected > 0) {
        size_t p = (s_bytes * 100) / s_expected;
        s_progress = (p > 100) ? 100 : (int)p;
    } else {
        s_progress = -1;
    }
}

const char *minihub_ota_source_str(minihub_ota_source_t src)
{
    switch (src) {
    case MINIHUB_OTA_SRC_URL:
        return "url";
    case MINIHUB_OTA_SRC_UPLOAD:
        return "upload";
    case MINIHUB_OTA_SRC_RAINMAKER:
        return "rainmaker";
    default:
        return "none";
    }
}

esp_err_t minihub_ota_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_state = MINIHUB_OTA_IDLE;
    s_source = MINIHUB_OTA_SRC_NONE;
    s_progress = -1;
    s_bytes = 0;
    s_expected = 0;
    s_ota_begun = false;
    set_error(NULL);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t img_state;
    if (running && esp_ota_get_state_partition(running, &img_state) == ESP_OK) {
        if (img_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mark_app_valid: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "marked running app valid (cancel rollback)");
            }
        }
    }
    return ESP_OK;
}

minihub_ota_state_t minihub_ota_get_state(void)
{
    return s_state;
}

const char *minihub_ota_get_error(void)
{
    return s_error;
}

minihub_ota_source_t minihub_ota_get_source(void)
{
    return s_source;
}

int minihub_ota_get_progress(void)
{
    return s_progress;
}

size_t minihub_ota_get_bytes_written(void)
{
    return s_bytes;
}

bool minihub_ota_is_busy(void)
{
    return s_state == MINIHUB_OTA_IN_PROGRESS;
}

static esp_err_t writer_begin_locked(minihub_ota_source_t src, size_t expected)
{
    /* Allow continue when start_url already reserved the slot (not yet ota_begin). */
    if (s_state == MINIHUB_OTA_IN_PROGRESS) {
        if (!(s_source == src && !s_ota_begun)) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    s_update_part = esp_ota_get_next_update_partition(NULL);
    if (!s_update_part) {
        set_error("no OTA partition");
        s_state = MINIHUB_OTA_FAILED;
        s_source = src;
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_ota_begin(s_update_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    if (err != ESP_OK) {
        set_error("ota_begin failed");
        s_state = MINIHUB_OTA_FAILED;
        s_source = src;
        return err;
    }
    s_ota_begun = true;
    s_source = src;
    s_state = MINIHUB_OTA_IN_PROGRESS;
    s_bytes = 0;
    s_expected = expected;
    s_progress = expected > 0 ? 0 : -1;
    set_error(NULL);
    ESP_LOGI(TAG, "OTA begin src=%s part=%s size=%u",
             minihub_ota_source_str(src), s_update_part->label, (unsigned)s_update_part->size);
    return ESP_OK;
}

static esp_err_t writer_write_locked(const void *data, size_t len)
{
    if (!s_ota_begun || s_state != MINIHUB_OTA_IN_PROGRESS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        set_error("ota_write failed");
        return err;
    }
    s_bytes += len;
    update_progress_locked();
    return ESP_OK;
}

static void writer_abort_locked(const char *err)
{
    if (s_ota_begun) {
        (void)esp_ota_abort(s_ota_handle);
        s_ota_begun = false;
    }
    s_state = MINIHUB_OTA_FAILED;
    set_error(err ? err : "aborted");
}

static esp_err_t writer_finish_locked(void)
{
    if (!s_ota_begun) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_end(s_ota_handle);
    s_ota_begun = false;
    if (err != ESP_OK) {
        set_error("ota_end failed");
        s_state = MINIHUB_OTA_FAILED;
        return err;
    }
    err = esp_ota_set_boot_partition(s_update_part);
    if (err != ESP_OK) {
        set_error("set_boot_partition failed");
        s_state = MINIHUB_OTA_FAILED;
        return err;
    }
    s_progress = 100;
    s_state = MINIHUB_OTA_SUCCESS;
    set_error(NULL);
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OTA success, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static void schedule_reboot(void)
{
    if (minihub_task_create_psram(reboot_task, "ota_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "reboot task failed, restarting inline");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}

esp_err_t minihub_ota_upload_begin(size_t expected_size)
{
    lock();
    esp_err_t err = writer_begin_locked(MINIHUB_OTA_SRC_UPLOAD, expected_size);
    unlock();
    return err;
}

esp_err_t minihub_ota_upload_write(const void *data, size_t len)
{
    lock();
    esp_err_t err = writer_write_locked(data, len);
    if (err != ESP_OK && s_ota_begun) {
        writer_abort_locked("ota_write failed");
    }
    unlock();
    return err;
}

esp_err_t minihub_ota_upload_finish(void)
{
    lock();
    esp_err_t err = writer_finish_locked();
    unlock();
    if (err == ESP_OK) {
        schedule_reboot();
    }
    return err;
}

void minihub_ota_upload_abort(const char *err)
{
    lock();
    if (s_state == MINIHUB_OTA_IN_PROGRESS && s_source == MINIHUB_OTA_SRC_UPLOAD) {
        writer_abort_locked(err);
    } else if (err) {
        set_error(err);
        s_state = MINIHUB_OTA_FAILED;
    }
    unlock();
}

bool minihub_ota_rainmaker_try_begin(size_t expected_size)
{
    lock();
    if (s_state == MINIHUB_OTA_IN_PROGRESS) {
        unlock();
        return false;
    }
    s_source = MINIHUB_OTA_SRC_RAINMAKER;
    s_state = MINIHUB_OTA_IN_PROGRESS;
    s_bytes = 0;
    s_expected = expected_size;
    s_progress = expected_size > 0 ? 0 : -1;
    set_error(NULL);
    unlock();
    return true;
}

void minihub_ota_rainmaker_set_progress(int progress_0_100, size_t bytes_written)
{
    lock();
    if (s_source == MINIHUB_OTA_SRC_RAINMAKER && s_state == MINIHUB_OTA_IN_PROGRESS) {
        s_bytes = bytes_written;
        if (progress_0_100 >= 0 && progress_0_100 <= 100) {
            s_progress = progress_0_100;
        } else {
            update_progress_locked();
        }
    }
    unlock();
}

void minihub_ota_rainmaker_finish(bool success, const char *err)
{
    lock();
    if (success) {
        s_progress = 100;
        s_state = MINIHUB_OTA_SUCCESS;
        set_error(NULL);
    } else {
        s_state = MINIHUB_OTA_FAILED;
        set_error(err ? err : "rainmaker OTA failed");
    }
    unlock();
}

static void url_ota_task(void *arg)
{
    char *url = (char *)arg;
    esp_err_t err;

    lock();
    err = writer_begin_locked(MINIHUB_OTA_SRC_URL, 0);
    unlock();
    if (err != ESP_OK) {
        free(url);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        lock();
        writer_abort_locked("http client init failed");
        unlock();
        free(url);
        vTaskDelete(NULL);
        return;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        lock();
        writer_abort_locked("http open failed");
        unlock();
        free(url);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        lock();
        writer_abort_locked("bad HTTP status");
        unlock();
        free(url);
        vTaskDelete(NULL);
        return;
    }
    if (content_length > 0) {
        lock();
        s_expected = (size_t)content_length;
        update_progress_locked();
        unlock();
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        lock();
        writer_abort_locked("no mem");
        unlock();
        free(url);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int n = esp_http_client_read(client, buf, 4096);
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        lock();
        err = writer_write_locked(buf, (size_t)n);
        unlock();
        if (err != ESP_OK) {
            break;
        }
    }
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);

    if (err != ESP_OK) {
        lock();
        writer_abort_locked(s_error[0] ? s_error : "download failed");
        unlock();
        vTaskDelete(NULL);
        return;
    }

    lock();
    err = writer_finish_locked();
    unlock();
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    schedule_reboot();
    vTaskDelete(NULL);
}

esp_err_t minihub_ota_start_url(const char *url)
{
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (s_state == MINIHUB_OTA_IN_PROGRESS) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* Reserve before task create to close the race with upload / RainMaker. */
    s_state = MINIHUB_OTA_IN_PROGRESS;
    s_source = MINIHUB_OTA_SRC_URL;
    s_ota_begun = false;
    s_bytes = 0;
    s_expected = 0;
    s_progress = -1;
    set_error(NULL);
    unlock();

    size_t cap = strlen(url) + 32;
    char *url_copy = malloc(cap);
    if (url_copy && minihub_net_copy_url_with_mac(url, url_copy, cap) != ESP_OK) {
        free(url_copy);
        url_copy = NULL;
    }
    if (!url_copy) {
        url_copy = strdup(url);
    }
    if (!url_copy) {
        lock();
        s_state = MINIHUB_OTA_FAILED;
        set_error("no mem");
        unlock();
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = minihub_task_create_psram(url_ota_task, "minihub_ota", 8192, url_copy, 5, NULL);
    if (ok != pdPASS) {
        free(url_copy);
        lock();
        s_state = MINIHUB_OTA_FAILED;
        set_error("task create failed");
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
