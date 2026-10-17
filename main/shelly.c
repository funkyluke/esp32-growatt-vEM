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

/* After this many failed polls in a row the address is resolved again
 * (the Shelly may have a new DHCP lease). */
#define MAX_FAILS_BEFORE_RERESOLVE  5
/* While running on the fallback IP, retry mDNS this often. */
#define MDNS_RETRY_US  (5LL * 60 * 1000000)

static SemaphoreHandle_t s_mutex;      /* guards s_snap and the diag fields */
static shelly_snapshot_t s_snap;
static int  s_diag_status;
static char s_diag_url[96];

static char s_ip[16];                  /* poll task only */
static bool s_ip_is_fallback;          /* poll task only */
static int64_t s_resolved_us;          /* poll task only */
static volatile bool s_reresolve;

#define RX_MAX 4096
static char s_rx[RX_MAX];              /* poll task only */

typedef struct {
    int  len;
    bool truncated;
} rx_ctx_t;

static void lock(void)   { xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mutex); }

int shelly_diag(char *url, size_t n) {
    lock();
    int st = s_diag_status;
    if (url) strlcpy(url, s_diag_url, n);
    unlock();
    return st;
}

static void diag_set(int status, const char *url) {
    lock();
    s_diag_status = status;
    strlcpy(s_diag_url, url, sizeof(s_diag_url));
    unlock();
}

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
    rx_ctx_t *rx = (rx_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int n = evt->data_len;
        if (n > RX_MAX - 1 - rx->len) { n = RX_MAX - 1 - rx->len; rx->truncated = true; }
        if (n > 0) { memcpy(s_rx + rx->len, evt->data, n); rx->len += n; }
    }
    return ESP_OK;
}

/* Reads a required number; false if it is missing (never report a made-up 0). */
static bool get_num(const cJSON *o, const char *key, float *out) {
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsNumber(it)) return false;
    *out = (float)it->valuedouble;
    return true;
}

static bool fetch_status(shelly_snapshot_t *snap) {
    char url[64];
    snprintf(url, sizeof(url), "http://%s/status", s_ip);
    rx_ctx_t rx = {0};

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_GET, .timeout_ms = 5000,
        .event_handler = http_event, .user_data = &rx, .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || rx.len == 0) {
        diag_set((status > 0) ? status : (int)err, url);
        ESP_LOGW(TAG, "GET %s -> err=0x%x status=%d len=%d", url, (unsigned)err, status, rx.len);
        return false;
    }
    if (rx.truncated) {
        diag_set(-2, url);
        ESP_LOGW(TAG, "/status is larger than %d bytes, response truncated", RX_MAX - 1);
        return false;
    }
    s_rx[rx.len] = '\0';

    cJSON *root = cJSON_ParseWithLength(s_rx, rx.len);
    if (!root) { diag_set(-3, url); ESP_LOGW(TAG, "JSON parse failed"); return false; }

    bool ok = false;
    cJSON *em = cJSON_GetObjectItem(root, "emeters");
    if (cJSON_IsArray(em) && cJSON_GetArraySize(em) >= 3) {
        ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            cJSON *e = cJSON_GetArrayItem(em, i);
            shelly_phase_t *ph = &snap->phase[i];
            ok = get_num(e, "power", &ph->power) && get_num(e, "current", &ph->current)
              && get_num(e, "voltage", &ph->voltage);
            /* Energy counters are optional (Wh). */
            float v;
            ph->total_wh          = get_num(e, "total", &v) ? v : 0;
            ph->total_returned_wh = get_num(e, "total_returned", &v) ? v : 0;
        }
        if (!ok) ESP_LOGW(TAG, "emeters[] entry without power/current/voltage");
    } else {
        ESP_LOGW(TAG, "no emeters[3] in /status (is this a 3EM Gen1?)");
    }
    cJSON_Delete(root);
    if (!ok) diag_set(-4, url);
    return ok;
}

static void forget_address(void) {
    s_ip[0] = '\0';
    s_ip_is_fallback = false;
}

static void poll_task(void *arg) {
    int fails = 0;
    while (1) {
        settings_t cfg;
        settings_copy(&cfg);

        if (s_reresolve) {
            s_reresolve = false;
            forget_address();
            fails = 0;
            lock();
            memset(&s_snap, 0, sizeof(s_snap));
            s_diag_status = 0;
            s_diag_url[0] = '\0';
            unlock();
        }

        /* On the fallback IP, keep trying mDNS now and then. */
        if (s_ip_is_fallback && esp_timer_get_time() - s_resolved_us > MDNS_RETRY_US) {
            if (resolve_ip(cfg.shelly_id)) s_ip_is_fallback = false;
            s_resolved_us = esp_timer_get_time();
        }

        if (s_ip[0] == '\0') {
            if (!resolve_ip(cfg.shelly_id)) {
                if (settings_ip_is_set(cfg.shelly_ip)) {
                    strlcpy(s_ip, cfg.shelly_ip, sizeof(s_ip));
                    s_ip_is_fallback = true;
                    ESP_LOGW(TAG, "using fallback IP %s", s_ip);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    continue;
                }
            }
            s_resolved_us = esp_timer_get_time();
        }

        shelly_snapshot_t snap = {0};
        bool ok = fetch_status(&snap);
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(300));
            ok = fetch_status(&snap);
        }

        if (ok) {
            fails = 0;
            snap.total_power = snap.phase[0].power + snap.phase[1].power + snap.phase[2].power;
            snap.valid = true;
            snap.last_ok_us = esp_timer_get_time();
            ESP_LOGD(TAG, "L1=%.0fW L2=%.0fW L3=%.0fW total=%.0fW",
                     snap.phase[0].power, snap.phase[1].power, snap.phase[2].power, snap.total_power);
            lock();
            s_snap = snap;
            unlock();
        } else {
            lock();
            s_snap.valid = false;
            unlock();
            if (++fails >= MAX_FAILS_BEFORE_RERESOLVE) {
                ESP_LOGW(TAG, "%d failed polls in a row, resolving the address again", fails);
                forget_address();
                fails = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(cfg.shelly_poll_s * 1000));
    }
}

void shelly_get_snapshot(shelly_snapshot_t *out) {
    lock();
    *out = s_snap;
    unlock();
}

shelly_state_t shelly_state(const shelly_snapshot_t *s, int64_t *age_ms) {
    settings_t cfg;
    settings_copy(&cfg);
    int64_t fresh_ms = cfg.shelly_poll_s * 2500LL, aging_ms = cfg.shelly_poll_s * 7500LL;
    if (fresh_ms < 5000) fresh_ms = 5000;
    if (aging_ms < 15000) aging_ms = 15000;
    int64_t age = (s->last_ok_us > 0) ? (esp_timer_get_time() - s->last_ok_us) / 1000 : -1;
    if (age_ms) *age_ms = age;
    return (age < 0) ? SHELLY_NONE : (age < fresh_ms) ? SHELLY_FRESH
         : (age < aging_ms) ? SHELLY_AGING : SHELLY_STALE;
}

const char *shelly_state_name(shelly_state_t st) {
    static const char *names[] = { "none", "fresh", "aging", "stale" };
    return names[st];
}

void shelly_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    settings_subscribe(APPLY_SHELLY, shelly_reresolve);
    ESP_ERROR_CHECK(mdns_init());
    xTaskCreate(poll_task, "shelly_poll", 4096, NULL, 5, NULL);
}
