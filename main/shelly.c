#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "mdns.h"
#include "cJSON.h"
#include "settings.h"
#include "shelly.h"

static const char *TAG = "shelly";

static shelly_snapshot_t s_snap;
static SemaphoreHandle_t s_mutex;
static char s_ip[16];
static volatile bool s_reresolve;
static int  s_diag_status;
static char s_diag_url[96];

#define RX_MAX 4096
static char s_rx[RX_MAX];

int shelly_diag_status(void) { return s_diag_status; }
const char *shelly_diag_url(void) { return s_diag_url; }

void shelly_reresolve(void) { s_reresolve = true; }

static bool resolve_ip(const char *device_id) {
    static const char *prefixes[] = { "shellyem3-", "shelly3em-" };
    for (int i = 0; i < 2; i++) {
        char name[48];
        snprintf(name, sizeof(name), "%s%s", prefixes[i], device_id);
        esp_ip4_addr_t addr;
        if (mdns_query_a(name, 2000, &addr) == ESP_OK) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "resolved %s -> %s", name, s_ip);
            return true;
        }
        ESP_LOGW(TAG, "mDNS lookup failed for %s", name);
    }
    return false;
}

static esp_err_t http_event(esp_http_client_event_t *evt) {
    int *len = (int *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int n = evt->data_len;
        if (n > RX_MAX - 1 - *len) n = RX_MAX - 1 - *len;
        if (n > 0) { memcpy(s_rx + *len, evt->data, n); *len += n; }
    }
    return ESP_OK;
}

static bool fetch_status(shelly_snapshot_t *snap) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/status", s_ip);
    int len = 0;
    s_rx[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 5000,
        .event_handler = http_event, .user_data = &len, .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || len == 0) {
        s_diag_status = (status > 0) ? status : (int)err;
        strlcpy(s_diag_url, url, sizeof(s_diag_url));
        ESP_LOGW(TAG, "GET %s -> err=0x%x status=%d len=%d", url, (unsigned)err, status, len);
        return false;
    }
    s_rx[len] = '\0';

    cJSON *root = cJSON_Parse(s_rx);
    if (!root) { ESP_LOGW(TAG, "JSON parse failed"); return false; }

    bool ok = false;
    cJSON *em = cJSON_GetObjectItem(root, "emeters");
    if (cJSON_IsArray(em) && cJSON_GetArraySize(em) >= 3) {
        for (int i = 0; i < 3; i++) {
            cJSON *e = cJSON_GetArrayItem(em, i);
            cJSON *p = cJSON_GetObjectItem(e, "power");
            cJSON *c = cJSON_GetObjectItem(e, "current");
            cJSON *v = cJSON_GetObjectItem(e, "voltage");
            snap->phase[i].power   = cJSON_IsNumber(p) ? (float)p->valuedouble : 0;
            snap->phase[i].current = cJSON_IsNumber(c) ? (float)c->valuedouble : 0;
            snap->phase[i].voltage = cJSON_IsNumber(v) ? (float)v->valuedouble : 0;
        }
        ok = true;
    } else {
        ESP_LOGW(TAG, "no emeters[3] in /status (is this a 3EM Gen1?)");
    }
    cJSON_Delete(root);
    return ok;
}

static void poll_task(void *arg) {
    while (1) {
        settings_t cfg;
        settings_copy(&cfg);

        if (s_reresolve) {
            s_reresolve = false;
            s_ip[0] = '\0';
            s_diag_status = 0;
            s_diag_url[0] = '\0';
            xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100));
            memset(&s_snap, 0, sizeof(s_snap));
            xSemaphoreGive(s_mutex);
        }

        if (s_ip[0] == '\0') {
            if (!resolve_ip(cfg.shelly_id)) {
                if (settings_ip_is_set(cfg.shelly_ip)) {
                    strlcpy(s_ip, cfg.shelly_ip, sizeof(s_ip));
                    ESP_LOGW(TAG, "using fallback IP %s", s_ip);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
        }

        shelly_snapshot_t snap = {0};
        bool ok = fetch_status(&snap);
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(300));
            ok = fetch_status(&snap);
        }

        if (ok) {
            snap.total_power = snap.phase[0].power + snap.phase[1].power + snap.phase[2].power;
            snap.valid = true;
            snap.last_ok_us = esp_timer_get_time();
            ESP_LOGI(TAG, "L1=%.0fW L2=%.0fW L3=%.0fW total=%.0fW",
                     snap.phase[0].power, snap.phase[1].power, snap.phase[2].power, snap.total_power);
            xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100));
            s_snap = snap;
            xSemaphoreGive(s_mutex);
        } else {
            xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100));
            s_snap.valid = false;
            xSemaphoreGive(s_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(cfg.shelly_poll_s * 1000));
    }
}

void shelly_get_snapshot(shelly_snapshot_t *out) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_mutex);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

void shelly_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(mdns_init());
    xTaskCreate(poll_task, "shelly_poll", 4096, NULL, 5, NULL);
}
