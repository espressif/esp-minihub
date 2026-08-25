/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start USB Host + MSC client; mounts first FAT stick at /usb0 when present. */
esp_err_t minihub_usb_msc_init(void);

bool minihub_usb_msc_mounted(void);

/** Mount path of the primary stick, or NULL if not mounted. */
const char *minihub_usb_msc_mount_path(void);

/**
 * Serialize all FatFs I/O on the MSC volume (record, list, download).
 * FatFs FS_LOCK only guards open file handles; diskio is still single-device.
 */
void minihub_usb_msc_io_lock(void);
void minihub_usb_msc_io_unlock(void);

#ifdef __cplusplus
}
#endif
