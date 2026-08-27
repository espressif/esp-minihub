#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

esp_err_t cap_account_bind_register_group(void);
esp_err_t account_bind_handle_command(const char *command,
                                      bool is_group_chat,
                                      char *output,
                                      size_t output_size);
