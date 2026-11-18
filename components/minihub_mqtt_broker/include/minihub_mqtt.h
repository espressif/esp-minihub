/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public surface of the bundled MQTT adapter. L2 plugins do not call these
 * directly: components/minihub_js/minihub_js_mqtt.c wraps them as host.mqtt.
 * Keeping the hook in C (a plain function pointer writing into a bounded ring
 * buffer) means no JSValue ever crosses the esp-mqtt task boundary.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MINIHUB_MQTT_FILTER_MAX   96
/* Shared by every L2 plugin in the runtime. Discovery plugins use ~6 static
 * filters each plus one per HA-discovered device's state topic, so 12 was too
 * tight for two plugins. 32 * 100 B = 3.2 KB of static RAM. */
#define MINIHUB_MQTT_FILTERS_MAX  32
/* One owner per L2 plugin that touches host.mqtt. The mask the hook receives is
 * a uint32_t, so this can grow to 32 without changing the callback signature. */
#define MINIHUB_MQTT_OWNERS_MAX   8

/**
 * @brief Raw MQTT message delivered to the JS bridge.
 *
 * Called on the esp-mqtt event task. @p topic is @b NOT NUL-terminated; use
 * @p topic_len. The callback must not block and must not call QuickJS.
 *
 * @p owner_mask has bit N set for every owner whose own filter matched, so one
 * message is copied once and handed to all interested plugins instead of being
 * consumed by whichever plugin polls first.
 */
typedef void (*minihub_mqtt_hook_t)(const char *topic, int topic_len,
                                    const char *data, int data_len,
                                    uint32_t owner_mask, void *user_data);

/**
 * @brief Register a topic filter (MQTT ``+`` / ``#`` wildcards allowed) for one owner.
 *
 * Filters survive a broker reconnect: the adapter re-subscribes every distinct
 * filter on MQTT_EVENT_CONNECTED. A filter registered twice by the same owner is
 * ignored (its QoS is refreshed); the same filter registered by a second owner
 * costs no broker subscription, only a table entry. Only messages matching a
 * registered filter are forwarded to the hook, so the JS ring buffer cannot be
 * flooded by unrelated broker traffic.
 *
 * @param owner  0 .. MINIHUB_MQTT_OWNERS_MAX - 1.
 *
 * @return
 *     - ESP_OK: Registered or already registered for this owner.
 *     - ESP_ERR_INVALID_ARG: Bad filter or owner out of range.
 *     - ESP_ERR_NO_MEM: Filter table full.
 */
esp_err_t minihub_mqtt_add_filter(const char *filter, int qos, int owner);

/** @brief Remove one owner's filter; the broker is unsubscribed only when the
 *         last owner of that exact filter string is gone. */
esp_err_t minihub_mqtt_remove_filter(const char *filter, int owner);

/** @brief Remove every filter owned by one owner (single L2 plugin unload /
 *         disable). A filter held by another owner stays subscribed. */
esp_err_t minihub_mqtt_clear_owner(int owner);

/** @brief Remove every filter registered through minihub_mqtt_add_filter(). */
void minihub_mqtt_clear_filters(void);

/** @brief Install (or clear, with @p hook = NULL) the single raw-message hook. */
void minihub_mqtt_set_hook(minihub_mqtt_hook_t hook, void *user_data);

/** @brief Publish a message. @p payload is NUL-terminated text. */
esp_err_t minihub_mqtt_publish(const char *topic, const char *payload,
                               int qos, int retain);

/** @brief True between MQTT_EVENT_CONNECTED and MQTT_EVENT_DISCONNECTED. */
bool minihub_mqtt_is_connected(void);

/** @brief Number of occupied filter table entries (a filter held by two owners
 *         counts twice, because that is what consumes the table). */
size_t minihub_mqtt_filter_count(void);

#ifdef __cplusplus
}
#endif
