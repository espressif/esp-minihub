/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_elf.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "minihub_config.h"
#include "minihub_hub.h"
#include "minihub_ble_gatt.h"
#include "minihub_net.h"
#include <errno.h>

/* Generated from build/modules/libadapter_*.so (tools/gen_host_export_symbols.py) */
extern const struct esp_elfsym s_minihub_module_dep_elfsyms[];

/* Always exported — older .mhub may need these even if current .so don't. */
static const struct esp_elfsym s_minihub_host_elfsyms[] = {
    ESP_ELFSYM_EXPORT(minihub_config_get_adapters),
    ESP_ELFSYM_EXPORT(minihub_config_save_adapters),
    ESP_ELFSYM_EXPORT(minihub_hub_reload),
    ESP_ELFSYM_EXPORT(esp_timer_get_time),
    ESP_ELFSYM_EXPORT(minihub_net_dns_probe),
    ESP_ELFSYM_EXPORT(minihub_net_has_ip),
    /* libc helpers frequently pulled by HTTP/cloud plugins */
    ESP_ELFSYM_EXPORT(strdup),
    ESP_ELFSYM_EXPORT(malloc),
    ESP_ELFSYM_EXPORT(free),
    /* Spec extras harvest / low-heap skip (dynamic cloud modules) */
    ESP_ELFSYM_EXPORT(heap_caps_get_free_size),
    /* Generic BLE GATT client for FE95-class / vendor BLE modules */
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_init),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_deinit),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_ready),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_scan_start),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_scan_stop),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_scan_get),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_connect),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_disconnect),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_is_connected),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_conn_handle),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_discover_svc),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_find_char),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_subscribe),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_write),
    ESP_ELFSYM_EXPORT(minihub_ble_gatt_wait_notify),
    ESP_ELFSYM_END
};

esp_err_t minihub_module_host_symbols_register(void)
{
    const struct esp_elfsym *tables[] = {
        s_minihub_host_elfsyms,
        s_minihub_module_dep_elfsyms,
    };

    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        int ret = esp_elf_register_symbol((esp_elf_symbol_table_t *)tables[i]);
        if (ret != 0 && ret != -EEXIST) {
            ESP_LOGE("module_sym", "host symbol register failed: %d", ret);
            return ESP_FAIL;
        }
    }

    ESP_LOGI("module_sym", "registered host symbols for dynamic modules");
    return ESP_OK;
}
