#include "esp_log.h"
#include "driver/uart.h"
#include "rs485.h"

static const char *TAG = "rs485";

#define RS485_TX_PIN   17
#define RS485_RX_PIN   18
#define RS485_EN_PIN   21   /* DE/RE, driven as the UART's RTS pin */
#define RS485_UART     UART_NUM_1
#define RS485_BAUD     9600

void rs485_init(void) {
    const uart_config_t uart_cfg = {
        .baud_rate  = RS485_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &uart_cfg));
    /* In half-duplex mode the UART drives RTS as DE/RE. */
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, RS485_TX_PIN, RS485_RX_PIN,
                                 RS485_EN_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(RS485_UART, UART_MODE_RS485_HALF_DUPLEX));
    ESP_LOGI(TAG, "initialized (UART%d, %d baud, EN=GPIO%d)", RS485_UART, RS485_BAUD, RS485_EN_PIN);
}

void rs485_send(const char *buf, size_t len) {
    uart_write_bytes(RS485_UART, buf, len);
}
