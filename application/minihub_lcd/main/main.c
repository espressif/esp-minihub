/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_flash_dispatcher.h"
#include "esp_private/esp_sys_event_app_init.h"

#include "minihub_core.h"
#include "minihub_tz.h"
#include "minihub_config.h"
#include "minihub_adapter.h"
#include "minihub_rules.h"
#include "minihub_scenes.h"
#include "minihub_web.h"
#include "minihub_pack.h"
#include "minihub_module.h"
#include "minihub_js.h"
#include "minihub_auth.h"
#include "minihub_ota.h"
#include "minihub_provision.h"
#include "minihub_net.h"
#if CONFIG_MINIHUB_MQTT_BROKER
#include "minihub_mqtt_broker.h"
#endif
#if CONFIG_MINIHUB_LCD_UI
#include "minihub_lcd.h"
#endif
#include "minihub_usb_msc.h"
#include "minihub_rtsp_cam.h"
#include "adapters_setup.h"
#if CONFIG_MINIHUB_MATTER_ADAPTER
#include "minihub_matter.h"
#endif
#if CONFIG_MINIHUB_THREAD_ADAPTER
#include "minihub_thread.h"
#endif
#if CONFIG_MINIHUB_RAINMAKER
#include "minihub_rainmaker.h"
#endif
#if CONFIG_MINIHUB_MCP_SERVER
#include "cap_mcp_server.h"
#include "minihub_mcp_server.h"
#endif

static const char *TAG = "minihub";

/*
 * PRE_APP_MAIN priority 130: after heap (100), internal DMA reserve (110),
 * and task WDT (120); before esp_ota_auto_confirm (200). Lower runs first.
 * Scheduler and heap are up here; the flash driver was brought up in system
 * init. Not idempotent — this is the only call on the boot path.
 */
ESP_PRE_APP_MAIN_HANDLER_REGISTER(minihub_flash_dispatcher_init, 130)
{
    (void)user_arg;
    (void)ctx;

    const esp_flash_dispatcher_config_t flash_disp_cfg = {
        .task_stack_size = 4096,
        .task_priority = 10,
        .task_core_id = tskNO_AFFINITY,
        .queue_size = 1,
    };
    esp_err_t err = esp_flash_dispatcher_init(&flash_disp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_flash_dispatcher_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "esp_flash_dispatcher ready");
    return ESP_OK;
}

static void sntp_init_sync(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "MiniHub starting...");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return;
    }

    minihub_tz_apply();

    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&wdt_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "task_wdt config: %s", esp_err_to_name(err));
    }

    err = minihub_factory_reset_button_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "factory-reset button start failed: %s", esp_err_to_name(err));
    }

    err = minihub_auth_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "auth init failed");
    }

    err = minihub_ota_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ota init failed");
    }

    err = minihub_core_init();
    if (err != ESP_OK) {
        return;
    }

#if CONFIG_MINIHUB_MCP_SERVER
    err = cap_mcp_server_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cap_mcp_server_init failed: %s", esp_err_to_name(err));
    } else {
        err = minihub_mcp_server_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MCP server init failed: %s", esp_err_to_name(err));
        } else {
            err = minihub_mcp_server_register_tools();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "MCP register tools failed: %s", esp_err_to_name(err));
            }
        }
    }
#endif

    err = minihub_config_init();
    if (err != ESP_OK) {
        return;
    }

#if CONFIG_MINIHUB_NET_WIFI_STA
    /* Pre-wifi-adapter LittleFS may lack adapters.wifi; STA used to start from
     * app_main unconditionally. Create an enabled slot once so load_all finds it. */
    (void)minihub_config_ensure_adapter_slot("wifi", false);
#endif

    err = minihub_adapter_registry_init();
    if (err != ESP_OK) {
        return;
    }

    err = minihub_protocol_adapters_register_all();
    if (err != ESP_OK) {
        return;
    }

    err = minihub_module_loader_init();
    if (err != ESP_OK) {
        return;
    }

    cJSON *adapters_cfg = minihub_config_get_adapters();

    err = minihub_module_load_all(adapters_cfg);
    if (err != ESP_OK) {
        return;
    }

    err = minihub_pack_init();
    if (err != ESP_OK) {
        return;
    }

    err = minihub_pack_merge_into_adapters(adapters_cfg);
    if (err != ESP_OK) {
        return;
    }

    err = minihub_adapter_load_all(adapters_cfg);
    if (err != ESP_OK) {
        return;
    }

    /* Net before Matter/Thread. Default route follows GOT_IP (ETH, else Wi-Fi, else prefer). */
    err = minihub_net_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "net init failed: %s (continuing for local UI)", esp_err_to_name(err));
    }

#if CONFIG_MINIHUB_THREAD_ADAPTER
    {
        /* Start Thread BR before Matter. CHIP platform Dnssd uses OT SRP/DNS
         * client; early Matter bootstrap + later adapter start() double-called
         * esp_matter::start and left the commissioner forever not-ready. */
        err = minihub_thread_init();
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Thread BR init failed: %s", esp_err_to_name(err));
        }
    }
#endif

    sntp_init_sync();

    /* Bring up HTTP before radio stacks so UI stays reachable during Zigbee form. */
    err = minihub_rules_init(minihub_config_get_rules());
    if (err != ESP_OK) {
        return;
    }

    err = minihub_scenes_init(minihub_config_get_scenes());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scenes init failed");
    }

    err = minihub_web_start(80);
    if (err != ESP_OK) {
        return;
    }

#if CONFIG_MINIHUB_MQTT_BROKER
    /* Site broker before the radio stacks: they can take a while to come up, and
     * a refused bind here retries on its own task instead of failing the boot. */
    err = minihub_mqtt_broker_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT broker init failed: %s", esp_err_to_name(err));
    }
#endif

    err = minihub_adapter_start_all();
    if (err != ESP_OK) {
        return;
    }

#if CONFIG_MINIHUB_THREAD_ADAPTER && CONFIG_MINIHUB_MATTER_ADAPTER
    if (minihub_matter_ready() && minihub_thread_ready()) {
        err = minihub_matter_attach_thread();
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Matter Thread attach: %s", esp_err_to_name(err));
        }
    }
#endif

#if CONFIG_MINIHUB_LCD_UI && CONFIG_MINIHUB_MATTER_ADAPTER
    /* BT controller needs internal DMA heap; init display after Matter/BLE. */
    err = minihub_lcd_start_hw();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD HW init failed: %s", esp_err_to_name(err));
    }
    minihub_heap_log();
#endif

    /* L2 / hybrid main.js after adapters started (same order as hub_reload). */
    err = minihub_js_reload();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "JS plugin reload failed: %s", esp_err_to_name(err));
    }

    /* Hybrid entry=js: load .so companion after JS entry ran. */
    err = minihub_module_load_deferred(adapters_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deferred module load failed: %s", esp_err_to_name(err));
    }

    /* USB Host MSC is RAM-heavy — start after Zigbee/Matter/BLE stacks. */
#if defined(CONFIG_MINIHUB_USB_MSC) && CONFIG_MINIHUB_USB_MSC
    err = minihub_usb_msc_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "USB MSC init failed: %s", esp_err_to_name(err));
    }
#endif
#if defined(CONFIG_MINIHUB_RTSP_CAM) && CONFIG_MINIHUB_RTSP_CAM
    err = minihub_rtsp_cam_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTSP cam init failed: %s", esp_err_to_name(err));
    } else {
        err = minihub_rtsp_cam_register_rpc();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RTSP cam RPC register failed: %s", esp_err_to_name(err));
        }
        err = minihub_rtsp_cam_register_http(minihub_web_httpd());
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RTSP cam HTTP register failed: %s", esp_err_to_name(err));
        }
    }
#endif

#if CONFIG_MINIHUB_RAINMAKER
    err = minihub_rainmaker_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RainMaker start failed: %s", esp_err_to_name(err));
    }
#endif

#if CONFIG_MINIHUB_LCD_UI && CONFIG_MINIHUB_MATTER_ADAPTER
    err = minihub_lcd_start_ui();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD UI mount failed: %s", esp_err_to_name(err));
    }
#endif

    err = minihub_adapter_start_poll_task();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adapter poll task failed");
    }

#if CONFIG_MINIHUB_LCD_UI && !CONFIG_MINIHUB_MATTER_ADAPTER
    err = minihub_lcd_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD UI start failed: %s", esp_err_to_name(err));
    }
#endif

    const char *net_state = "pending";
    EventGroupHandle_t net_eg = minihub_net_event_group();
    if (minihub_net_has_ip() ||
        (net_eg != NULL && (xEventGroupGetBits(net_eg) & MINIHUB_NET_GOT_IP_BIT))) {
        net_state = "up";
    }
    minihub_event_bus_publish("hub.ready", NULL);
#if CONFIG_MINIHUB_MCP_SERVER
    minihub_mcp_set_hub_ready(true);
    if (minihub_net_has_ip()) {
        minihub_mcp_notify_got_ip();
    }
#endif
    ESP_LOGI(TAG, "MiniHub ready (net=%s prov=%s web_ui=%s lcd_ui=%s eth=%d wifi=%d)",
             net_state,
             minihub_provision_active() ? "active" : "off",
#if defined(CONFIG_MINIHUB_WEB_UI) && CONFIG_MINIHUB_WEB_UI
             "on",
#else
             "off",
#endif
#if defined(CONFIG_MINIHUB_LCD_UI) && CONFIG_MINIHUB_LCD_UI
             "on",
#else
             "off",
#endif
#if CONFIG_MINIHUB_NET_ETH
             1,
#else
             0,
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
             1);
#else
             0);
#endif

    err = minihub_heap_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heap monitor start failed: %s", esp_err_to_name(err));
    }
}
