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
#include "esp_eth_phy.h"
#include "esp_mac.h"
#endif

static const char *TAG = "minihub_net";

/* SoftAP SSID presented for web-based Wi-Fi configuration when no credentials
 * are stored. Matches the legacy network_prov SoftAP name so UX stays familiar. */
#define MINIHUB_WEB_PROV_AP_SSID "PROV_MiniHub"

static EventGroupHandle_t s_net_eg;
static bool s_started;
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
            ESP_LOGW(TAG, "%s dns[%s]=(empty)", ifname ? ifname : "netif", dns_name[i]);
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
        /* During BLE/SoftAP provisioning the manager owns connect; auto-connect
         * here races set_config and yields ESP_ERR_WIFI_STATE. */
        if (!minihub_provision_busy()) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        refresh_default_netif();
        if (s_wifi_hold_reconnect) {
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
    /* CHIP ENABLE_WIFI_STATION is off (Matter over Thread/Eth); MiniHub still
     * brings up STA for host IP. Default route follows GOT_IP (ETH, else Wi-Fi). */
    ESP_RETURN_ON_ERROR(ensure_wifi_driver_init(), TAG, "wifi init");

    wifi_config_t wifi_config = {0};
    esp_err_t err = minihub_provision_load_credentials(&wifi_config);
    if (err == ESP_OK) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi set_ps(NONE) failed: %s", esp_err_to_name(err));
        }
        if (!minihub_provision_busy()) {
            esp_err_t cerr = esp_wifi_connect();
            if (cerr != ESP_OK && cerr != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(cerr));
            }
        }
        return ESP_OK;
    }

#if CONFIG_MINIHUB_WIFI_PROVISIONING
#if CONFIG_MINIHUB_WIFI_PROV_TRANSPORT_SOFTAP
    /* network_prov's SoftAP HTTP server binds port 80, colliding with the
     * MiniHub web console (also port 80) -> web unreachable after a factory
     * reset. Wi-Fi is instead configured through the web UI (POST /api/wifi/sta
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
#else /* BLE transport: no port-80 conflict, keep network_prov */
    err = minihub_provision_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi provisioning failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi BLE provisioning started");
    return ESP_OK;
#endif
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

static esp_err_t start_eth(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_MINIHUB_NET_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_MINIHUB_NET_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "MAC create failed");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "PHY create failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "eth install");

#if CONFIG_MINIHUB_NET_ETH_YT8531_INIT
    ESP_RETURN_ON_ERROR(eth_phy_yt8531_specific_init(s_eth_handle), TAG, "YT8531 init");
#endif

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_eth_netif, ESP_ERR_NO_MEM, TAG, "eth netif");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue), TAG, "netif attach");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL),
                        TAG, "eth event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_got_ip_handler, NULL),
                        TAG, "eth got ip");

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start");
    return ESP_OK;
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
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return err;
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
             1
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
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

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
