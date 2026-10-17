#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "driver/uart.h"
#include "settings.h"
#include "vem.h"
#include "web.h"
#include "modbus.h"

static const char *TAG = "modbus";

/* Waveshare ESP32-S3-RS485-CAN */
#define RS485_TX_PIN   17
#define RS485_RX_PIN   18
#define RS485_EN_PIN   21   /* DE/RE, driven as the UART's RTS pin */
#define RS485_UART     UART_NUM_1
#define FRAME_MAX      256
#define LOG_N          24

static const int BAUDS[] = { 2400, 4800, 9600, 19200 };

typedef struct {
    int64_t  t_us;
    uint8_t  len;
    uint8_t  data[24];       /* first bytes of the frame */
    uint8_t  result;         /* mb_result_t */
    mb_req_info_t info;
    bool     answered;
} log_entry_t;

static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_uart_q;
static volatile bool s_reconfig;
static modbus_stats_t s_stats;
static log_entry_t s_log[LOG_N];
static int s_log_head;       /* next slot */
static int s_log_count;

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

void modbus_get_stats(modbus_stats_t *out) { lock(); *out = s_stats; unlock(); }

int64_t modbus_last_request_age_ms(void) {
    lock();
    int64_t t = s_stats.last_request_us;
    unlock();
    return t ? (esp_timer_get_time() - t) / 1000 : -1;
}

static void log_frame(const uint8_t *f, size_t len, mb_result_t r, const mb_req_info_t *info, bool answered) {
    log_entry_t *e = &s_log[s_log_head];
    e->t_us = esp_timer_get_time();
    e->len = len > 255 ? 255 : (uint8_t)len;
    memcpy(e->data, f, len < sizeof(e->data) ? len : sizeof(e->data));
    e->result = (uint8_t)r;
    e->info = *info;
    e->answered = answered;
    s_log_head = (s_log_head + 1) % LOG_N;
    if (s_log_count < LOG_N) s_log_count++;
}

static void apply_uart_config(const settings_t *cfg) {
    int baud = BAUDS[cfg->mb_baud < 4 ? cfg->mb_baud : 2];
    uart_set_baudrate(RS485_UART, baud);
    ESP_LOGI(TAG, "mode %d, %s at address %d, %d baud", (int)cfg->mb_mode,
             mb_meter_name((mb_meter_t)cfg->mb_meter), (int)cfg->mb_addr, baud);
}

static void handle_frame(const uint8_t *f, size_t len, const settings_t *cfg) {
    static uint8_t resp[FRAME_MAX];
    mb_req_info_t info;
    size_t resp_len = 0;
    bool answered = false;

    vem_t vem;
    vem_compute(&vem);
    mb_result_t r = mb_process(f, len, (uint8_t)cfg->mb_addr, (mb_meter_t)cfg->mb_meter,
                               &vem.mb, resp, &resp_len, &info);

    if (r == MB_R_REPLY && cfg->mb_mode == MODBUS_MODE_EMULATE && vem.lim.answer) {
        uart_write_bytes(RS485_UART, resp, resp_len);
        uart_wait_tx_done(RS485_UART, pdMS_TO_TICKS(200));
        answered = true;
    }

    lock();
    switch (r) {
    case MB_R_REPLY:
    case MB_R_BROADCAST:
        s_stats.requests++;
        s_stats.last_request_us = esp_timer_get_time();
        s_stats.last_fc = info.fc;
        s_stats.last_start = info.start;
        s_stats.last_count = info.count;
        if (info.unmapped) s_stats.unmapped++;
        if (info.exception) s_stats.exceptions++;
        if (answered) s_stats.replies++;
        break;
    case MB_R_NOT_FOR_US: s_stats.other_addr++; break;
    case MB_R_BAD_CRC:    s_stats.crc_errors++; break;
    case MB_R_BAD_FRAME:  s_stats.bad_frames++; break;
    }
    log_frame(f, len, r, &info, answered);
    unlock();

    if (cfg->mb_mode == MODBUS_MODE_SNIFF || r == MB_R_BAD_CRC || info.unmapped) {
        char hex[3 * 24 + 1];
        size_t n = len < 24 ? len : 24;
        for (size_t i = 0; i < n; i++) snprintf(hex + 3 * i, 4, "%02X ", f[i]);
        ESP_LOGI(TAG, "rx %d B [%s] res=%d fc=%02X start=0x%04X n=%u%s", (int)len, hex, (int)r,
                 info.fc, info.start, info.count, info.unmapped ? " (unmapped regs)" : "");
    }
}

static void modbus_task(void *arg) {
    uint8_t frame[FRAME_MAX];
    size_t len = 0;
    settings_t cfg;
    settings_copy(&cfg);
    apply_uart_config(&cfg);
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        if (s_reconfig) {
            s_reconfig = false;
            settings_copy(&cfg);
            apply_uart_config(&cfg);
            uart_flush_input(RS485_UART);
            len = 0;
        }

        uart_event_t ev;
        if (xQueueReceive(s_uart_q, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) continue;

        switch (ev.type) {
        case UART_DATA: {
            size_t room = sizeof(frame) - len;
            size_t want = ev.size < room ? ev.size : room;
            int n = uart_read_bytes(RS485_UART, frame + len, want, 0);
            if (n > 0) len += n;
            if (ev.size > room) {           /* overlong: drop it */
                uart_flush_input(RS485_UART);
                len = 0;
                lock(); s_stats.bad_frames++; unlock();
                break;
            }
            /* The RX timeout (3 character times of silence) ends a frame. */
            if (ev.timeout_flag && len > 0) {
                settings_copy(&cfg);        /* pick up live limiter/address changes */
                if (cfg.mb_mode != MODBUS_MODE_OFF) handle_frame(frame, len, &cfg);
                len = 0;
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            uart_flush_input(RS485_UART);
            xQueueReset(s_uart_q);
            len = 0;
            lock(); s_stats.bad_frames++; unlock();
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            lock(); s_stats.line_errors++; unlock();
            break;
        default:
            break;
        }
    }
}

static void on_settings_changed(void) { s_reconfig = true; }

/* ---- web API ---- */

static esp_err_t modbus_api_handler(httpd_req_t *req) {
    modbus_stats_t st;
    log_entry_t log[LOG_N];
    int count, head;
    lock();
    st = s_stats;
    memcpy(log, s_log, sizeof(log));
    count = s_log_count;
    head = s_log_head;
    unlock();

    int64_t now = esp_timer_get_time();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "requests", st.requests);
    cJSON_AddNumberToObject(o, "replies", st.replies);
    cJSON_AddNumberToObject(o, "crc_errors", st.crc_errors);
    cJSON_AddNumberToObject(o, "bad_frames", st.bad_frames);
    cJSON_AddNumberToObject(o, "line_errors", st.line_errors);
    cJSON_AddNumberToObject(o, "other_addr", st.other_addr);
    cJSON_AddNumberToObject(o, "unmapped", st.unmapped);
    cJSON_AddNumberToObject(o, "exceptions", st.exceptions);
    cJSON_AddNumberToObject(o, "last_request_age_ms",
                            st.last_request_us ? (double)((now - st.last_request_us) / 1000) : -1);
    cJSON *arr = cJSON_AddArrayToObject(o, "log");
    for (int i = 0; i < count; i++) {
        const log_entry_t *e = &log[(head - 1 - i + LOG_N) % LOG_N];   /* newest first */
        char hex[3 * 24 + 1] = "";
        size_t n = e->len < 24 ? e->len : 24;
        for (size_t k = 0; k < n; k++) snprintf(hex + 3 * k, 4, "%02X ", e->data[k]);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "age_ms", (double)((now - e->t_us) / 1000));
        cJSON_AddNumberToObject(j, "len", e->len);
        cJSON_AddStringToObject(j, "hex", hex);
        static const char *RES[] = { "ok", "other address", "broadcast", "bad CRC", "bad frame" };
        cJSON_AddStringToObject(j, "result", e->result < 5 ? RES[e->result] : "?");
        cJSON_AddNumberToObject(j, "addr", e->info.addr);
        cJSON_AddNumberToObject(j, "fc", e->info.fc);
        cJSON_AddNumberToObject(j, "start", e->info.start);
        cJSON_AddNumberToObject(j, "count", e->info.count);
        cJSON_AddBoolToObject(j, "unmapped", e->info.unmapped);
        cJSON_AddNumberToObject(j, "exception", e->info.exception);
        cJSON_AddBoolToObject(j, "answered", e->answered);
        cJSON_AddItemToArray(arr, j);
    }
    return web_send_json(req, o);
}

void modbus_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    const uart_config_t uart_cfg = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART, 1024, 1024, 20, &s_uart_q, 0));
    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &uart_cfg));
    /* In half-duplex mode the UART drives RTS as DE/RE. */
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, RS485_TX_PIN, RS485_RX_PIN,
                                 RS485_EN_PIN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(RS485_UART, UART_MODE_RS485_HALF_DUPLEX));
    /* RX timeout in character times: marks the end of a Modbus RTU frame. */
    ESP_ERROR_CHECK(uart_set_rx_timeout(RS485_UART, 3));

    settings_subscribe(APPLY_MODBUS, on_settings_changed);
    xTaskCreate(modbus_task, "modbus", 4096, NULL, 10, NULL);

    static const httpd_uri_t api = { .uri = "/api/modbus", .method = HTTP_GET, .handler = modbus_api_handler };
    web_register(&api);
}
