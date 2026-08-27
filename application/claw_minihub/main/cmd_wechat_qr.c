/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_wechat_qr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"

esp_err_t wechat_qr_login_persist_hook(const cap_im_wechat_qr_login_status_t *status);
#endif

#define TAG "CMD_WECHAT_QR"

static struct {
    struct arg_lit *start;
    struct arg_lit *status;
    struct arg_lit *cancel;
    struct arg_lit *persisted;
    struct arg_lit *force;
    struct arg_str *account_id;
    struct arg_end *end;
} wechat_qr_args;

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static int log_error(const char *command, esp_err_t err, const char *message)
{
    ESP_LOGW(TAG, "cmd=%s ok=0 err=%s msg=%s", command, esp_err_to_name(err), message);
    return 1;
}

static int print_status(const char *command)
{
    cap_im_wechat_qr_login_status_t status = {0};
    esp_err_t err = cap_im_wechat_qr_login_get_status(&status);
    if (err != ESP_OK) {
        return log_error(command, err, "query_status_failed");
    }

    ESP_LOGI(TAG,
             "cmd=%s ok=1 active=%d completed=%d persisted=%d configured=%d "
             "account_id=%s user_id=%s status=%s message=%s qr_data_url=%s",
             command,
             status.active,
             status.completed,
             status.persisted,
             status.configured,
             status.account_id[0] ? status.account_id : "-",
             status.user_id[0] ? status.user_id : "-",
             status.status[0] ? status.status : "-",
             status.message[0] ? status.message : "-",
             status.qr_data_url[0] ? status.qr_data_url : "-");

    if (status.completed && status.token[0]) {
        ESP_LOGI(TAG, "cmd=%s token_ready=1 token_len=%u", command, (unsigned)strlen(status.token));
    }
    if (status.completed && !status.configured) {
        ESP_LOGW(TAG, "cmd=%s msg=qr_done_but_gateway_not_configured run=wechat_qr --mark-persisted",
                 command);
    }
    return 0;
}

static int cmd_start(void)
{
    const char *account_id = wechat_qr_args.account_id->count ? wechat_qr_args.account_id->sval[0] : NULL;
    const bool force = wechat_qr_args.force->count > 0;
    esp_err_t err = cap_im_wechat_qr_login_start(account_id, force);
    if (err != ESP_OK) {
        return log_error("start", err, "start_qr_login_failed");
    }
    return print_status("start");
}

static int cmd_cancel(void)
{
    esp_err_t err = cap_im_wechat_qr_login_cancel();
    if (err != ESP_OK) {
        return log_error("cancel", err, "cancel_qr_login_failed");
    }
    ESP_LOGI(TAG, "cmd=cancel ok=1 msg=wechat_qr_login_cancelled");
    return 0;
}

static int cmd_mark_persisted(void)
{
    cap_im_wechat_qr_login_status_t status = {0};
    cap_im_wechat_client_config_t client = {0};
    esp_err_t err;

    err = cap_im_wechat_qr_login_get_status(&status);
    if (err != ESP_OK) {
        return log_error("mark_persisted", err, "query_status_failed");
    }
    if (!status.completed || !status.token[0]) {
        return log_error("mark_persisted", ESP_ERR_INVALID_STATE, "qr_login_not_completed");
    }

    err = wechat_qr_login_persist_hook(&status);
    if (err != ESP_OK) {
        return log_error("mark_persisted", err, "persist_hook_failed");
    }

    client.token = status.token;
    client.base_url = status.base_url[0] ? status.base_url : "https://ilinkai.weixin.qq.com";
    client.account_id = status.account_id[0] ? status.account_id : "default";
    err = cap_im_wechat_set_client_config(&client);
    if (err != ESP_OK) {
        return log_error("mark_persisted", err, "set_client_config_failed");
    }

    err = cap_im_wechat_start();
    if (err != ESP_OK) {
        return log_error("mark_persisted", err, "gateway_start_failed");
    }

    err = cap_im_wechat_qr_login_mark_persisted();
    if (err != ESP_OK) {
        return log_error("mark_persisted", err, "mark_persisted_failed");
    }

    ESP_LOGI(TAG, "cmd=mark_persisted ok=1 msg=token_saved_and_gateway_configured");
    return print_status("mark_persisted");
}
#endif

static int wechat_qr_cmd_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wechat_qr_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wechat_qr_args.end, argv[0]);
        return 1;
    }

    int op_count = wechat_qr_args.start->count + wechat_qr_args.status->count +
                   wechat_qr_args.cancel->count + wechat_qr_args.persisted->count;
    if (op_count != 1) {
        ESP_LOGW(TAG, "cmd=parse ok=0 msg=exactly_one_of_start_status_cancel_persisted_required");
        return 1;
    }

#if !CONFIG_APP_CLAW_CAP_IM_WECHAT
    (void)argc;
    (void)argv;
    ESP_LOGW(TAG, "cmd=wechat_qr ok=0 err=%s msg=CONFIG_APP_CLAW_CAP_IM_WECHAT=n",
             esp_err_to_name(ESP_ERR_NOT_SUPPORTED));
    return 1;
#else
    if (wechat_qr_args.start->count) {
        return cmd_start();
    }
    if (wechat_qr_args.status->count) {
        return print_status("status");
    }
    if (wechat_qr_args.cancel->count) {
        return cmd_cancel();
    }
    return cmd_mark_persisted();
#endif
}

void register_wechat_qr_command(void)
{
    wechat_qr_args.start = arg_lit0(NULL, "start", "Start WeChat QR login");
    wechat_qr_args.status = arg_lit0(NULL, "status", "Query WeChat QR login status");
    wechat_qr_args.cancel = arg_lit0(NULL, "cancel", "Cancel WeChat QR login session");
    wechat_qr_args.persisted = arg_lit0(NULL, "mark-persisted", "Mark QR credentials as persisted");
    wechat_qr_args.force = arg_lit0(NULL, "force", "Force-start a new QR login session");
    wechat_qr_args.account_id = arg_str0(NULL, "account-id", "<id>", "Optional WeChat account id");
    wechat_qr_args.end = arg_end(6);

    const esp_console_cmd_t cmd = {
        .command = "wechat_qr",
        .help = "WeChat QR login helper (prints qr_data_url for scan).\n"
                "Examples:\n"
                " wechat_qr --start\n"
                " wechat_qr --status\n"
                " wechat_qr --cancel\n",
        .hint = "<--start|--status|--cancel|--mark-persisted> [--force] [--account-id=<id>]",
        .func = &wechat_qr_cmd_handler,
        .argtable = &wechat_qr_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
