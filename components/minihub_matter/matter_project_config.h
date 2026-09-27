/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sdkconfig.h>

#ifndef CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER

#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY 1
#endif

#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 8

/*
 * ESP32 default is 1: only the highest-scored address is kept. Matter prefers
 * IPv6 link-local (score 7) over IPv4 (score 2). If LL send fails (ERR_RTE when
 * Hub has no IPv6 LL on ETH), commissioning cannot fall back to IPv4.
 */
#define CHIP_CONFIG_MDNS_RESOLVE_LOOKUP_RESULTS 5

/* Already the CHIP default (1); keep explicit so Thread FindOperational can
 * retry while the commissionee's _matter._tcp SRP entry is still publishing. */
#ifndef CHIP_DEVICE_CONFIG_ENABLE_AUTOMATIC_CASE_RETRIES
#define CHIP_DEVICE_CONFIG_ENABLE_AUTOMATIC_CASE_RETRIES 1
#endif

#endif
