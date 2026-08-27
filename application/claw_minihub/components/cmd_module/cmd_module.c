/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plugin-agnostic serial helpers for dynamic Store modules.
 * Example:
 *   module --list
 *   module --rpc --id <plugin> --method status
 *   module --rpc --id <plugin> --method login --params {"username":"...","password":"..."}
 */
#include "cmd_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "minihub_module.h"

#define TAG "CMD_MODULE"

static struct {
    struct arg_lit *list;
    struct arg_lit *rpc;
    struct arg_str *id;
    struct arg_str *method;
    struct arg_str *params;
    struct arg_end *end;
} s_args;

static int cmd_module_list(void)
{
    cJSON *arr = minihub_module_list_json();
    if (!arr) {
        ESP_LOGW(TAG, "cmd=module ok=0 msg=list failed");
        return 1;
    }
    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        ESP_LOGW(TAG, "cmd=module ok=0 msg=oom");
        return 1;
    }
    printf("%s\n", printed);
    free(printed);
    ESP_LOGI(TAG, "cmd=module ok=1 action=list count=%u",
             (unsigned)minihub_module_count());
    return 0;
}

static int cmd_module_rpc(void)
{
    const char *module_id;
    const char *method;
    cJSON *params = NULL;
    cJSON *resp = NULL;
    esp_err_t err;

    if (!s_args.id->count || !s_args.method->count) {
        ESP_LOGW(TAG, "cmd=module ok=0 msg=rpc needs --id and --method");
        return 1;
    }
    module_id = s_args.id->sval[0];
    method = s_args.method->sval[0];
    if (!module_id || !module_id[0] || !method || !method[0]) {
        ESP_LOGW(TAG, "cmd=module ok=0 msg=empty id/method");
        return 1;
    }

    if (s_args.params->count) {
        params = cJSON_Parse(s_args.params->sval[0]);
        if (!params) {
            ESP_LOGW(TAG, "cmd=module ok=0 msg=invalid --params JSON");
            return 1;
        }
    }

    err = minihub_module_invoke_rpc(module_id, method, params, &resp);
    cJSON_Delete(params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd=module ok=0 id=%s method=%s err=%s", module_id, method,
                 esp_err_to_name(err));
        cJSON_Delete(resp);
        return 1;
    }

    if (resp) {
        char *printed = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (printed) {
            printf("%s\n", printed);
            free(printed);
        }
    } else {
        printf("{\"ok\":true}\n");
    }
    ESP_LOGI(TAG, "cmd=module ok=1 id=%s method=%s", module_id, method);
    return 0;
}

static int cmd_module_handler(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.list->count) {
        return cmd_module_list();
    }
    if (s_args.rpc->count) {
        return cmd_module_rpc();
    }

    ESP_LOGW(TAG, "cmd=module ok=0 msg=specify --list or --rpc");
    return 1;
}

void register_module_command(void)
{
    s_args.list = arg_lit0(NULL, "list", "List dynamic / Store modules (JSON)");
    s_args.rpc = arg_lit0(NULL, "rpc", "Invoke module RPC");
    s_args.id = arg_str0(NULL, "id", "<id>", "Module id (with --rpc)");
    s_args.method = arg_str0(NULL, "method", "<name>", "RPC method (with --rpc)");
    s_args.params = arg_str0(NULL, "params", "<json>", "Optional params JSON");
    s_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "module",
        .help = "Dynamic modules: --list | --rpc --id <id> --method <name> [--params <json>]",
        .hint = NULL,
        .func = &cmd_module_handler,
        .argtable = &s_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
