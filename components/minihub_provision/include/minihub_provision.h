/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load Wi-Fi credentials. NVS wins when it has an SSID; otherwise Kconfig. */
esp_err_t minihub_provision_load_credentials(wifi_config_t *out_sta_cfg);

/**
 * Store STA credentials in NVS. Empty password is an open network.
 * Does not connect; the caller applies the config on the live STA.
 */
esp_err_t minihub_provision_save_credentials(const char *ssid, const char *password);

/**
 * Start provisioning using the Kconfig transport (SoftAP or BLE).
 * Prefer this over softap/ble-specific helpers.
 */
esp_err_t minihub_provision_start(void);

/** Start SoftAP provisioning without blocking (returns immediately). */
esp_err_t minihub_provision_start_softap(void);

/** Start BLE GATT provisioning without blocking (returns immediately). */
esp_err_t minihub_provision_start_ble(void);

bool minihub_provision_active(void);

/** True while provisioning manager is starting or active (suppress Wi-Fi auto-connect). */
bool minihub_provision_busy(void);

const char *minihub_provision_softap_ssid(void);

/** BLE advertising / device name used for provisioning (e.g. PROV_MiniHub). */
const char *minihub_provision_ble_device_name(void);

const char *minihub_provision_pop(void);

/**
 * Erase NVS and restart. Does not format the storage filesystem.
 * Does not return.
 */
void minihub_factory_reset_execute(void);

/** Reply to the caller first, then erase NVS and restart. */
esp_err_t minihub_factory_reset_schedule(void);

/**
 * Start BOOT-button factory reset monitor (long-press → nvs_flash_erase + restart).
 * No-op when CONFIG_MINIHUB_FACTORY_RESET_BUTTON is disabled.
 */
esp_err_t minihub_factory_reset_button_start(void);

#ifdef __cplusplus
}
#endif
