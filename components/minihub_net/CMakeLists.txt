set(REQ esp_netif esp_event esp_wifi nvs_flash freertos minihub_provision esp_eth minihub_adapter)
set(SRCS "minihub_net.c" "minihub_net_cfg.c")
if(CONFIG_MINIHUB_NET_WIFI_STA)
    list(APPEND SRCS "wifi_adapter.c")
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${REQ}
)
