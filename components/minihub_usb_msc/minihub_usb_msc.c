/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Host MSC → VFS FAT mount (based on IDF usb/host/msc example).
 */

#include "minihub_usb_msc.h"

#include "sdkconfig.h"

#if !CONFIG_MINIHUB_USB_MSC

esp_err_t minihub_usb_msc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool minihub_usb_msc_mounted(void)
{
    return false;
}

const char *minihub_usb_msc_mount_path(void)
{
    return NULL;
}

void minihub_usb_msc_io_lock(void) {}
void minihub_usb_msc_io_unlock(void) {}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"

static const char *TAG = "usb_msc";

#ifndef CONFIG_MINIHUB_USB_MSC_MOUNT_PATH
#define CONFIG_MINIHUB_USB_MSC_MOUNT_PATH "/usb0"
#endif

#define APP_QUEUE_LEN 8

typedef struct {
    enum {
        APP_DEVICE_CONNECTED,
        APP_DEVICE_DISCONNECTED,
    } id;
    union {
        uint8_t new_dev_address;
        msc_host_device_handle_t device_handle;
    } data;
} app_message_t;

static QueueHandle_t s_app_queue;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_io_lock;
static msc_host_device_handle_t s_msc_device;
static msc_host_vfs_handle_t s_vfs;
static bool s_mounted;
static char s_mount_path[16] = CONFIG_MINIHUB_USB_MSC_MOUNT_PATH;

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!s_app_queue) {
        return;
    }
    app_message_t message = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        message.id = APP_DEVICE_CONNECTED;
        message.data.new_dev_address = event->device.address;
        xQueueSend(s_app_queue, &message, 0);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        message.id = APP_DEVICE_DISCONNECTED;
        message.data.device_handle = event->device.handle;
        xQueueSend(s_app_queue, &message, 0);
    }
}

static void log_heap_snapshot(const char *when)
{
    ESP_LOGI(TAG,
             "%s heap free_int=%u largest_int=%u free_psram=%u largest_psram=%u free_dma=%u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static esp_err_t mount_device(uint8_t addr)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "already mounted; ignoring usb_addr=%u", (unsigned)addr);
        return ESP_ERR_INVALID_STATE;
    }

    msc_host_device_handle_t msc = NULL;
    esp_err_t err = msc_host_install_device(addr, &msc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device: %s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };

    char mount_path[16];
    snprintf(mount_path, sizeof(mount_path), "%s", s_mount_path);

    /* Stale /usb0 from a previous failed mount can consume the only FF_VOLUMES slot
     * and surface as ESP_ERR_NO_MEM from esp_vfs_fat_register_cfg. */
    (void)esp_vfs_fat_unregister_path(mount_path);

    msc_host_vfs_handle_t vfs = NULL;
    err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        log_heap_snapshot("before vfs_register");
        err = msc_host_vfs_register(msc, mount_path, &mount_config, &vfs);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "msc_host_vfs_register(%s) try %d: %s",
                 mount_path, attempt + 1, esp_err_to_name(err));
        (void)esp_vfs_fat_unregister_path(mount_path);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register(%s): %s", mount_path, esp_err_to_name(err));
        (void)msc_host_uninstall_device(msc);
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_msc_device = msc;
    s_vfs = vfs;
    s_mounted = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "MSC mounted at %s (usb_addr=%u)", s_mount_path, (unsigned)addr);

    char marker_dir[64];
    snprintf(marker_dir, sizeof(marker_dir), "%s/minihub", s_mount_path);
    if (mkdir(marker_dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %s", marker_dir, strerror(errno));
    } else {
        char marker[80];
        snprintf(marker, sizeof(marker), "%s/.usb_ok", marker_dir);
        FILE *f = fopen(marker, "w");
        if (f) {
            fprintf(f, "ok\n");
            fclose(f);
        }
    }
    return ESP_OK;
}

static void unmount_device(msc_host_device_handle_t handle)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_mounted || (handle && handle != s_msc_device)) {
        xSemaphoreGive(s_lock);
        return;
    }
    msc_host_vfs_handle_t vfs = s_vfs;
    msc_host_device_handle_t msc = s_msc_device;
    s_vfs = NULL;
    s_msc_device = NULL;
    s_mounted = false;
    xSemaphoreGive(s_lock);

    if (vfs) {
        (void)msc_host_vfs_unregister(vfs);
    }
    if (msc) {
        (void)msc_host_uninstall_device(msc);
    }
    ESP_LOGI(TAG, "MSC unmounted (%s)", s_mount_path);
}

static void msc_app_task(void *arg)
{
    (void)arg;
    app_message_t msg;
    while (true) {
        if (xQueueReceive(s_app_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.id == APP_DEVICE_CONNECTED) {
            (void)mount_device(msg.data.new_dev_address);
        } else if (msg.id == APP_DEVICE_DISCONNECTED) {
            unmount_device(msg.data.device_handle);
        }
    }
}

static void usb_host_task(void *arg)
{
    (void)arg;
    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install: %s", esp_err_to_name(err));
        (void)usb_host_uninstall();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB Host MSC ready; waiting for U-disk");
    while (true) {
        uint32_t event_flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

esp_err_t minihub_usb_msc_init(void)
{
    if (s_app_queue) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_io_lock = xSemaphoreCreateRecursiveMutex();
    s_app_queue = xQueueCreate(APP_QUEUE_LEN, sizeof(app_message_t));
    if (!s_lock || !s_io_lock || !s_app_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(msc_app_task, "usb_msc_app", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool minihub_usb_msc_mounted(void)
{
    bool m = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        m = s_mounted;
        xSemaphoreGive(s_lock);
    }
    return m;
}

const char *minihub_usb_msc_mount_path(void)
{
    return minihub_usb_msc_mounted() ? s_mount_path : NULL;
}

void minihub_usb_msc_io_lock(void)
{
    if (s_io_lock) {
        xSemaphoreTakeRecursive(s_io_lock, portMAX_DELAY);
    }
}

void minihub_usb_msc_io_unlock(void)
{
    if (s_io_lock) {
        xSemaphoreGiveRecursive(s_io_lock);
    }
}

#endif /* CONFIG_MINIHUB_USB_MSC */
