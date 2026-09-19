/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @file minihub_plugin_sdk.h
 * @brief Standalone Store L3 plugin SDK (umbrella include).
 *
 * Out-of-tree plugins:
 * @code
 * #include "minihub_plugin_sdk.h"
 * @endcode
 * with ``-I`` pointing at ``docs/module_sdk/include`` (or an SDK package copy)
 * and a cross compiler matching Host (ESP32-S31: ``riscv32-esp-elf-gcc``).
 *
 * Category headers (also includable individually):
 * - ``minihub_plugin_sdk/esp_idf.h`` — err, log, HTTP client, NVS, heap, timer
 * - ``minihub_plugin_sdk/lwip.h`` — ``lwip_*`` sockets
 * - ``minihub_plugin_sdk/cjson.h`` — cJSON
 * - ``minihub_plugin_sdk/mbedtls.h`` — MD / HMAC / base64
 * - ``minihub_plugin_sdk/psa.h`` — PSA Crypto subset
 * - ``minihub_plugin_sdk/freertos.h`` — tasks / mutexes / libgcc helpers
 * - ``minihub_plugin_sdk/minihub.h`` — points, ABI, HTTP worker, net, BLE GATT
 *
 * libc (``malloc``, ``memcpy``, ``fopen``, ``mkdir``, ``stat``, ``unlink``,
 * ``time``, ``close``, …) comes from the toolchain. Runtime binding:
 * ``host_export_allowlist.json``.
 * Host build runs ``tools/check_plugin_sdk.py`` when dynamic modules are on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "minihub_plugin_sdk/esp_idf.h"
#include "minihub_plugin_sdk/lwip.h"
#include "minihub_plugin_sdk/cjson.h"
#include "minihub_plugin_sdk/mbedtls.h"
#include "minihub_plugin_sdk/psa.h"
#include "minihub_plugin_sdk/freertos.h"
#include "minihub_plugin_sdk/minihub.h"
