/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_blockdev.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start USB Host + MSC client; mounts first FAT stick at /usb0 when present.
 */
esp_err_t minihub_usb_msc_init(void);

bool minihub_usb_msc_mounted(void);

/**
 * @brief Mount path of the primary stick, or NULL if not mounted.
 */
const char *minihub_usb_msc_mount_path(void);

/**
 * @brief Serialize all FatFs I/O on the MSC volume (record, list, download).
 * FatFs FS_LOCK only guards open file handles; diskio is still single-device.
 */
void minihub_usb_msc_io_lock(void);
void minihub_usb_msc_io_unlock(void);

/**
 * @brief Borrow the USB stick as a raw block device for Vibe/OTA LittleFS.
 *
 * Unmounts the FAT VFS if it was mounted (exclusive block access). The MSC
 * device and USB Host Library stay owned by this component — Vibe must not
 * call usb_host_install / msc_host_install itself while USB MSC is enabled.
 *
 * On success the caller owns *out and must release it via the blockdev ops
 * (or LittleFS teardown) and then call minihub_usb_msc_release_blockdev().
 */
esp_err_t minihub_usb_msc_acquire_blockdev(esp_blockdev_handle_t *out);

/**
 * @brief End a borrow from minihub_usb_msc_acquire_blockdev().
 *
 * Safe to call when nothing was lent. When @p remount_fat is true, attempts to
 * remount FAT (probe-failed path that left the stick unchanged). Pass false
 * after LittleFS format/mount — the volume is no longer FAT.
 */
void minihub_usb_msc_release_blockdev(bool remount_fat);

#ifdef __cplusplus
}
#endif
