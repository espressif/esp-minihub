set(REQ esp_netif esp_event esp_wifi nvs_flash freertos minihub_provision esp_eth)
set(SRCS "minihub_net.c" "minihub_net_cfg.c")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${REQ}
)
