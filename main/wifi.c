#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_evg;
#define WIFI_CONNECTED_BIT BIT0
static int s_retry_count;
static esp_ip4_addr_t s_ip;
static volatile bool s_apply_busy;

static void wifi_fill_config(wifi_config_t *wc, const char *ssid, const char *pass) {
    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->sta.ssid, ssid, sizeof(wc->sta.ssid));
    strlcpy((char *)wc->sta.password, pass, sizeof(wc->sta.password));
    wc->sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Several APs share this SSID (mesh/repeater); join the strongest. */
    wc->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        s_retry_count++;
        ESP_LOGW(TAG, "disconnected (reason %d), retry #%d", d->reason, s_retry_count);
        xEventGroupClearBits(s_evg, WIFI_CONNECTED_BIT);
        /* Never block: this runs in the system event loop. */
        esp_wifi_connect();
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
    wifi_fill_config(&wc, cur.wifi_ssid, cur.wifi_pass);

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
        settings_update_wifi(ctx->old.wifi_ssid, ctx->old.wifi_pass);
        wifi_fill_config(&wc, ctx->old.wifi_ssid, ctx->old.wifi_pass);
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
        settings_update_wifi(old_cfg->wifi_ssid, old_cfg->wifi_pass);
        settings_wifi_msg_set("Could not start the WiFi switch (out of memory). Nothing changed.");
        s_apply_busy = false;
        free(ctx);
    }
}

void wifi_init_sta(void) {
    s_evg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    settings_t cfg;
    settings_copy(&cfg);
    wifi_config_t wifi_cfg;
    wifi_fill_config(&wifi_cfg, cfg.wifi_ssid, cfg.wifi_pass);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Mains powered: turn off modem power-save, which can drop inbound packets. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "connecting to SSID \"%s\"...", cfg.wifi_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_evg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG, "connected");
    else                           ESP_LOGE(TAG, "failed to connect within 30 s");
}
