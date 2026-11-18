/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace is shared with the Web OTA URL setting. The expected staged-image
 * SHA is stored in NVS and a media sidecar so Vibe Mode can reject an unexpected
 * firmware.bin before writing ota_0. An unchanged image is detected by comparing
 * esp_app_desc.app_elf_sha256 of the staged file with ota_0. */
#define MINIHUB_VIBE_NVS_NAMESPACE    "minihub"
#define MINIHUB_VIBE_NVS_KEY_DEVICE  "vibe_dev"
#define MINIHUB_VIBE_NVS_KEY_DEVICE_MASK "vibe_dev_mask"
#define MINIHUB_VIBE_NVS_KEY_PENDING "vibe_pending"
#define MINIHUB_VIBE_NVS_KEY_SHA     "vibe_sha"
#define MINIHUB_VIBE_NVS_KEY_RESULT  "vibe_result"

#define MINIHUB_VIBE_DEVICE_SDMMC "sdmmc"
#define MINIHUB_VIBE_DEVICE_USB   "usb"

/* Bitmask of operator-selected staging media. SD and USB may be selected
 * independently; when both are set the upgrade fails over (SD first, then USB).
 * A zero mask means "no external medium selected" and media_open() fails. */
#define MINIHUB_VIBE_DEV_SDMMC_BIT (1u << 0)
#define MINIHUB_VIBE_DEV_USB_BIT   (1u << 1)
#define MINIHUB_VIBE_DEV_MASK_ALL  (MINIHUB_VIBE_DEV_SDMMC_BIT | MINIHUB_VIBE_DEV_USB_BIT)

#define MINIHUB_VIBE_MOUNT_PATH      "/vibe"
#define MINIHUB_VIBE_FIRMWARE_DIR    "Firmware"
#define MINIHUB_VIBE_FIRMWARE_FILE   "firmware.bin"
#define MINIHUB_VIBE_FIRMWARE_PATH   MINIHUB_VIBE_MOUNT_PATH "/" MINIHUB_VIBE_FIRMWARE_DIR "/" MINIHUB_VIBE_FIRMWARE_FILE
#define MINIHUB_VIBE_FIRMWARE_SHA_PATH MINIHUB_VIBE_FIRMWARE_PATH ".sha256"

#define MINIHUB_VIBE_APP_LABEL     "ota_0"
#define MINIHUB_VIBE_FACTORY_LABEL "vibe_mode"

#define MINIHUB_VIBE_SHA256_LEN 32

typedef enum {
    MINIHUB_VIBE_DEVICE_NONE = 0,
    MINIHUB_VIBE_DEVICE_SDMMC_ID = 1,
    MINIHUB_VIBE_DEVICE_USB_ID = 2,
} minihub_vibe_device_t;

/* ---- external media (LittleFS on SDMMC or USB MSC) ---- */

/** Mount the configured device, or probe SDMMC then USB when NVS has no value.
 *  The device that mounts successfully is written back to NVS. */
esp_err_t minihub_vibe_media_open(minihub_vibe_device_t *out_device);
esp_err_t minihub_vibe_media_close(void);
/** After a media probe, the device (SDMMC or USB) that was physically present
 *  but had no LittleFS volume and therefore needs formatting, or
 *  MINIHUB_VIBE_DEVICE_NONE if every probed medium either mounted or was absent.
 *  Lets the OTA layer surface a "format this medium?" prompt instead of a bare
 *  failure. */
minihub_vibe_device_t minihub_vibe_media_needs_format(void);
/** Format the given medium (SDMMC or USB) to LittleFS and leave it mounted at
 *  MINIHUB_VIBE_MOUNT_PATH. Wipes all data on the medium. Returns
 *  ESP_ERR_INVALID_ARG for an unsupported device or the acquire error if the
 *  medium is absent. */
esp_err_t minihub_vibe_media_format(minihub_vibe_device_t device);
esp_err_t minihub_vibe_device_from_nvs(minihub_vibe_device_t *out_device);
esp_err_t minihub_vibe_device_to_nvs(minihub_vibe_device_t device);

/** Operator-selected staging-media bitmask (MINIHUB_VIBE_DEV_*_BIT). Reads the
 *  NVS mask; if unset, migrates a legacy single-device choice, and if neither
 *  exists defaults to MINIHUB_VIBE_DEV_MASK_ALL (SD + USB with failover). */
esp_err_t minihub_vibe_mask_from_nvs(uint8_t *out_mask);
/** Persist the staging-media bitmask (MINIHUB_VIBE_DEV_*_BIT). */
esp_err_t minihub_vibe_mask_to_nvs(uint8_t mask);
/** Effective mask for the current build (always has at least the bits the
 *  enabled MINIHUB_VIBE_SDMMC / MINIHUB_VIBE_USB Kconfigs allow). */
uint8_t minihub_vibe_mask_effective(void);

/** Effective device the next (or last) firmware stage writes to: the mounted
 *  device if any, else the NVS-preferred one, else the build default
 *  (SDMMC first, then USB). Lets the web console show where an upgrade lands. */
minihub_vibe_device_t minihub_vibe_get_device(void);
/** "sdmmc" | "usb" | "none" for the device enum, matching MINIHUB_VIBE_DEVICE_*. */
const char *minihub_vibe_device_str(minihub_vibe_device_t device);

/* ---- NVS hand-off state ---- */
esp_err_t minihub_vibe_set_pending(bool pending);
/** Read and clear the pending flag (one-shot consumption). */
bool minihub_vibe_take_pending(void);
esp_err_t minihub_vibe_set_result(const char *message);
esp_err_t minihub_vibe_get_result(char *buf, size_t buf_len);
/** Persist the expected staged-image SHA (NVS blob). */
esp_err_t minihub_vibe_set_expected_sha(const uint8_t sha256[MINIHUB_VIBE_SHA256_LEN]);
/** Clear the expected staged-image SHA from NVS. */
esp_err_t minihub_vibe_clear_expected_sha(void);
/** Load expected SHA from NVS, else from the media sidecar (pending path). */
esp_err_t minihub_vibe_load_expected_sha(uint8_t sha256[MINIHUB_VIBE_SHA256_LEN]);
/** Load expected SHA from the media sidecar only (force-GPIO recovery). */
esp_err_t minihub_vibe_load_expected_sha_sidecar(uint8_t sha256[MINIHUB_VIBE_SHA256_LEN]);
/** Write /Firmware/firmware.bin.sha256 (64 lowercase hex chars + newline). */
esp_err_t minihub_vibe_write_sha_sidecar(const uint8_t sha256[MINIHUB_VIBE_SHA256_LEN]);
/** Increment and return the consecutive Vibe Mode copy-failure count. */
uint8_t minihub_vibe_bump_failure(void);
void minihub_vibe_clear_failures(void);

/* ---- firmware helpers ---- */
esp_err_t minihub_vibe_sha256_file(const char *path, uint8_t sha256[MINIHUB_VIBE_SHA256_LEN],
                                   size_t *image_size_out);
/** Refuse a staged OTA .bin whose esp_app_desc.project_name differs from
 *  expected (e.g. minihub_lcd). expected NULL/empty skips the check (blank
 *  ota_0). Returns ESP_ERR_INVALID_VERSION on mismatch. */
esp_err_t minihub_vibe_require_project_name(const char *firmware_path,
                                            const char *expected_project);

/* ---- main-application side: stage the image, then enter Vibe Mode ---- */
esp_err_t minihub_vibe_stage_begin(size_t expected_size);
esp_err_t minihub_vibe_stage_write(const void *data, size_t len);
/** Close the file and arm the one-shot pending flag. */
esp_err_t minihub_vibe_stage_finish(void);
void minihub_vibe_stage_abort(void);
/** Select the factory app through otadata and reboot after a short delay. */
esp_err_t minihub_vibe_enter(void);

/* ---- Vibe Mode side: copy to ota_0, then select it ---- */
/** Returns ESP_OK with *copied=false when ota_0 already has the same
 *  app_elf_sha256 as the staged file. force_recovery=true: expect SHA from
 *  sidecar only (no NVS). */
esp_err_t minihub_vibe_apply(const char *firmware_path, bool *copied, bool force_recovery);
/** Rewrite otadata so the next boot returns to ota_0. */
esp_err_t minihub_vibe_return_to_app(void);

#ifdef __cplusplus
}
#endif
