#include "esp_log.h"
#include "settings.h"
#include "wifi.h"
#include "shelly.h"
#include "web.h"
#include "ota.h"
#include "modbus.h"

static const char *TAG = "app";

void app_main(void) {
    ESP_LOGI(TAG, "starting...");

    settings_init();
    ota_init();
    wifi_init_sta();
    web_start();
    shelly_init();
    modbus_init();
}
