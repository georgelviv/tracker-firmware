idf_component_register(
    SRCS "app_main.c" "modem.c" "gnss.c" "wifi.c" "firebase.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_wifi esp_netif esp_event
                  nvs_flash esp_http_client mbedtls esp_timer
)
