/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host-test stand-in for the four FreeRTOS calls the device-widget registry
 * makes. The host tests are single-threaded, so the mutex is a no-op. */

#pragma once

#include <stdint.h>

typedef void *SemaphoreHandle_t;
typedef int BaseType_t;

#define pdTRUE        1
#define pdFALSE       0
#define portMAX_DELAY ((uint32_t)0xFFFFFFFFu)
