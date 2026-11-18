/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_vibe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_blockdev.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_VIBE_SDMMC
#include "driver/sdmmc_host.h"
#include "hal/gpio_types.h"
#include "sdmmc_cmd.h"
#endif

#if CONFIG_MINIHUB_VIBE_USB
#include "esp_intr_alloc.h"
#include "usb/msc_host.h"
#include "usb/usb_host.h"
#endif
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
#include "minihub_usb_msc.h"
#endif

static const char *TAG = "minihub_vibe_media";

static esp_blockdev_handle_t s_blockdev;
static minihub_vibe_device_t s_device = MINIHUB_VIBE_DEVICE_NONE;
static bool s_mounted;
/* Set by media_open when a probed medium was physically present but had no
 * LittleFS volume. Lets the OTA layer ask the operator to format it rather
 * than failing with a bare "no media". MINIHUB_VIBE_DEVICE_NONE otherwise. */
static minihub_vibe_device_t s_needs_format = MINIHUB_VIBE_DEVICE_NONE;

#if CONFIG_MINIHUB_VIBE_SDMMC
static sdmmc_card_t *s_card;
static bool s_sd_host_initialized;
#endif

/* esp_vfs_littlefs_register() calls ops->release() on the BDL handle on every
 * teardown: from esp_littlefs_free() when the mount fails (including a corrupt
 * or absent filesystem) and from esp_vfs_littlefs_unregister_blockdev() on a
 * clean unmount. Some of its early parameter checks fail before the handle is
 * recorded, though, so the caller cannot tell from the return code alone
 * whether it still owns the handle. Give LittleFS a borrowed ops table whose
 * release is idempotent, so the handle is freed exactly once no matter which
 * path runs and no matter whether we release it ourselves afterwards.
 *
 * The same table normalises the erased state: esp_littlefs refuses any BDL with
 * default_val_after_erase=0. The SDMMC BDL is a logical block device
 * (erase_before_write=0, and_type_write=0) and derives that flag from
 * SCR.erase_mem_state, which is 0 on MMC and on several SD cards. Making erase
 * actually produce 0xFF turns the reported capability into a true statement
 * rather than overriding a hardware fact. */
static const esp_blockdev_ops_t *s_bd_orig_ops;
static bool s_bd_normalise_erase;
static bool s_bd_released;

static esp_err_t bd_wrap_read(esp_blockdev_handle_t bd, uint8_t *dst, size_t dst_size,
                              uint64_t src, size_t len)
{
    return s_bd_orig_ops->read(bd, dst, dst_size, src, len);
}

static esp_err_t bd_wrap_write(esp_blockdev_handle_t bd, const uint8_t *src,
                               uint64_t dst, size_t len)
{
    return s_bd_orig_ops->write(bd, src, dst, len);
}

static esp_err_t bd_wrap_erase(esp_blockdev_handle_t bd, uint64_t start, size_t len)
{
    if (!s_bd_normalise_erase) {
        return s_bd_orig_ops->erase(bd, start, len);
    }
    size_t unit = bd->geometry.write_size ? bd->geometry.write_size : 512;
    uint8_t *ff = malloc(unit);
    if (!ff) {
        return ESP_ERR_NO_MEM;
    }
    memset(ff, 0xff, unit);
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < len; off += unit) {
        size_t n = (len - off < unit) ? (len - off) : unit;
        err = s_bd_orig_ops->write(bd, ff, start + off, n);
        if (err != ESP_OK) {
            break;
        }
    }
    free(ff);
    return err;
}

static esp_err_t bd_wrap_sync(esp_blockdev_handle_t bd)
{
    return s_bd_orig_ops->sync ? s_bd_orig_ops->sync(bd) : ESP_OK;
}

static esp_err_t bd_wrap_ioctl(esp_blockdev_handle_t bd, const uint8_t cmd, void *args)
{
    return s_bd_orig_ops->ioctl ? s_bd_orig_ops->ioctl(bd, cmd, args) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bd_wrap_release(esp_blockdev_handle_t bd)
{
    if (s_bd_released) {
        return ESP_OK;
    }
    s_bd_released = true;
    return s_bd_orig_ops->release ? s_bd_orig_ops->release(bd) : ESP_OK;
}

static const esp_blockdev_ops_t s_borrowed_ops = {
    .read = bd_wrap_read,
    .write = bd_wrap_write,
    .erase = bd_wrap_erase,
    .sync = bd_wrap_sync,
    .ioctl = bd_wrap_ioctl,
    .release = bd_wrap_release,
};

#if CONFIG_MINIHUB_VIBE_USB
static SemaphoreHandle_t s_usb_event;
static uint8_t s_usb_address;
static msc_host_device_handle_t s_msc_device;
static bool s_usb_host_installed;
static bool s_usb_driver_installed;
static TaskHandle_t s_usb_lib_task;
static TaskHandle_t s_usb_lib_waiter;
static volatile bool s_usb_lib_stop;
#endif

#if CONFIG_MINIHUB_VIBE_SDMMC
static esp_err_t sd_acquire(esp_blockdev_handle_t *out)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = (CONFIG_MINIHUB_VIBE_SD_BUS_WIDTH == 4) ? 4 : 1;
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
    if (CONFIG_MINIHUB_VIBE_SD_PIN_CLK >= 0) {
        slot.clk = CONFIG_MINIHUB_VIBE_SD_PIN_CLK;
    }
    if (CONFIG_MINIHUB_VIBE_SD_PIN_CMD >= 0) {
        slot.cmd = CONFIG_MINIHUB_VIBE_SD_PIN_CMD;
    }
    if (CONFIG_MINIHUB_VIBE_SD_PIN_D0 >= 0) {
        slot.d0 = CONFIG_MINIHUB_VIBE_SD_PIN_D0;
    }
#if CONFIG_MINIHUB_VIBE_SD_BUS_WIDTH == 4
    if (CONFIG_MINIHUB_VIBE_SD_PIN_D1 >= 0) {
        slot.d1 = CONFIG_MINIHUB_VIBE_SD_PIN_D1;
    }
    if (CONFIG_MINIHUB_VIBE_SD_PIN_D2 >= 0) {
        slot.d2 = CONFIG_MINIHUB_VIBE_SD_PIN_D2;
    }
    if (CONFIG_MINIHUB_VIBE_SD_PIN_D3 >= 0) {
        slot.d3 = CONFIG_MINIHUB_VIBE_SD_PIN_D3;
    }
#endif
#endif

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        return err;
    }
    s_sd_host_initialized = true;
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        return ESP_ERR_NO_MEM;
    }
    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        free(card);
        return err;
    }
    err = sdmmc_get_blockdev(card, out);
    if (err != ESP_OK) {
        free(card);
        return err;
    }
    s_card = card;
    return ESP_OK;
}
#endif

#if CONFIG_MINIHUB_VIBE_USB
static void usb_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED && s_usb_event) {
        s_usb_address = event->device.address;
        xSemaphoreGive(s_usb_event);
    }
}

/* msc_host_install() starts a client-event task, but USB Host Library events
 * (connect/disconnect) are only dispatched while someone calls
 * usb_host_lib_handle_events(). Without this task the CONNECTED callback never
 * fires, so the probe always times out even with a stick already plugged in. */
static void usb_lib_task(void *arg)
{
    (void)arg;
    while (!s_usb_lib_stop) {
        uint32_t flags = 0;
        (void)usb_host_lib_handle_events(pdMS_TO_TICKS(50), &flags);
    }
    if (s_usb_lib_waiter) {
        xTaskNotifyGive(s_usb_lib_waiter);
    }
    vTaskDelete(NULL);
}

static void usb_release_all(void)
{
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
    /* Host already owns the USB stack. Do not remount FAT: the stick may now
     * be LittleFS (OTA format). RTSP recording stays unavailable until a FAT
     * stick is inserted again. */
    minihub_usb_msc_release_blockdev(false);
    return;
#endif
    if (s_msc_device) {
        (void)msc_host_uninstall_device(s_msc_device);
        s_msc_device = NULL;
    }
    if (s_usb_driver_installed) {
        (void)msc_host_uninstall();
        s_usb_driver_installed = false;
    }
    if (s_usb_lib_task) {
        s_usb_lib_waiter = xTaskGetCurrentTaskHandle();
        s_usb_lib_stop = true;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        s_usb_lib_waiter = NULL;
        s_usb_lib_task = NULL;
    }
    /* Intentionally do NOT call usb_host_uninstall() and do NOT clear
     * s_usb_host_installed. usb_host_uninstall() -> hub_uninstall() aborts the
     * chip (ESP_ERROR_CHECK on a root port that is not NOT_POWERED) whenever a
     * USB device is physically attached, which previously wedged Vibe in a boot
     * loop. Vibe reboots the moment this returns, so the reset tears the stack
     * down; in Host we keep the library installed and let the next probe reuse
     * it (usb_acquire skips usb_host_install when s_usb_host_installed is set).
     * This makes USB acquisition idempotent and crash-free across both images,
     * and fixes ESP_ERR_INVALID_STATE when USB is probed a second time in one
     * boot (e.g. after a boot-time media scan left the library installed). */
}

static esp_err_t usb_acquire(esp_blockdev_handle_t *out)
{
#ifndef MSC_HOST_BDL_API_SUPPORTED
#error "Vibe Mode needs usb_host_msc with msc_host_get_blockdev() (IDF >= 6.0.4)"
#else
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
    /* Board 10 (and any Host with CONFIG_MINIHUB_USB_MSC) already installed
     * usb_host + msc_host for FAT at /usb0. A second usb_host_install here
     * returns ESP_ERR_INVALID_STATE and OTA never reaches LittleFS / needs_format.
     * Borrow the stick through that owner instead. */
    return minihub_usb_msc_acquire_blockdev(out);
#else
    if (!s_usb_event) {
        s_usb_event = xSemaphoreCreateBinary();
        if (!s_usb_event) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* The USB Host Library is installed at most once and deliberately never
     * uninstalled (see usb_release_all): usb_host_uninstall() -> hub_uninstall()
     * aborts the chip (ESP_ERROR_CHECK on a root port that is not NOT_POWERED)
     * whenever a USB device is physically attached, which previously wedged
     * Vibe in a boot loop. So install only when not already up and reuse it on
     * later probes. msc_host_install() calls usb_host_client_register(), which
     * returns INVALID_STATE unless the library is installed first. */
    if (!s_usb_host_installed) {
        const usb_host_config_t host_config = {
            .intr_flags = ESP_INTR_FLAG_LOWMED,
        };
        esp_err_t err = usb_host_install(&host_config);
        if (err != ESP_OK) {
            return err;
        }
        s_usb_host_installed = true;
    }

    if (!s_usb_lib_task) {
        s_usb_lib_stop = false;
        if (xTaskCreate(usb_lib_task, "vibe_usb_lib", 4096, NULL, 6, &s_usb_lib_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_usb_driver_installed) {
        const msc_host_driver_config_t config = {
            .create_backround_task = true,
            .task_priority = 5,
            .stack_size = 4096,
            .core_id = tskNO_AFFINITY,
            .callback = usb_event_cb,
            .callback_arg = NULL,
        };
        esp_err_t err = msc_host_install(&config);
        if (err != ESP_OK) {
            return err;
        }
        s_usb_driver_installed = true;
    }

    if (xSemaphoreTake(s_usb_event, pdMS_TO_TICKS(CONFIG_MINIHUB_VIBE_USB_WAIT_MS)) != pdTRUE) {
        ESP_LOGI(TAG, "no USB mass-storage device within %d ms", CONFIG_MINIHUB_VIBE_USB_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = msc_host_install_device(s_usb_address, &s_msc_device);
    if (err != ESP_OK) {
        return err;
    }
    /* Independent handle: the one msc_host_install_device() owns is reserved
     * for the FAT VFS helper and is released by msc_host_uninstall_device(). */
    return msc_host_get_blockdev(s_msc_device, out);
#endif /* CONFIG_MINIHUB_USB_MSC */
#endif /* MSC_HOST_BDL_API_SUPPORTED */
}
#endif

static void release_blockdev(esp_blockdev_handle_t bd)
{
    if (bd && bd->ops && bd->ops->release) {
        (void)bd->ops->release(bd);
    }
}

static esp_err_t acquire(minihub_vibe_device_t device, esp_blockdev_handle_t *out)
{
    switch (device) {
#if CONFIG_MINIHUB_VIBE_SDMMC
    case MINIHUB_VIBE_DEVICE_SDMMC_ID:
        return sd_acquire(out);
#endif
#if CONFIG_MINIHUB_VIBE_USB
    case MINIHUB_VIBE_DEVICE_USB_ID:
        return usb_acquire(out);
#endif
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/* esp_littlefs logs "Corrupted dir pair" / "mount failed" / "Failed to
 * initialize LittleFS" at ERROR level when handed a medium with no LittleFS
 * volume — a FAT SD card, a blank card or an ordinary USB stick. For Vibe that
 * is the expected "no firmware media" case, not an error, so check the
 * superblock magic before asking the library to mount. */
static bool has_littlefs_superblock(esp_blockdev_handle_t bd)
{
    size_t unit = bd->geometry.read_size ? bd->geometry.read_size : 512;
    if (unit < 64) {
        unit = 64;
    }
    /* The superblock lives in metadata block pair {0, 1}; block size varies
     * with the BDL geometry, so scan the first 8 KiB in read_size units. */
    size_t units = 8192 / unit;
    if (units < 2) {
        units = 2;
    }
    uint8_t *buf = malloc(unit);
    if (!buf) {
        /* Let the real mount report an allocation problem instead of guessing. */
        return true;
    }
    bool found = false;
    for (size_t i = 0; i < units && !found; ++i) {
        if (bd->ops->read(bd, buf, unit, (uint64_t)i * unit, unit) != ESP_OK) {
            break;
        }
        for (size_t off = 0; off + 8 <= unit; ++off) {
            if (memcmp(buf + off, "littlefs", 8) == 0) {
                found = true;
                break;
            }
        }
    }
    free(buf);
    return found;
}

static esp_err_t mount(esp_blockdev_handle_t bd)
{
    s_bd_orig_ops = bd->ops;
    s_bd_normalise_erase = !bd->device_flags.default_val_after_erase;
    s_bd_released = false;
    if (s_bd_normalise_erase) {
        ESP_LOGI(TAG, "blockdev reports erase state 0; normalising erase to 0xFF for LittleFS");
    }
    bd->ops = &s_borrowed_ops;
    bd->device_flags.default_val_after_erase = 1;

    if (!has_littlefs_superblock(bd)) {
        ESP_LOGI(TAG, "no LittleFS superblock on this medium; skipping mount");
        return ESP_ERR_NOT_FOUND;
    }

    esp_vfs_littlefs_conf_t conf = {0};
    conf.base_path = MINIHUB_VIBE_MOUNT_PATH;
    conf.blockdev = bd;
    /* A bool Kconfig set to n is not emitted into sdkconfig.h at all, so this
     * must be #ifdef rather than a value expression. conf is zero-initialised. */
#ifdef CONFIG_MINIHUB_VIBE_FORMAT_ON_FAIL
    conf.format_if_mount_failed = 1;
#endif
    conf.read_only = 0;
    conf.dont_mount = 0;
    conf.grow_on_mount = 0;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        return err;
    }
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%s", MINIHUB_VIBE_MOUNT_PATH, MINIHUB_VIBE_FIRMWARE_DIR);
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %s", dir, strerror(errno));
    }
    s_blockdev = bd;
    s_mounted = true;
    return ESP_OK;
}

esp_err_t minihub_vibe_media_open(minihub_vibe_device_t *out_device)
{
    if (s_mounted) {
        if (out_device) {
            *out_device = s_device;
        }
        s_needs_format = MINIHUB_VIBE_DEVICE_NONE;
        return ESP_OK;
    }
    s_needs_format = MINIHUB_VIBE_DEVICE_NONE;

    /* Operator-chosen media: start from the build-allowed set, then intersect
     * with the NVS mask. A zero mask means "no external medium selected". */
    uint8_t mask = minihub_vibe_mask_effective();
    uint8_t stored = MINIHUB_VIBE_DEV_MASK_ALL;
    if (minihub_vibe_mask_from_nvs(&stored) == ESP_OK) {
        mask &= stored;
    }
    if (mask == 0) {
        ESP_LOGI(TAG, "no external medium selected (mask=0)");
        return ESP_ERR_NOT_FOUND;
    }

    /* Fixed failover order: SD card first, then USB drive. */
    static const minihub_vibe_device_t order[2] = {
        MINIHUB_VIBE_DEVICE_SDMMC_ID, MINIHUB_VIBE_DEVICE_USB_ID
    };
    minihub_vibe_device_t first_unformatted = MINIHUB_VIBE_DEVICE_NONE;
    for (int i = 0; i < 2; ++i) {
        minihub_vibe_device_t dev = order[i];
        uint8_t bit = (dev == MINIHUB_VIBE_DEVICE_SDMMC_ID)
                          ? MINIHUB_VIBE_DEV_SDMMC_BIT
                          : MINIHUB_VIBE_DEV_USB_BIT;
        if (!(mask & bit)) {
            continue;
        }
        esp_blockdev_handle_t bd = NULL;
        esp_err_t err = acquire(dev, &bd);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "device %d unavailable: %s", dev, esp_err_to_name(err));
            continue;
        }
        err = mount(bd);
        if (err == ESP_OK) {
            s_device = dev;
            s_needs_format = MINIHUB_VIBE_DEVICE_NONE;
            ESP_LOGI(TAG, "LittleFS mounted at %s (device=%d)", MINIHUB_VIBE_MOUNT_PATH, s_device);
            if (out_device) {
                *out_device = s_device;
            }
            return ESP_OK;
        }
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "no LittleFS volume on device %d", dev);
            if (first_unformatted == MINIHUB_VIBE_DEVICE_NONE) {
                first_unformatted = dev;
            }
        } else {
            ESP_LOGW(TAG, "LittleFS mount on device %d failed: %s", dev, esp_err_to_name(err));
        }
        /* LittleFS may already have released the handle on this failure path.
         * s_borrowed_ops.release is idempotent, so this is safe either way. */
        release_blockdev(bd);
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
        if (dev == MINIHUB_VIBE_DEVICE_USB_ID) {
            /* Probe left the stick unchanged — remount FAT for RTSP until the
             * operator confirms LittleFS format. */
            minihub_usb_msc_release_blockdev(true);
        }
#endif
    }
    s_needs_format = first_unformatted;
    return ESP_ERR_NOT_FOUND;
}

minihub_vibe_device_t minihub_vibe_media_needs_format(void)
{
    return s_needs_format;
}

esp_err_t minihub_vibe_media_format(minihub_vibe_device_t device)
{
    if (device != MINIHUB_VIBE_DEVICE_SDMMC_ID && device != MINIHUB_VIBE_DEVICE_USB_ID) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Drop any currently mounted medium before formatting a (possibly different)
     * one: close() also tears down the SD host / USB client so acquire() below
     * starts from a clean state. */
    (void)minihub_vibe_media_close();

    esp_blockdev_handle_t bd = NULL;
    esp_err_t err = acquire(device, &bd);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "format: device %d unavailable: %s", device, esp_err_to_name(err));
        return err;
    }
    s_bd_orig_ops = bd->ops;
    s_bd_normalise_erase = !bd->device_flags.default_val_after_erase;
    s_bd_released = false;
    if (s_bd_normalise_erase) {
        ESP_LOGI(TAG, "blockdev reports erase state 0; normalising erase to 0xFF for LittleFS");
    }
    bd->ops = &s_borrowed_ops;
    bd->device_flags.default_val_after_erase = 1;

    esp_vfs_littlefs_conf_t conf = {0};
    conf.base_path = MINIHUB_VIBE_MOUNT_PATH;
    conf.blockdev = bd;
    /* Force a fresh LittleFS volume. This is the interactive "format" the web
     * console asks the operator to confirm; it wipes everything on the medium. */
    conf.format_if_mount_failed = 1;
    conf.read_only = 0;
    conf.dont_mount = 0;
    conf.grow_on_mount = 0;

    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format LittleFS on device %d failed: %s", device, esp_err_to_name(err));
        release_blockdev(bd);
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
        if (device == MINIHUB_VIBE_DEVICE_USB_ID) {
            minihub_usb_msc_release_blockdev(true);
        }
#endif
        return err;
    }
    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%s", MINIHUB_VIBE_MOUNT_PATH, MINIHUB_VIBE_FIRMWARE_DIR);
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s: %s", dir, strerror(errno));
    }
    s_blockdev = bd;
    s_device = device;
    s_mounted = true;
    s_needs_format = MINIHUB_VIBE_DEVICE_NONE;
    ESP_LOGI(TAG, "formatted and mounted LittleFS on device %d at %s", device, MINIHUB_VIBE_MOUNT_PATH);
    return ESP_OK;
}

minihub_vibe_device_t minihub_vibe_get_device(void)
{
    if (s_mounted) {
        return s_device;
    }
    uint8_t mask = minihub_vibe_mask_effective();
    uint8_t stored = MINIHUB_VIBE_DEV_MASK_ALL;
    if (minihub_vibe_mask_from_nvs(&stored) == ESP_OK) {
        mask &= stored;
    }
    if (mask & MINIHUB_VIBE_DEV_SDMMC_BIT) {
        return MINIHUB_VIBE_DEVICE_SDMMC_ID;
    }
    if (mask & MINIHUB_VIBE_DEV_USB_BIT) {
        return MINIHUB_VIBE_DEVICE_USB_ID;
    }
    return MINIHUB_VIBE_DEVICE_NONE;
}

const char *minihub_vibe_device_str(minihub_vibe_device_t device)
{
    switch (device) {
    case MINIHUB_VIBE_DEVICE_SDMMC_ID:
        return MINIHUB_VIBE_DEVICE_SDMMC;
    case MINIHUB_VIBE_DEVICE_USB_ID:
        return MINIHUB_VIBE_DEVICE_USB;
    default:
        return "none";
    }
}

esp_err_t minihub_vibe_media_close(void)
{
    if (s_mounted) {
        /* LittleFS invokes the handle's release op when it tears down; this
         * covers both sdmmc_get_blockdev() and msc_host_get_blockdev(). */
        (void)esp_vfs_littlefs_unregister_blockdev(s_blockdev);
        s_mounted = false;
        s_blockdev = NULL;
    }

#if CONFIG_MINIHUB_VIBE_SDMMC
    if (s_sd_host_initialized) {
        sdmmc_host_deinit();
        s_sd_host_initialized = false;
    }
    free(s_card);
    s_card = NULL;
#endif
#if CONFIG_MINIHUB_VIBE_USB
    usb_release_all();
#endif
    s_device = MINIHUB_VIBE_DEVICE_NONE;
    return ESP_OK;
}
