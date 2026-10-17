idf_component_register(
    SRCS "main.c"
         "settings.c"
         "wifi.c"
         "shelly.c"
         "web.c"
         "ota.c"
         "modbus.c"
         "mb_proto.c"
         "limiter.c"
         "vem.c"
         "history.c"
         "mqtt_ha.c"
         "clock.c"
         "telegram.c"
         "monitor.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
                   "www/settings.html"
                   "www/vendor/uplot.min.js"
                   "www/vendor/uplot.min.css"
    PRIV_REQUIRES esp_driver_uart esp_hw_support esp_wifi esp_netif esp_event nvs_flash
                  esp_http_server esp_http_client mdns cjson esp_timer
                  app_update esp_app_format mbedtls mqtt
)
