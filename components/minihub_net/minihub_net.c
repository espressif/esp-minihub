/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_net.h"
#include "minihub_net_cfg.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#if CONFIG_MINIHUB_NET_WIFI_STA
#include "minihub_provision.h"
#endif

#if CONFIG_MINIHUB_NET_ETH
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "eth_phy_802_3_regs.h"
#include "esp_mac.h"
#endif

static const char *TAG = "minihub_net";

/* SoftAP SSID presented for web-based Wi-Fi configuration when no credentials
 * are stored. Matches the legacy network_prov SoftAP name so UX stays familiar. */
#define MINIHUB_WEB_PROV_AP_SSID "PROV_MiniHub"

static EventGroupHandle_t s_net_eg;
static bool s_started;
/* ETH is optional hardware; track whether the uplink actually came up rather
 * than just whether it was compiled in. */
static bool s_eth_up;
#if CONFIG_MINIHUB_NET_ETH
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle;
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
static esp_netif_t *s_sta_netif;
/* set_config is rejected while STA is connecting. Hold auto-reconnect across
 * disconnect so the driver can leave that state before the new config. */
static volatile bool s_wifi_hold_reconnect;
static EventGroupHandle_t s_wifi_idle_eg;
#define WIFI_STA_IDLE_BIT BIT0
/* When true, STA stays disconnected and never auto-reconnects (set by an empty
 * SSID in minihub_net_set_wifi_sta). */
static bool s_wifi_sta_disabled;
#endif

/* Optional hooks — weak so RainMaker / web / LCD can override when linked. */
__attribute__((weak)) void minihub_rainmaker_notify_got_ip(void)
{
}

__attribute__((weak)) esp_err_t minihub_web_refresh_geo_async(void)
{
    return ESP_OK;
}

__attribute__((weak)) void minihub_lcd_request_refresh(void)
{
}

static void log_netif_dns(const char *ifname, esp_netif_t *netif)
{
    if (!netif) {
        ESP_LOGW(TAG, "%s: no netif for DNS dump", ifname ? ifname : "?");
        return;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "%s ip=" IPSTR " gw=" IPSTR, ifname ? ifname : "netif", IP2STR(&ip.ip),
                 IP2STR(&ip.gw));
    }
    static const char *dns_name[] = {"main", "backup", "fallback"};
    for (int i = 0; i <= (int)ESP_NETIF_DNS_FALLBACK; i++) {
        esp_netif_dns_info_t dns = {0};
        esp_err_t err = esp_netif_get_dns_info(netif, (esp_netif_dns_type_t)i, &dns);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s dns[%s]: %s", ifname ? ifname : "netif", dns_name[i],
                     esp_err_to_name(err));
            continue;
        }
        if (dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0) {
            ESP_LOGI(TAG, "%s dns[%s]=" IPSTR, ifname ? ifname : "netif", dns_name[i],
                     IP2STR(&dns.ip.u_addr.ip4));
        } else if (dns.ip.type == ESP_IPADDR_TYPE_V6) {
            ESP_LOGI(TAG, "%s dns[%s]=IPv6 (set)", ifname ? ifname : "netif", dns_name[i]);
        } else {
            /* A router commonly supplies only main+backup DNS; an unset
             * fallback is normal, not a warning. */
            ESP_LOGD(TAG, "%s dns[%s]=(empty)", ifname ? ifname : "netif", dns_name[i]);
        }
    }
}

static bool netif_has_ip(esp_netif_t *netif)
{
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t info;
    return esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
}

/** Fallback when neither uplink has IPv4 yet: Ethernet if present, else Wi-Fi. */
static esp_netif_t *preferred_netif(void)
{
#if CONFIG_MINIHUB_NET_ETH
    if (s_eth_netif) {
        return s_eth_netif;
    }
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
    return s_sta_netif;
#else
    return NULL;
#endif
}

/** ETH with IPv4, else Wi-Fi with IPv4, else Ethernet (if built) / Wi-Fi. */
static void refresh_default_netif(void)
{
    esp_netif_t *chosen = NULL;
#if CONFIG_MINIHUB_NET_ETH
    if (netif_has_ip(s_eth_netif)) {
        chosen = s_eth_netif;
    }
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
    if (!chosen && netif_has_ip(s_sta_netif)) {
        chosen = s_sta_netif;
    }
#endif
    if (!chosen) {
        chosen = preferred_netif();
    }
    if (chosen) {
        esp_netif_set_default_netif(chosen);
    }
}

void minihub_net_refresh_default_netif(void)
{
    refresh_default_netif();
}

/** If DHCP only gave one DNS (often the router), add a public backup for resolve failures. */
static void ensure_dns_backup(esp_netif_t *netif)
{
    if (!netif) {
        return;
    }
    esp_netif_dns_info_t backup = {0};
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &backup) == ESP_OK &&
        backup.ip.type == ESP_IPADDR_TYPE_V4 && backup.ip.u_addr.ip4.addr != 0) {
        return;
    }
    /* AliDNS — better reachability than 8.8.8.8 on many CN networks. */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);
    esp_err_t err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set DNS backup 223.5.5.5 failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "DNS backup set to 223.5.5.5");
}

static void on_got_ip(const char *ifname, esp_netif_t *netif)
{
    if (s_net_eg) {
        xEventGroupSetBits(s_net_eg, MINIHUB_NET_GOT_IP_BIT);
    }

    refresh_default_netif();

    ensure_dns_backup(netif);
    ESP_LOGI(TAG, "%s got IP", ifname ? ifname : "netif");
    log_netif_dns(ifname ? ifname : "netif", netif);
    minihub_rainmaker_notify_got_ip();
    (void)minihub_web_refresh_geo_async();
    minihub_lcd_request_refresh();
}

#if CONFIG_MINIHUB_NET_WIFI_STA
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* While the provisioning manager is up it owns connect; auto-connect
         * here races set_config and yields ESP_ERR_WIFI_STATE. */
        if (!minihub_provision_busy() && !s_wifi_sta_disabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        refresh_default_netif();
        if (s_wifi_sta_disabled) {
            /* Wi-Fi explicitly disabled (empty SSID); stay disconnected. */
        } else if (s_wifi_hold_reconnect) {
            if (s_wifi_idle_eg) {
                xEventGroupSetBits(s_wifi_idle_eg, WIFI_STA_IDLE_BIT);
            }
        } else if (!minihub_provision_busy()) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        on_got_ip("wifi", s_sta_netif);
        /* Modem sleep stalls the console: chunked HTTP waits on the peer ACK
         * across DTIM, so opening the page takes tens of seconds. A wall-powered
         * hub keeps the STA awake. Thread/Matter coex used to mask this by
         * holding the radio; with both adapters disabled the sleep is real. */
        esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi set_ps failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t ensure_wifi_driver_init(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;

    if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT) {
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    return esp_wifi_init(&cfg);
}

static esp_err_t prepare_wifi(void)
{
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    } else {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "wifi event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG, "sta got ip");
    return ESP_OK;
}

static esp_err_t start_wifi_sta(void)
{
    /* Host STA. With the Matter adapter on, MINIHUB_NET_WIFI_STA selects
     * CHIP ENABLE_WIFI_STATION. Default route follows GOT_IP (ETH, else Wi-Fi). */
    ESP_RETURN_ON_ERROR(ensure_wifi_driver_init(), TAG, "wifi init");

    wifi_config_t wifi_config = {0};
    esp_err_t err = minihub_provision_load_credentials(&wifi_config);
    if (err == ESP_OK) {
        if (wifi_config.sta.ssid[0] == '\0') {
            /* Wi-Fi disabled (empty credentials): bring STA up idle so the
             * driver is ready but no uplink connection is attempted, and keep
             * it that way across reconnect/disconnect events. */
            s_wifi_sta_disabled = true;
            ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
            ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config");
            ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi set_ps(NONE) failed: %s", esp_err_to_name(err));
        }
        /* The initial connect is owned by wifi_event_handler() on
         * WIFI_EVENT_STA_START (it also skips connect while provisioning owns
         * the radio or STA is explicitly disabled). Calling esp_wifi_connect()
         * here as well raced that handler and made the Wi-Fi driver log
         * "sta is connecting, return error" on every boot. */
        return ESP_OK;
    }

#if CONFIG_MINIHUB_WIFI_PROVISIONING
    /* network_prov's SoftAP HTTP server binds port 80, colliding with the
     * MiniHub web console (also port 80) -> web unreachable after a factory
     * reset. Wi-Fi is instead configured through the web UI (POST /api/net/wifi
     * -> minihub_net_set_wifi_sta), so skip network_prov and bring up a plain
     * SoftAP purely so the web console on port 80 stays reachable. */
    ESP_LOGI(TAG, "no Wi-Fi credentials; starting SoftAP for web configuration");
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, MINIHUB_WEB_PROV_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(MINIHUB_WEB_PROV_AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    return ESP_OK;
#else
    ESP_LOGE(TAG, "no Wi-Fi credentials and provisioning disabled");
    return err;
#endif
}
#endif /* WIFI_STA */

#if CONFIG_MINIHUB_NET_ETH

#if CONFIG_MINIHUB_NET_ETH_YT8531_INIT
static esp_err_t eth_phy_yt8531_specific_init(esp_eth_handle_t eth_handle)
{
    bool auto_nego_en = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_S_AUTONEGO, &auto_nego_en), TAG, "auto nego");

    esp_eth_phy_reg_rw_data_t phy_reg = {.reg_value_p = NULL};
    uint32_t reg_val;
    phy_reg.reg_value_p = &reg_val;

    reg_val = 0xA001;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "ext addr");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG, &phy_reg), TAG, "read chip cfg");
    reg_val |= (1U << 8);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "write chip cfg");

    reg_val = 0xA003;
    phy_reg.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "ext addr rgmii");
    phy_reg.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_READ_PHY_REG, &phy_reg), TAG, "read rgmii");
    reg_val = (reg_val & ~0x00FFU) | (13U << 4) | (13U << 0);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth_handle, ETH_CMD_WRITE_PHY_REG, &phy_reg), TAG, "write rgmii");

    ESP_LOGI(TAG, "YT8531DC-CA RGMII delays configured");
    return ESP_OK;
}
#endif

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet link up, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        /* Matter CASE prefers IPv6 LLA; without a Hub LL on ETH, udp_sendto_if → ERR_RTE. */
        if (s_eth_netif) {
            esp_err_t ip6_err = esp_netif_create_ip6_linklocal(s_eth_netif);
            if (ip6_err != ESP_OK && ip6_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "create IPv6 link-local failed: %s", esp_err_to_name(ip6_err));
            }
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        refresh_default_netif();
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void eth_got_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    on_got_ip("eth", event ? event->esp_netif : s_eth_netif);
}

/* RGMII sources the EMAC clock from the PHY RX pad. With no PHY fitted or
 * clocked, the MAC software reset never completes and IDF reports
 * "emac_esp32_init: reset timeout" from esp_eth_driver_install(). Probe the
 * MDIO bus before installing the driver, so a missing PHY is an unavailable
 * optional uplink instead of a boot error. The probe also pins the address, so
 * the driver no longer needs its post-MAC-init auto-detection. */
static int eth_phy_probe_addr(esp_eth_mac_t *mac, int configured)
{
    const int first = configured >= 0 ? configured : 0;
    const int last = configured >= 0 ? configured : 31;
    for (int addr = first; addr <= last; ++addr) {
        uint32_t id1 = 0;
        if (mac->read_phy_reg(mac, (uint32_t)addr, ETH_PHY_IDR1_REG_ADDR, &id1) != ESP_OK) {
            return -1; /* MDIO bus itself is not usable */
        }
        if (id1 != 0x0000 && id1 != 0xFFFF) {
            return addr;
        }
    }
    return -1;
}

/* Roll back a half-installed optional uplink. Order follows the IDF ethernet
 * example: stop driver, drop glue, destroy netif, uninstall driver. */
static void eth_teardown(esp_eth_mac_t *mac, esp_eth_phy_t *phy,
                         esp_eth_netif_glue_handle_t glue)
{
    s_eth_up = false;
    bool installed = (s_eth_handle != NULL);
    if (installed) {
        (void)esp_eth_stop(s_eth_handle);
    }
    if (glue) {
        (void)esp_eth_del_netif_glue(glue);
    }
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    if (installed) {
        (void)esp_eth_driver_uninstall(s_eth_handle);
        s_eth_handle = NULL;
    } else {
        /* Install never succeeded, so mac/phy are still owned by us. */
        if (phy) {
            phy->del(phy);
        }
        if (mac) {
            mac->del(mac);
        }
    }
    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler);
    (void)esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
}

static esp_err_t start_eth(void)
{
    esp_err_t err;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    esp_eth_netif_glue_handle_t glue = NULL;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_MINIHUB_NET_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_MINIHUB_NET_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (!mac) {
        return ESP_FAIL;
    }

    const int addr = eth_phy_probe_addr(mac, phy_config.phy_addr);
    if (addr < 0) {
        ESP_LOGI(TAG, "Ethernet PHY not detected; ETH uplink skipped");
        mac->del(mac);
        return ESP_ERR_NOT_FOUND;
    }
    phy_config.phy_addr = addr;

    phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eth install: %s", esp_err_to_name(err));
        s_eth_handle = NULL;
        goto fail;
    }
    mac = NULL;
    phy = NULL; /* the driver owns them now */

#if CONFIG_MINIHUB_NET_ETH_YT8531_INIT
    err = eth_phy_yt8531_specific_init(s_eth_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "YT8531 init: %s", esp_err_to_name(err));
        goto fail;
    }
#endif

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    glue = esp_eth_new_netif_glue(s_eth_handle);
    if (!glue) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = esp_netif_attach(s_eth_netif, glue);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eth netif attach: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        goto fail;
    }
    s_eth_up = true;
    return ESP_OK;

fail:
    eth_teardown(mac, phy, glue);
    return err;
}
#endif /* ETH */

esp_err_t minihub_net_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

#if !CONFIG_MINIHUB_NET_WIFI_STA && !CONFIG_MINIHUB_NET_ETH
    ESP_LOGW(TAG, "no uplink enabled (WIFI_STA and ETH both off)");
    return ESP_ERR_INVALID_STATE;
#endif

    s_net_eg = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_net_eg, ESP_ERR_NO_MEM, TAG, "event group");

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

#if CONFIG_MINIHUB_NET_ETH
    err = start_eth();
    if (err != ESP_OK) {
        /* Ethernet is an optional uplink. A board without a fitted/clocked
         * RGMII PHY — or a genuine ETH init failure — must not take down
         * esp_netif, the saved IP configuration or the Wi-Fi STA. Returning
         * here used to leave s_started false, which made every later
         * minihub_net_start_wifi() call fail with ESP_ERR_INVALID_STATE. */
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGI(TAG, "Ethernet uplink not present; continuing without ETH");
        } else {
            ESP_LOGW(TAG, "Ethernet uplink unavailable: %s (continuing without ETH)",
                     esp_err_to_name(err));
        }
    }
#endif

#if CONFIG_MINIHUB_NET_WIFI_STA
    /* Prepare Wi-Fi stack only; STA connect / BLE provision runs in minihub_net_start_wifi(). */
    err = prepare_wifi();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi prepare failed: %s (continuing if ETH is up)", esp_err_to_name(err));
#if !CONFIG_MINIHUB_NET_ETH
        return err;
#endif
    }
#endif

    s_started = true;
    (void)minihub_net_apply_saved_configs();
    refresh_default_netif();
    ESP_LOGI(TAG, "uplinks prepared: wifi=%d eth=%d (wifi STA deferred)",
#if CONFIG_MINIHUB_NET_WIFI_STA
             1,
#else
             0,
#endif
#if CONFIG_MINIHUB_NET_ETH
             s_eth_up ? 1 : 0
#else
             0
#endif
    );
    return ESP_OK;
}

esp_err_t minihub_net_start_wifi(void)
{
#if !CONFIG_MINIHUB_NET_WIFI_STA
    return ESP_OK;
#else
    static bool s_wifi_started;
    if (!s_started) {
        ESP_LOGE(TAG, "minihub_net_start_wifi: call minihub_net_init first");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = start_wifi_sta();
    if (err == ESP_OK) {
        s_wifi_started = true;
    } else {
        ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    }
    return err;
#endif
}

esp_netif_t *minihub_net_get_netif(void)
{
#if CONFIG_MINIHUB_NET_ETH
    if (netif_has_ip(s_eth_netif)) {
        return s_eth_netif;
    }
#endif
#if CONFIG_MINIHUB_NET_WIFI_STA
    if (netif_has_ip(s_sta_netif)) {
        return s_sta_netif;
    }
#endif
    return NULL;
}

bool minihub_net_has_ip(void)
{
    return minihub_net_get_netif() != NULL;
}

bool minihub_net_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return false;
    }
    buf[0] = '\0';
    esp_netif_t *netif = minihub_net_get_netif();
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    return true;
}

EventGroupHandle_t minihub_net_event_group(void)
{
    return s_net_eg;
}

esp_netif_t *minihub_net_eth_netif(void)
{
#if CONFIG_MINIHUB_NET_ETH
    return s_eth_netif;
#else
    return NULL;
#endif
}

esp_netif_t *minihub_net_wifi_netif(void)
{
#if CONFIG_MINIHUB_NET_WIFI_STA
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    }
    return s_sta_netif;
#else
    return NULL;
#endif
}

void minihub_net_dns_probe(const char *hostname)
{
    esp_netif_t *netif = minihub_net_get_netif();
    if (!netif) {
        ESP_LOGW(TAG, "dns_probe: no netif with IP (hostname=%s)", hostname ? hostname : "(none)");
        return;
    }
    /* Without SET_DNS_PER_DEFAULT_NETIF, get_dns_info reads lwIP dns_getserver(). */
    ensure_dns_backup(netif);
    const char *ifkey = esp_netif_get_ifkey(netif);
    log_netif_dns(ifkey ? ifkey : "netif", netif);

    if (!hostname || hostname[0] == '\0') {
        return;
    }

    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET; /* force A-record; avoid AF_UNSPEC oddities */
    hints.ai_socktype = SOCK_STREAM;
    int64_t t0 = esp_timer_get_time();
    int gai = getaddrinfo(hostname, "443", &hints, &res);
    int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
    if (gai != 0) {
        ESP_LOGE(TAG, "dns_probe: getaddrinfo(%s) failed gai=%d errno=%d after %lld ms", hostname, gai,
                 errno, (long long)dt_ms);
        /* Control: is ALL DNS broken, or only this host? */
        res = NULL;
        t0 = esp_timer_get_time();
        gai = getaddrinfo("dns.alidns.com", "443", &hints, &res);
        dt_ms = (esp_timer_get_time() - t0) / 1000;
        if (gai != 0) {
            ESP_LOGE(TAG, "dns_probe: control getaddrinfo(dns.alidns.com) also failed gai=%d after %lld ms",
                     gai, (long long)dt_ms);
        } else {
            char addr_str[64] = {0};
            inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, addr_str, sizeof(addr_str));
            ESP_LOGW(TAG, "dns_probe: control dns.alidns.com ok -> %s (%lld ms); host-specific fail",
                     addr_str, (long long)dt_ms);
            freeaddrinfo(res);
        }
        return;
    }

    char addr_str[64] = {0};
    inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "dns_probe: getaddrinfo(%s) ok -> %s (%lld ms)", hostname, addr_str,
             (long long)dt_ms);
    freeaddrinfo(res);
}

esp_err_t minihub_net_get_wifi_sta(char *ssid, size_t ssid_len, bool *has_password)
{
    if (!ssid || ssid_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    if (has_password) {
        *has_password = false;
    }
#if !CONFIG_MINIHUB_NET_WIFI_STA
    return ESP_ERR_NOT_SUPPORTED;
#else
    wifi_config_t cfg = {0};
    esp_err_t err = minihub_provision_load_credentials(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    strncpy(ssid, (const char *)cfg.sta.ssid, ssid_len - 1);
    ssid[ssid_len - 1] = '\0';
    if (has_password) {
        *has_password = cfg.sta.password[0] != '\0';
    }
    return ESP_OK;
#endif
}

esp_err_t minihub_net_set_wifi_sta(const char *ssid, const char *password, bool open_network)
{
#if !CONFIG_MINIHUB_NET_WIFI_STA
    (void)ssid;
    (void)password;
    (void)open_network;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Empty SSID => disable Wi-Fi STA: clear saved credentials, disconnect any
     * current uplink, and keep the interface disconnected (no auto-reconnect).
     * This lets the hub drop Wi-Fi (e.g. to rely on Ethernet) and stay that way
     * across reboots. */
    if (ssid[0] == '\0') {
        s_wifi_sta_disabled = true;
        esp_err_t cerr = minihub_provision_clear_credentials();
        if (cerr != ESP_OK) {
            ESP_LOGW(TAG, "clear Wi-Fi credentials: %s", esp_err_to_name(cerr));
        }
        wifi_mode_t mode = WIFI_MODE_NULL;
        if (esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT && !minihub_provision_busy()) {
            (void)esp_wifi_disconnect();
        }
        ESP_LOGI(TAG, "Wi-Fi STA disabled (empty SSID)");
        return ESP_OK;
    }

    s_wifi_sta_disabled = false;

    char pass_buf[sizeof(((wifi_config_t *)0)->sta.password)];
    pass_buf[0] = '\0';
    if (!open_network) {
        if (password && password[0] != '\0') {
            strncpy(pass_buf, password, sizeof(pass_buf) - 1);
            pass_buf[sizeof(pass_buf) - 1] = '\0';
        } else {
            wifi_config_t cur = {0};
            if (minihub_provision_load_credentials(&cur) == ESP_OK) {
                strncpy(pass_buf, (const char *)cur.sta.password, sizeof(pass_buf) - 1);
                pass_buf[sizeof(pass_buf) - 1] = '\0';
            }
        }
    }

    esp_err_t err = minihub_provision_save_credentials(ssid, pass_buf);
    if (err != ESP_OK) {
        return err;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT || minihub_provision_busy()) {
        ESP_LOGI(TAG, "Wi-Fi STA saved (ssid=%s); applied on next start", ssid);
        return ESP_OK;
    }

    /* When the device booted into the SoftAP provisioning transport with no
     * credentials it is in WIFI_MODE_AP, so the STA interface is NOT enabled and
     * esp_wifi_connect() below would silently fail ("设置 wifi 后连接失败").
     * Enable STA alongside the AP (APSTA) so the uplink can connect while the
     * config web console on the AP stays reachable; on the next reboot with
     * credentials stored start_wifi_sta() switches back to WIFI_MODE_STA. */
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_NULL) {
        esp_err_t merr = esp_wifi_set_mode(mode == WIFI_MODE_NULL ? WIFI_MODE_STA : WIFI_MODE_APSTA);
        if (merr != ESP_OK) {
            ESP_LOGE(TAG, "failed to enable STA mode: %s", esp_err_to_name(merr));
        }
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass_buf, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass_buf[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    if (!s_wifi_idle_eg) {
        s_wifi_idle_eg = xEventGroupCreate();
    }
    s_wifi_hold_reconnect = true;
    if (s_wifi_idle_eg) {
        xEventGroupClearBits(s_wifi_idle_eg, WIFI_STA_IDLE_BIT);
    }
    esp_err_t derr = esp_wifi_disconnect();
    if (derr == ESP_OK && s_wifi_idle_eg) {
        (void)xEventGroupWaitBits(s_wifi_idle_eg, WIFI_STA_IDLE_BIT, pdTRUE, pdTRUE,
                                  pdMS_TO_TICKS(2000));
    }

    err = ESP_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_ERR_WIFI_STATE) {
            break;
        }
        if (s_wifi_idle_eg) {
            xEventGroupClearBits(s_wifi_idle_eg, WIFI_STA_IDLE_BIT);
        }
        (void)esp_wifi_disconnect();
        if (s_wifi_idle_eg) {
            (void)xEventGroupWaitBits(s_wifi_idle_eg, WIFI_STA_IDLE_BIT, pdTRUE, pdTRUE,
                                      pdMS_TO_TICKS(500));
        }
    }
    s_wifi_hold_reconnect = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi set_config: %s (saved for next start)", esp_err_to_name(err));
        (void)esp_wifi_connect();
        return ESP_OK;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA reconnecting to %s", ssid);
    }
    return ESP_OK;
#endif
}

static bool mac_is_blank(const uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) {
            return false;
        }
    }
    return true;
}

esp_err_t minihub_net_format_mac(char *out, size_t out_len)
{
    if (!out || out_len < 18) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK || mac_is_blank(mac)) {
        err = esp_read_mac(mac, ESP_MAC_BASE);
    }
    if (err != ESP_OK || mac_is_blank(mac)) {
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    int n = snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t minihub_net_copy_url_with_mac(const char *url, char *out, size_t out_len)
{
    if (!url || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char mac[18];
    if (minihub_net_format_mac(mac, sizeof(mac)) != ESP_OK) {
        size_t n = strlen(url);
        if (n + 1 > out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, url, n + 1);
        return ESP_OK;
    }
    const char *query = strchr(url, '?');
    if (query && strstr(query, "mac=")) {
        size_t n = strlen(url);
        if (n + 1 > out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, url, n + 1);
        return ESP_OK;
    }
    int n = snprintf(out, out_len, "%s%smac=%s", url, query ? "&" : "?", mac);
    if (n < 0 || (size_t)n >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void lowercase_copy(char *out, size_t out_len, const char *in)
{
    size_t i = 0;
    if (!out || out_len == 0) {
        return;
    }
    for (; in && in[i] && i + 1 < out_len; i++) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

static const char *board_name(void)
{
#ifdef CONFIG_MINIHUB_BOARD
    if (CONFIG_MINIHUB_BOARD[0]) {
        return CONFIG_MINIHUB_BOARD;
    }
#endif
    return NULL;
}

void minihub_net_append_chip_board_query(char *url, size_t url_len)
{
    if (!url || !url[0] || url_len == 0) {
        return;
    }
    char chip[32];
    lowercase_copy(chip, sizeof(chip), CONFIG_IDF_TARGET);
    size_t used = strnlen(url, url_len);
    if (used >= url_len) {
        return;
    }
    const char *sep = strchr(url, '?') ? "&" : "?";
    int n = snprintf(url + used, url_len - used, "%schip=%s", sep, chip);
    if (n < 0 || (size_t)n >= url_len - used) {
        url[used] = '\0';
        return;
    }
    const char *board = board_name();
    if (!board) {
        return;
    }
    used = strnlen(url, url_len);
    if (used >= url_len) {
        return;
    }
    sep = strchr(url, '?') ? "&" : "?";
    n = snprintf(url + used, url_len - used, "%sboard=%s", sep, board);
    if (n < 0 || (size_t)n >= url_len - used) {
        url[used] = '\0';
    }
}

static char *append_query(char *url, const char *key, const char *value)
{
    if (!url || !key || !key[0] || !value || !value[0]) {
        return url;
    }
    char *next = NULL;
    int n = asprintf(&next, "%s%s%s=%s", url, strchr(url, '?') ? "&" : "?", key, value);
    free(url);
    if (n < 0 || !next) {
        return NULL;
    }
    return next;
}

char *minihub_net_url_with_device_queries(const char *url, const char *cur_version)
{
    if (!url) {
        return NULL;
    }
    /* Start from a mac-augmented copy when the buffer fits; otherwise plain. */
    char with_mac[768];
    const char *base = url;
    if (minihub_net_copy_url_with_mac(url, with_mac, sizeof(with_mac)) == ESP_OK) {
        base = with_mac;
    }
    size_t base_len = strlen(base);
    char *full = malloc(base_len + 1);
    if (!full) {
        return NULL;
    }
    memcpy(full, base, base_len + 1);
    char chip[32];
    lowercase_copy(chip, sizeof(chip), CONFIG_IDF_TARGET);
    full = append_query(full, "chip", chip);
    if (!full) {
        return NULL;
    }
    const char *board = board_name();
    if (board) {
        full = append_query(full, "board", board);
        if (!full) {
            return NULL;
        }
    }
    if (cur_version && cur_version[0]) {
        full = append_query(full, "cur_version", cur_version);
        if (!full) {
            return NULL;
        }
    }
    return full;
}
