#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "settings.h"
#include "wifi.h"
#include "web.h"
#include "telegram.h"

static const char *TAG = "telegram";

#define QUEUE_LEN     12
#define MIN_GAP_MS    3000
#define MAX_TRIES     4

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mutex;
static char s_status[96] = "nothing sent yet";

static void set_status(const char *s) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_status, s, sizeof(s_status));
    xSemaphoreGive(s_mutex);
}

void telegram_status(char *out, size_t n) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(out, s_status, n);
    xSemaphoreGive(s_mutex);
}

bool telegram_enabled(void) {
    settings_t cfg;
    settings_copy(&cfg);
    return cfg.tg_enable && cfg.tg_token[0] && cfg.tg_chat[0];
}

static bool enqueue(const char *text, bool force) {
    if (!s_q || (!force && !telegram_enabled())) return false;
    char *m = strdup(text);
    if (!m) return false;
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, message dropped");
        free(m);
        return false;
    }
    return true;
}

bool telegram_send(const char *text) { return enqueue(text, false); }

bool telegram_sendf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return telegram_send(buf);
}

typedef struct { char body[256]; int len; } resp_t;

static esp_err_t http_event(esp_http_client_event_t *evt) {
    resp_t *r = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        int n = evt->data_len;
        if (n > (int)sizeof(r->body) - 1 - r->len) n = sizeof(r->body) - 1 - r->len;
        if (n > 0) { memcpy(r->body + r->len, evt->data, n); r->len += n; r->body[r->len] = 0; }
    }
    return ESP_OK;
}

/* Returns 0 on success, a retry delay in ms for transient errors, or -1 to drop. */
static int post(const settings_t *cfg, const char *text) {
    char url[160];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", cfg->tg_token);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "chat_id", cfg->tg_chat);
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddBoolToObject(o, "disable_web_page_preview", true);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!body) return 5000;

    resp_t r = {0};
    esp_http_client_config_t hc = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event, .user_data = &r,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) { cJSON_free(body); return 5000; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body, strlen(body));
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    cJSON_free(body);

    char st[96];
    /* A 401 also makes perform() fail (ESP_ERR_NOT_SUPPORTED, it expects an
     * auth challenge), so any HTTP status wins over `err`. */
    if (status <= 0) {
        /* never log the URL: it contains the bot token */
        snprintf(st, sizeof(st), "network error (%s)", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", st);
        set_status(st);
        return 10000;
    }
    if (status == 200) { set_status("last message sent OK"); return 0; }

    cJSON *j = cJSON_Parse(r.body);
    const cJSON *d = j ? cJSON_GetObjectItem(j, "description") : NULL;
    snprintf(st, sizeof(st), "HTTP %d: %s", status, cJSON_IsString(d) ? d->valuestring : "error");
    ESP_LOGW(TAG, "%s", st);
    set_status(st);
    int retry = -1;
    if (status == 429) {
        const cJSON *p = j ? cJSON_GetObjectItem(j, "parameters") : NULL;
        const cJSON *ra = p ? cJSON_GetObjectItem(p, "retry_after") : NULL;
        retry = cJSON_IsNumber(ra) ? ra->valueint * 1000 + 500 : 30000;
    } else if (status >= 500) {
        retry = 15000;
    }                                   /* 400/401/403/404: bad token or chat, drop */
    cJSON_Delete(j);
    return retry;
}

static void tg_task(void *arg) {
    while (1) {
        char *m;
        if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) continue;
        for (int tries = 0; tries < MAX_TRIES; tries++) {
            while (!wifi_is_connected()) vTaskDelay(pdMS_TO_TICKS(2000));
            settings_t cfg;
            settings_copy(&cfg);
            if (!cfg.tg_token[0] || !cfg.tg_chat[0]) break;
            int r = post(&cfg, m);
            if (r <= 0) break;
            vTaskDelay(pdMS_TO_TICKS(r));
        }
        free(m);
        vTaskDelay(pdMS_TO_TICKS(MIN_GAP_MS));
    }
}

static esp_err_t test_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    char ctype[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    cJSON *o = cJSON_CreateObject();
    settings_t cfg;
    settings_copy(&cfg);
    if (strncmp(ctype, "application/json", 16) != 0) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "Content-Type must be application/json");
    } else if (!cfg.tg_token[0] || !cfg.tg_chat[0]) {
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", "Save a bot token and chat ID first");
    } else {
        bool ok = enqueue("Growatt vEM: test message. Telegram works.", true);
        cJSON_AddBoolToObject(o, "ok", ok);
        if (!ok) cJSON_AddStringToObject(o, "error", "Queue full");
    }
    return web_send_json(req, o);
}

static esp_err_t status_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    char st[96];
    telegram_status(st, sizeof(st));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", telegram_enabled());
    cJSON_AddStringToObject(o, "status", st);
    cJSON_AddNumberToObject(o, "queued", s_q ? uxQueueMessagesWaiting(s_q) : 0);
    return web_send_json(req, o);
}

void telegram_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_q = xQueueCreate(QUEUE_LEN, sizeof(char *));
    xTaskCreate(tg_task, "telegram", 8192, NULL, 3, NULL);
    static const httpd_uri_t t = { .uri = "/api/telegram/test", .method = HTTP_POST, .handler = test_handler };
    static const httpd_uri_t s = { .uri = "/api/telegram", .method = HTTP_GET, .handler = status_handler };
    web_register(&t);
    web_register(&s);
}
