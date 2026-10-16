idf_component_register(
    SRCS "main.c"
         "settings.c"
         "wifi.c"
         "shelly.c"
         "web.c"
         "rs485.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_uart esp_hw_support esp_wifi esp_netif esp_event nvs_flash esp_http_server esp_http_client mdns cjson esp_timer
)
