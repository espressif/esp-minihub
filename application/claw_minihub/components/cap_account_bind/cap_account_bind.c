/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Account-bind Claw capability. Vendor clouds ship as Store plugins —
 * install via MiniHub Store, then use Web UI or serial `module --rpc`.
 */
#include "cap_account_bind.h"
#include "account_bind_priv.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "esp_log.h"
#include "minihub_net.h"

static const char *TAG = "cap_account_bind";

static const char *s_usage =
    "Usage:\n"
    "/account help\n"
    "/account providers\n"
    "/account status <provider>\n"
    "/account bind <provider> <username> <password>\n"
    "/account send <provider> [code]\n"
    "/account verify <provider> <code>\n"
    "/account logout <provider>\n"
    "\n"
    "Cloud plugins: install from MiniHub Store, then Web login or\n"
    "  module --list\n"
    "  module --rpc --id <plugin> --method <name> [--params <json>]\n"
    "WeChat: use the device portal / WeChat QR flow.";

static const account_bind_provider_t s_placeholder_providers[] = {
    {
        .id = "tuya",
        .display_name = "Tuya",
        .implemented = false,
        .bind = NULL,
        .status = NULL,
        .logout = NULL,
        .send = NULL,
        .verify = NULL,
    },
};

static const account_bind_provider_t *s_providers[] = {
    &s_placeholder_providers[0],
};

void account_bind_append_minihub_login_link(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return;
    }
    size_t len = strlen(output);
    char ip[16] = {0};
    (void)minihub_net_get_ip(ip, sizeof(ip));
    if (len + 96 >= output_size) {
        return;
    }
    if (ip[0]) {
        snprintf(output + len, output_size - len,
                 "\n\nOr open MiniHub Web login:\nhttp://%s/minihub?login=miot\n", ip);
    } else {
        snprintf(output + len, output_size - len,
                 "\n\nOr open MiniHub Web login (check device IP on screen):\n/minihub?login=miot\n");
    }
}

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static const char *skip_space(const char *text)
{
    if (!text) {
        return "";
    }
    while (*text && is_space(*text)) {
        text++;
    }
    return text;
}

static esp_err_t read_token(const char **cursor, char *token, size_t token_size, bool *out_has_token)
{
    const char *start;
    size_t len = 0;

    if (!cursor || !*cursor || !token || token_size == 0 || !out_has_token) {
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';
    *out_has_token = false;
    start = skip_space(*cursor);
    if (!start[0]) {
        *cursor = start;
        return ESP_OK;
    }
    while (start[len] && !is_space(start[len])) {
        len++;
    }
    if (len >= token_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(token, start, len);
    token[len] = '\0';
    *cursor = start + len;
    *out_has_token = true;
    return ESP_OK;
}

static esp_err_t read_rest(const char **cursor, char *token, size_t token_size, bool *out_has_token)
{
    const char *start;
    size_t len;

    if (!cursor || !*cursor || !token || token_size == 0 || !out_has_token) {
        return ESP_ERR_INVALID_ARG;
    }
    token[0] = '\0';
    *out_has_token = false;
    start = skip_space(*cursor);
    if (!start[0]) {
        *cursor = start;
        return ESP_OK;
    }
    len = strlen(start);
    if (len >= token_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(token, start, len);
    token[len] = '\0';
    *cursor = start + len;
    *out_has_token = true;
    return ESP_OK;
}

static void trim_inplace(char *s)
{
    size_t len;

    if (!s) {
        return;
    }
    while (s[0] && is_space((unsigned char)s[0])) {
        memmove(s, s + 1, strlen(s));
    }
    len = strlen(s);
    while (len > 0 && is_space((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static esp_err_t read_bind_credentials(const char **cursor, char *user, size_t user_size, char *pass,
                                       size_t pass_size, bool *out_has_user, bool *out_has_pass)
{
    const char *start;
    const char *pipe;
    esp_err_t err;

    if (!cursor || !user || !pass || !out_has_user || !out_has_pass) {
        return ESP_ERR_INVALID_ARG;
    }
    user[0] = '\0';
    pass[0] = '\0';
    *out_has_user = false;
    *out_has_pass = false;

    start = skip_space(*cursor);
    if (!start[0]) {
        *cursor = start;
        return ESP_OK;
    }

    pipe = strchr(start, '|');
    if (pipe && pipe > start) {
        size_t ulen = (size_t)(pipe - start);
        if (ulen >= user_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(user, start, ulen);
        user[ulen] = '\0';
        trim_inplace(user);
        if (strlcpy(pass, pipe + 1, pass_size) >= pass_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        trim_inplace(pass);
        *out_has_user = user[0] != '\0';
        *out_has_pass = pass[0] != '\0';
        *cursor = start + strlen(start);
        return ESP_OK;
    }

    {
        const char *c = start;
        err = read_token(&c, user, user_size, out_has_user);
        if (err != ESP_OK) {
            return err;
        }
        err = read_rest(&c, pass, pass_size, out_has_pass);
        if (err != ESP_OK) {
            return err;
        }
        *cursor = c;
    }
    return ESP_OK;
}

static esp_err_t read_command_input(const char *input_json, char *command, size_t command_size)
{
    cJSON *root = NULL;
    cJSON *item = NULL;

    if (!command || command_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    command[0] = '\0';
    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }
    root = cJSON_Parse(input_json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (cJSON_IsString(item) && item->valuestring) {
        if (strlcpy(command, item->valuestring, command_size) >= command_size) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static bool context_is_obvious_group_chat(const claw_cap_call_context_t *ctx)
{
    const char *chat_id = ctx ? ctx->chat_id : NULL;

    if (!chat_id || !chat_id[0]) {
        return false;
    }
    return strstr(chat_id, "group") != NULL ||
           strstr(chat_id, "room") != NULL ||
           strstr(chat_id, "guild") != NULL;
}

const account_bind_provider_t *account_bind_providers(void)
{
    return s_providers[0];
}

size_t account_bind_provider_count(void)
{
    return sizeof(s_providers) / sizeof(s_providers[0]);
}

const account_bind_provider_t *account_bind_find_provider(const char *id)
{
    if (!id || !id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < account_bind_provider_count(); i++) {
        if (strcmp(s_providers[i]->id, id) == 0) {
            return s_providers[i];
        }
    }
    return NULL;
}

static void write_providers_list(char *output, size_t output_size)
{
    size_t offset = 0;
    int written = snprintf(output, output_size, "Account providers:\n");
    if (written < 0 || (size_t)written >= output_size) {
        return;
    }
    offset = (size_t)written;

    for (size_t i = 0; i < account_bind_provider_count(); i++) {
        const account_bind_provider_t *provider = s_providers[i];
        written = snprintf(output + offset,
                           output_size - offset,
                           "- %s (%s): %s\n",
                           provider->id,
                           provider->display_name,
                           provider->implemented ? "implemented" : "not implemented");
        if (written < 0 || (size_t)written >= output_size - offset) {
            return;
        }
        offset += (size_t)written;
    }
    written = snprintf(output + offset, output_size - offset,
                       "\nStore plugins: module --list / Web /api/modules.\n");
    (void)written;
}

static esp_err_t dispatch_provider_command(const account_bind_provider_t *provider,
                                           const char *subcommand,
                                           const char *cursor,
                                           char *output,
                                           size_t output_size)
{
    char user[128];
    char pass[128];
    bool has_user = false;
    bool has_pass = false;

    if (strcmp(subcommand, "status") == 0) {
        if (!provider->status) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider->id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        return provider->status(output, output_size);
    }

    if (strcmp(subcommand, "logout") == 0) {
        if (!provider->logout) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider->id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        return provider->logout(output, output_size);
    }

    if (strcmp(subcommand, "bind") == 0) {
        if (!provider->bind) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider->id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        if (read_bind_credentials(&cursor, user, sizeof(user), pass, sizeof(pass), &has_user,
                                  &has_pass) != ESP_OK) {
            return ESP_FAIL;
        }
        if (!has_user || !has_pass) {
            snprintf(output, output_size,
                     "Usage: /account bind %s <username> <password>", provider->id);
            account_bind_append_minihub_login_link(output, output_size);
            return ESP_OK;
        }
        return provider->bind(user, pass, output, output_size);
    }

    if (strcmp(subcommand, "send") == 0) {
        char icode[32];
        bool has_icode = false;
        if (!provider->send) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider->id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        if (read_token(&cursor, icode, sizeof(icode), &has_icode) != ESP_OK) {
            return ESP_FAIL;
        }
        return provider->send(has_icode ? icode : NULL, output, output_size);
    }

    if (strcmp(subcommand, "verify") == 0) {
        char ticket[64];
        bool has_ticket = false;
        if (!provider->verify) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider->id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        if (read_token(&cursor, ticket, sizeof(ticket), &has_ticket) != ESP_OK) {
            return ESP_FAIL;
        }
        if (!has_ticket) {
            snprintf(output, output_size, "Usage: /account verify <provider> <code>");
            return ESP_OK;
        }
        return provider->verify(ticket, output, output_size);
    }

    strlcpy(output, s_usage, output_size);
    return ESP_OK;
}

esp_err_t account_bind_handle_command(const char *command,
                                      bool is_group_chat,
                                      char *output,
                                      size_t output_size)
{
    char normalized[512];
    char subcommand[32];
    char provider_id[32];
    const char *cursor = NULL;
    bool has_subcommand = false;
    bool has_provider = false;
    esp_err_t err = ESP_OK;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';
    if (!command) {
        command = "";
    }

    if (strlcpy(normalized, command, sizeof(normalized)) >= sizeof(normalized)) {
        snprintf(output, output_size, "Account command too long.");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "account command: %s", normalized);
    cursor = normalized;

    err = read_token(&cursor, subcommand, sizeof(subcommand), &has_subcommand);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Account command parse failed.");
        return err;
    }

    if (!has_subcommand || strcmp(subcommand, "help") == 0 || strcmp(subcommand, "?") == 0) {
        strlcpy(output, s_usage, output_size);
        return ESP_OK;
    }

    if (strcmp(subcommand, "providers") == 0) {
        write_providers_list(output, output_size);
        return ESP_OK;
    }

    if ((strcmp(subcommand, "bind") == 0 || strcmp(subcommand, "send") == 0 ||
         strcmp(subcommand, "verify") == 0) &&
            is_group_chat) {
        strlcpy(output,
                "For safety, bind and 2FA codes in a private chat or the device settings page.",
                output_size);
        return ESP_OK;
    }

    if (strcmp(subcommand, "status") == 0 || strcmp(subcommand, "logout") == 0 ||
            strcmp(subcommand, "bind") == 0 || strcmp(subcommand, "send") == 0 ||
            strcmp(subcommand, "verify") == 0) {
        err = read_token(&cursor, provider_id, sizeof(provider_id), &has_provider);
        if (err != ESP_OK) {
            return err;
        }
        if (!has_provider) {
            snprintf(output, output_size, "Usage: /account %s <provider> …", subcommand);
            return ESP_OK;
        }
        const account_bind_provider_t *provider = account_bind_find_provider(provider_id);
        if (!provider || !provider->implemented) {
            snprintf(output, output_size, "provider %s not implemented\n\n", provider_id);
            write_providers_list(output + strlen(output), output_size - strlen(output));
            return ESP_OK;
        }
        return dispatch_provider_command(provider, subcommand, cursor, output, output_size);
    }

    strlcpy(output, s_usage, output_size);
    return ESP_OK;
}

static esp_err_t account_bind_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    char command[512];
    esp_err_t err = read_command_input(input_json, command, sizeof(command));
    if (err != ESP_OK) {
        return err;
    }
    return account_bind_handle_command(command,
                                       context_is_obvious_group_chat(ctx),
                                       output,
                                       output_size);
}

static const claw_cap_descriptor_t s_account_bind_caps[] = {
    {
        .id = "account_bind_command",
        .name = "account_bind_command",
        .family = "app",
        .description = "Handle /account third-party bind commands.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_RESTRICTED,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}",
        .execute = account_bind_execute,
    },
};

static const claw_cap_group_t s_account_bind_group = {
    .group_id = "cap_account_bind",
    .plugin_name = "cap_account_bind",
    .version = "1.0.0",
    .descriptors = s_account_bind_caps,
    .descriptor_count = sizeof(s_account_bind_caps) / sizeof(s_account_bind_caps[0]),
};

esp_err_t cap_account_bind_register_group(void)
{
    if (claw_cap_group_exists(s_account_bind_group.group_id)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Registering cap group %s", s_account_bind_group.group_id);
    return claw_cap_register_group(&s_account_bind_group);
}
