/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MINIHUB_THREAD_ADAPTER

#include "minihub_adapter.h"
#include "minihub_module.h"
#include "minihub_thread.h"
#include "thread_rpc.h"

#include "esp_log.h"

static const char *TAG = "adapter_thread";
static bool s_started;

static esp_err_t thread_adapter_init(const cJSON *config)
{
    (void)config;
    s_started = false;
    /* Host-static RPC → Store 「已安装」shows as 内置 (managed=false). */
    (void)minihub_module_register_rpc("thread", thread_module_rpc);
    return ESP_OK;
}

static esp_err_t thread_adapter_start(void)
{
    /* minihub_thread_init respects adapters.thread.enabled. */
    esp_err_t err = minihub_thread_init();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "Thread BR init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    ESP_LOGI(TAG, "Thread adapter started (ready=%d)", minihub_thread_ready() ? 1 : 0);
    return ESP_OK;
}

static esp_err_t thread_adapter_stop(void)
{
    s_started = false;
    return minihub_thread_stop();
}

static esp_err_t thread_adapter_handle_command(const minihub_command_t *cmd)
{
    (void)cmd;
    /* Matter devices live under adapters.matter; Thread is RF/BR only. */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t thread_adapter_poll(void)
{
    (void)s_started;
    return ESP_OK;
}

const minihub_adapter_t thread_adapter = {
    .name = "thread",
    .version = "0.1.0",
    .init = thread_adapter_init,
    .start = thread_adapter_start,
    .stop = thread_adapter_stop,
    .handle_command = thread_adapter_handle_command,
    .poll = thread_adapter_poll,
};

REGISTER_ADAPTER(thread_adapter);

#else /* !CONFIG_MINIHUB_THREAD_ADAPTER */

/* Translation unit kept empty when Thread is disabled. */

#endif /* CONFIG_MINIHUB_THREAD_ADAPTER */
