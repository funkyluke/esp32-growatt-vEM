#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include <stdio.h>
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_evg;
#define WIFI_CONNECTED_BIT BIT0
static int s_retry_count;
static esp_ip4_addr_t s_ip;
static volatile bool s_apply_busy;
static volatile bool s_ap_on;
static esp_netif_t *s_sta_netif, *s_ap_netif;

/* Fallback access point: started when there has been no IP for this long,
 * stopped again once the station has been connected for AP_STOP_US. */
#define AP_START_US      (120LL * 1000000)
#define AP_STOP_US       (60LL * 1000000)
#define AP_RETRY_MS      30000
#define AP_PASSWORD      "growattvem"

static void wifi_fill_config(wifi_config_t *wc, const char *ssid, const char *pass, const char *bssid) {
    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->sta.ssid, ssid, sizeof(wc->sta.ssid));
    strlcpy((char *)wc->sta.password, pass, sizeof(wc->sta.password));
    wc->sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Several APs share this SSID (mesh/repeater); join the strongest. */
    wc->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    unsigned b[6];
    if (bssid && sscanf(bssid, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) wc->sta.bssid[i] = (uint8_t)b[i];
        wc->sta.bssid_set = true;
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected (reason %d), retry #%d", d->reason, s_retry_count);
        xEventGroupClearBits(s_evg, WIFI_CONNECTED_BIT);
        /* Never block: this runs in the system event loop. With the fallback
         * AP up, the AP task retries slowly instead (scans disturb the AP). */
        if (!s_ap_on) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_ip = evt->ip_info.ip;
        s_retry_count = 0;
        xEventGroupSetBits(s_evg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&s_ip));
    }
}

const esp_ip4_addr_t *wifi_ip(void) { return &s_ip; }
bool wifi_apply_busy(void) { return s_apply_busy; }
bool wifi_is_connected(void) { return (xEventGroupGetBits(s_evg) & WIFI_CONNECTED_BIT) != 0; }
bool wifi_ap_active(void) { return s_ap_on; }

static void ap_start(void) {
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    wifi_config_t ac = { 0 };
    snprintf((char *)ac.ap.ssid, sizeof(ac.ap.ssid), "GrowattVEM-%02X%02X", mac[4], mac[5]);
    ac.ap.ssid_len = strlen((char *)ac.ap.ssid);
    strlcpy((char *)ac.ap.password, AP_PASSWORD, sizeof(ac.ap.password));
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = 2;
    s_ap_on = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    ESP_LOGW(TAG, "no WiFi for %d s: fallback AP \"%s\" (password %s) at http://192.168.4.1/",
             (int)(AP_START_US / 1000000), (char *)ac.ap.ssid, AP_PASSWORD);
}

static void ap_stop(void) {
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
    ESP_LOGI(TAG, "connected again, fallback AP stopped");
}

static void ap_task(void *arg) {
    int64_t down_since = 0, up_since = 0, last_retry = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        if (wifi_is_connected()) {
            down_since = 0;
            if (!up_since) up_since = now;
            if (s_ap_on && now - up_since > AP_STOP_US) ap_stop();
        } else {
            up_since = 0;
            if (!down_since) down_since = now;
            if (!s_ap_on && now - down_since > AP_START_US) { ap_start(); last_retry = now; }
            if (s_ap_on && now - last_retry > AP_RETRY_MS * 1000LL) { last_retry = now; esp_wifi_connect(); }
        }
    }
}

typedef struct {
    settings_t old;
    uint32_t   wifi_mask;
} wifi_apply_ctx_t;

static void apply_task(void *arg) {
    wifi_apply_ctx_t *ctx = (wifi_apply_ctx_t *)arg;

    /* give the HTTP response time to leave before the link drops */
    vTaskDelay(pdMS_TO_TICKS(1000));

    settings_t cur;
    settings_copy(&cur);
    wifi_config_t wc;
    wifi_fill_config(&wc, cur.wifi_ssid, cur.wifi_pass, cur.wifi_bssid);

    ESP_LOGW(TAG, "switching to SSID \"%s\"", cur.wifi_ssid);
    xEventGroupClearBits(s_evg, WIFI_CONNECTED_BIT);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_disconnect();

    EventBits_t bits = xEventGroupWaitBits(s_evg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    char msg[160];
    if (bits & WIFI_CONNECTED_BIT) {
        esp_err_t e = settings_persist(ctx->wifi_mask);
        snprintf(msg, sizeof(msg), e == ESP_OK
                 ? "Joined \"%s\" and saved the new WiFi settings."
                 : "Joined \"%s\" but saving failed; the old settings return after a reboot.",
                 cur.wifi_ssid);
        ESP_LOGI(TAG, "%s", msg);
    } else {
        settings_update_wifi(&ctx->old);
        wifi_fill_config(&wc, ctx->old.wifi_ssid, ctx->old.wifi_pass, ctx->old.wifi_bssid);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_disconnect();
        snprintf(msg, sizeof(msg),
                 "Could not join \"%s\" within 30 s. Reverted to the previous WiFi settings.",
                 cur.wifi_ssid);
        ESP_LOGE(TAG, "%s", msg);
    }
    settings_wifi_msg_set(msg);

    s_apply_busy = false;
    free(ctx);
    vTaskDelete(NULL);
}

void wifi_apply(const settings_t *old_cfg, uint32_t wifi_mask) {
    wifi_apply_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->old = *old_cfg;
    ctx->wifi_mask = wifi_mask;
    s_apply_busy = true;
    settings_wifi_msg_set("Switching WiFi...");
    if (xTaskCreate(apply_task, "wifi_apply", 4096, ctx, 5, NULL) != pdPASS) {
        /* Could not start: undo the live change so config and radio agree. */
        settings_update_wifi(old_cfg);
        settings_wifi_msg_set("Could not start the WiFi switch (out of memory). Nothing changed.");
        s_apply_busy = false;
        free(ctx);
    }
}

void wifi_init_sta(void) {
    s_evg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    settings_t cfg;
    settings_copy(&cfg);
    wifi_config_t wifi_cfg;
    wifi_fill_config(&wifi_cfg, cfg.wifi_ssid, cfg.wifi_pass, cfg.wifi_bssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Mains powered: turn off modem power-save, which can drop inbound packets. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    xTaskCreate(ap_task, "wifi_ap", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "connecting to SSID \"%s\"...", cfg.wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_evg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG, "connected");
    else                           ESP_LOGE(TAG, "failed to connect within 30 s");
}
