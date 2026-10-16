#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "settings.h"
#include "wifi.h"
#include "shelly.h"
#include "web.h"
#include "rs485.h"

static const char *TAG = "app";

/* Demo: send a Modbus read request over RS485 once a second. */
static void comm_demo_task(void *arg) {
    static const uint8_t modbus_read[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A };
    while (1) {
        rs485_send((const char *)modbus_read, sizeof(modbus_read));
        ESP_LOGI(TAG, "RS485: sent Modbus request (%d bytes)", (int)sizeof(modbus_read));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "starting...");

    settings_init();
    rs485_init();
    wifi_init_sta();
    web_start();
    shelly_init();

    xTaskCreate(comm_demo_task, "comm_demo", 4096, NULL, 5, NULL);
}
