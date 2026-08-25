/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

/** Create a FreeRTOS task with stack in PSRAM (requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY).
 * Prefer this for MiniHub tasks; esp_flash_dispatcher makes NVS/flash safe from PSRAM stacks.
 */
static inline BaseType_t minihub_task_create_psram(TaskFunction_t fn, const char *name, uint32_t stack_words,
                                                   void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreateWithCaps(fn, name, stack_words, arg, prio, out, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/**
 * Create a FreeRTOS task with stack in internal DRAM.
 * Prefer minihub_task_create_psram unless a vendor stack forbids external stacks.
 */
static inline BaseType_t minihub_task_create_internal(TaskFunction_t fn, const char *name, uint32_t stack_words,
                                                      void *arg, UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreateWithCaps(fn, name, stack_words, arg, prio, out, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
