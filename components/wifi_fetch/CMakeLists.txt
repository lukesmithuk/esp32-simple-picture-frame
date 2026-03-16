idf_component_register(
    SRCS "wifi_fetch.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_wifi
        esp_http_client
        esp_netif
        esp_hw_support
        esp_event
        nvs_flash
        board
        json
)
