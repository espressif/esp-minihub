/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_net_cfg.h"
#include "minihub_net.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "minihub_net_cfg";
static const char *NVS_NS = "mh_net";
static const uint32_t CFG_MAGIC = 0x4D484E31; /* MHN1 */

typedef struct {
    uint32_t magic;
    uint8_t dhcp;
    uint8_t _pad[3];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gw;
    uint32_t dns_main;
    uint32_t dns_backup;
} nvs_net_blob_t;

static const char *nvs_key(minihub_net_iface_t iface)
{
    return iface == MINIHUB_NET_IF_WIFI ? "wifi" : "eth";
}

static esp_netif_t *netif_for(minihub_net_iface_t iface)
{
    if (iface == MINIHUB_NET_IF_WIFI) {
        return minihub_net_wifi_netif();
    }
    return minihub_net_eth_netif();
}

static void ip4_to_str(uint32_t addr, char *buf, size_t len)
{
    if (!buf || len < 8) {
        return;
    }
    if (addr == 0) {
        buf[0] = '\0';
        return;
    }
    esp_ip4_addr_t a = {.addr = addr};
    snprintf(buf, len, IPSTR, IP2STR(&a));
}

static bool parse_ip4(const char *s, uint32_t *out)
{
    if (!s || !s[0] || !out) {
        return false;
    }
    esp_ip4_addr_t a = {0};
    if (esp_netif_str_to_ip4(s, &a) != ESP_OK || a.addr == 0) {
        return false;
    }
    *out = a.addr;
    return true;
}

static void fill_dns_str(esp_netif_t *netif, esp_netif_dns_type_t type, char *buf, size_t len)
{
    buf[0] = '\0';
    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(netif, type, &dns) != ESP_OK) {
        return;
    }
    if (dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
        ip4_to_str(dns.ip.u_addr.ip4.addr, buf, len);
    }
}

static bool iface_compiled_in(minihub_net_iface_t iface)
{
#if CONFIG_MINIHUB_NET_ETH
    if (iface == MINIHUB_NET_IF_ETH) {
        return true;
    }
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
    if (iface == MINIHUB_NET_IF_WIFI) {
        return true;
    }
#endif
    (void)iface;
    return false;
}

esp_err_t minihub_net_get_iface_status(minihub_net_iface_t iface, minihub_net_iface_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->present = iface_compiled_in(iface);
    out->dhcp = true;
    if (!out->present) {
        return ESP_OK;
    }
    esp_netif_t *netif = netif_for(iface);
    if (!netif) {
        return ESP_OK;
    }
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        ip4_to_str(info.ip.addr, out->ip, sizeof(out->ip));
        ip4_to_str(info.netmask.addr, out->netmask, sizeof(out->netmask));
        ip4_to_str(info.gw.addr, out->gw, sizeof(out->gw));
    }
    /* up = has IPv4; netif existing alone used to look "connected" with blank fields. */
    out->up = out->ip[0] != '\0';
    if (out->up) {
        fill_dns_str(netif, ESP_NETIF_DNS_MAIN, out->dns_main, sizeof(out->dns_main));
        fill_dns_str(netif, ESP_NETIF_DNS_BACKUP, out->dns_backup, sizeof(out->dns_backup));
    }

    esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
    if (esp_netif_dhcpc_get_status(netif, &st) == ESP_OK) {
        if (st == ESP_NETIF_DHCP_STARTED) {
            out->dhcp = true;
        } else if (st == ESP_NETIF_DHCP_STOPPED) {
            out->dhcp = false;
        }
    }
    return ESP_OK;
}

static esp_err_t save_blob(minihub_net_iface_t iface, const nvs_net_blob_t *blob)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs open");
    esp_err_t err = nvs_set_blob(h, nvs_key(iface), blob, sizeof(*blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t load_blob(minihub_net_iface_t iface, nvs_net_blob_t *blob)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t sz = sizeof(*blob);
    err = nvs_get_blob(h, nvs_key(iface), blob, &sz);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    if (blob->magic != CFG_MAGIC) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t set_dns4(esp_netif_t *netif, esp_netif_dns_type_t type, uint32_t addr)
{
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = addr;
    return esp_netif_set_dns_info(netif, type, &dns);
}

static esp_err_t apply_blob(esp_netif_t *netif, const nvs_net_blob_t *blob)
{
    if (!netif || !blob) {
        return ESP_ERR_INVALID_ARG;
    }
    if (blob->dhcp) {
        esp_err_t err = esp_netif_dhcpc_start(netif);
        if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            err = ESP_OK;
        }
        return err;
    }

    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "dhcpc_stop: %s", esp_err_to_name(err));
    }

    esp_netif_ip_info_t info = {0};
    info.ip.addr = blob->ip;
    info.netmask.addr = blob->netmask;
    info.gw.addr = blob->gw;
    err = esp_netif_set_ip_info(netif, &info);
    if (err != ESP_OK) {
        return err;
    }
    if (blob->dns_main) {
        (void)set_dns4(netif, ESP_NETIF_DNS_MAIN, blob->dns_main);
    }
    if (blob->dns_backup) {
        (void)set_dns4(netif, ESP_NETIF_DNS_BACKUP, blob->dns_backup);
    }
    return ESP_OK;
}

esp_err_t minihub_net_set_iface_config(minihub_net_iface_t iface, const minihub_net_iface_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!iface_compiled_in(iface)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_netif_t *netif = netif_for(iface);
    if (!netif) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_net_blob_t blob = {.magic = CFG_MAGIC, .dhcp = cfg->dhcp ? 1 : 0};
    if (!cfg->dhcp) {
        if (!parse_ip4(cfg->ip, &blob.ip) || !parse_ip4(cfg->netmask, &blob.netmask) ||
            !parse_ip4(cfg->gw, &blob.gw)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->dns_main[0] && !parse_ip4(cfg->dns_main, &blob.dns_main)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (cfg->dns_backup[0] && !parse_ip4(cfg->dns_backup, &blob.dns_backup)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    ESP_RETURN_ON_ERROR(save_blob(iface, &blob), TAG, "save");
    esp_err_t err = apply_blob(netif, &blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply %s failed: %s", nvs_key(iface), esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "applied %s dhcp=%d", nvs_key(iface), (int)blob.dhcp);
    return ESP_OK;
}

esp_err_t minihub_net_apply_saved_configs(void)
{
    for (int i = 0; i < 2; i++) {
        minihub_net_iface_t iface = (minihub_net_iface_t)i;
        if (!iface_compiled_in(iface)) {
            continue;
        }
        esp_netif_t *netif = netif_for(iface);
        if (!netif) {
            continue;
        }
        nvs_net_blob_t blob = {0};
        if (load_blob(iface, &blob) != ESP_OK) {
            continue;
        }
        esp_err_t err = apply_blob(netif, &blob);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restore %s: %s", nvs_key(iface), esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "restored %s dhcp=%d", nvs_key(iface), (int)blob.dhcp);
        }
    }
    return ESP_OK;
}
