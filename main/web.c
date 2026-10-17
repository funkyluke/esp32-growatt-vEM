#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "settings.h"
#include "shelly.h"
#include "wifi.h"
#include "ota.h"
#include "vem.h"
#include "modbus.h"
#include "web.h"

static const char *TAG = "web";

static httpd_handle_t s_server;

/* Pages embedded from main/www/ (EMBED_TXTFILES adds a trailing NUL). */
extern const char index_html_start[]    asm("_binary_index_html_start");
extern const char index_html_end[]      asm("_binary_index_html_end");
extern const char settings_html_start[] asm("_binary_settings_html_start");
extern const char settings_html_end[]   asm("_binary_settings_html_end");

/* ---- helpers ---- */

static esp_err_t send_page(httpd_req_t *req, const char *start, const char *end) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, start, end - start - 1);
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* Sends a cJSON object and frees it. */
esp_err_t web_send_json(httpd_req_t *req, cJSON *o) {
    char *json = o ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return e;
}

/* A JSON (or binary) content type cannot be sent cross-site without a CORS
 * preflight, which this server never answers, so it blocks CSRF. */
static bool content_type_is(httpd_req_t *req, const char *type) {
    char ctype[48] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));
    return strncmp(ctype, type, strlen(type)) == 0;
}

/* HTTP Basic auth against the web password (user "admin"). Returns true if
 * the request may proceed; otherwise a 401 has already been sent. */
bool web_check_auth(httpd_req_t *req) {
    settings_t cfg;
    settings_copy(&cfg);
    if (cfg.web_pass[0] == '\0') return true;

    char hdr[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[96];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &n, (const unsigned char *)hdr + 6,
                                  strlen(hdr + 6)) == 0) {
            dec[n] = '\0';
            char want[48];
            snprintf(want, sizeof(want), "admin:%s", cfg.web_pass);
            /* constant-time compare */
            size_t wl = strlen(want);
            unsigned diff = (n != wl);
            for (size_t i = 0; i < n && i < wl; i++) diff |= dec[i] ^ (unsigned char)want[i];
            if (!diff) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Growatt vEM\"");
    httpd_resp_send(req, "Login required", HTTPD_RESP_USE_STRLEN);
    return false;
}

/* ---- pages ---- */

static esp_err_t index_handler(httpd_req_t *req) {
    return send_page(req, index_html_start, index_html_end);
}

static esp_err_t settings_page_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    return send_page(req, settings_html_start, settings_html_end);
}

/* ---- API ---- */

static esp_err_t status_handler(httpd_req_t *req) {
    settings_t cfg;
    settings_copy(&cfg);
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(wifi_ip()));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddStringToObject(o, "wifi", cfg.wifi_ssid);   /* escaped by cJSON */
    return web_send_json(req, o);
}

static const char *reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "crash (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    case ESP_RST_USB:      return "USB";
    default:               return "other";
    }
}

static esp_err_t info_handler(httpd_req_t *req) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "built", esp_app_get_description()->date);
    cJSON_AddStringToObject(o, "reset reason", reset_reason_name(esp_reset_reason()));
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t ost;
    bool pending = esp_ota_get_state_partition(run, &ost) == ESP_OK && ost == ESP_OTA_IMG_PENDING_VERIFY;
    char part[40];
    snprintf(part, sizeof(part), "%s%s", run->label, pending ? " (not yet confirmed)" : "");
    cJSON_AddStringToObject(o, "app slot", part);
    cJSON_AddNumberToObject(o, "uptime s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "free heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "min free heap", esp_get_minimum_free_heap_size());
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(o, "WiFi RSSI dBm", ap.rssi);
    return web_send_json(req, o);
}

/* Rounded to 2 decimals so the JSON stays short (float -> double noise). */
static double r2(float f) { return round((double)f * 100.0) / 100.0; }

static esp_err_t power_handler(httpd_req_t *req) {
    vem_t v;
    vem_compute(&v);
    settings_t cfg;
    settings_copy(&cfg);
    char diag_url[96];
    int diag_status = shelly_diag(diag_url, sizeof(diag_url));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", cfg.shelly_id);
    cJSON_AddBoolToObject(o, "ok", v.state == SHELLY_FRESH || v.state == SHELLY_AGING);
    cJSON_AddStringToObject(o, "state", shelly_state_name(v.state));
    cJSON_AddNumberToObject(o, "age_ms", (double)v.age_ms);
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(o, "total", r2(v.snap.total_power));
    cJSON_AddNumberToObject(o, "import_kwh", r2(v.import_kwh));
    cJSON_AddNumberToObject(o, "export_kwh", r2(v.export_kwh));
    cJSON_AddNumberToObject(o, "err", diag_status);
    cJSON_AddStringToObject(o, "url", diag_url);
    cJSON *ph = cJSON_AddArrayToObject(o, "phases");
    for (int i = 0; i < 3; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "power", r2(v.snap.phase[i].power));
        cJSON_AddNumberToObject(p, "current", r2(v.snap.phase[i].current));
        cJSON_AddNumberToObject(p, "voltage", r2(v.snap.phase[i].voltage));
        cJSON_AddItemToArray(ph, p);
    }
    cJSON *lim = cJSON_AddObjectToObject(o, "limiter");
    cJSON_AddStringToObject(lim, "state", limiter_state_name(v.lim.state));
    cJSON_AddNumberToObject(lim, "limit_w", cfg.lim_export_w);
    cJSON_AddNumberToObject(lim, "offset_w", r2(v.lim.offset_w));
    cJSON_AddNumberToObject(lim, "reported_w", r2(v.lim.reported_w));
    cJSON_AddBoolToObject(lim, "answering", v.lim.answer && cfg.mb_mode == MODBUS_MODE_EMULATE);
    cJSON *inv = cJSON_AddObjectToObject(o, "inverter");
    cJSON_AddNumberToObject(inv, "last_poll_age_ms", (double)modbus_last_request_age_ms());
    cJSON_AddStringToObject(inv, "meter", mb_meter_name((mb_meter_t)cfg.mb_meter));
    return web_send_json(req, o);
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    char *json = settings_to_json();
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);
    return ESP_OK;
}

/* Reads a small JSON request body. NULL (with a reply sent) on failure. */
static cJSON *read_json_body(httpd_req_t *req) {
    if (!content_type_is(req, "application/json")) {
        reply(req, "415 Unsupported Media Type", "{\"ok\":false,\"error\":\"Content-Type must be application/json\"}");
        return NULL;
    }
    if (req->content_len == 0 || req->content_len > 2048) {
        reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"Bad request size\"}");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { reply(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"Out of memory\"}"); return NULL; }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(body); return NULL; }
        got += n;
    }
    cJSON *root = cJSON_ParseWithLength(body, got);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        reply(req, "400 Bad Request", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
        return NULL;
    }
    return root;
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    if (wifi_apply_busy())
        return reply(req, "409 Conflict", "{\"ok\":false,\"error\":\"A WiFi change is still being applied, try again in a moment\"}");
    cJSON *root = read_json_body(req);
    if (!root) return ESP_OK;

    settings_t old;
    uint64_t changed = 0;
    char err[160];
    bool ok = settings_apply(root, &old, &changed, err, sizeof(err));
    cJSON_Delete(root);
    if (!ok) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", false);
        cJSON_AddStringToObject(o, "error", err);
        httpd_resp_set_status(req, "400 Bad Request");
        return web_send_json(req, o);
    }

    /* WiFi fields are persisted only after the new network really works. */
    uint64_t wifi_mask = changed & settings_mask(APPLY_WIFI);
    esp_err_t pe = settings_persist(changed & ~wifi_mask);
    settings_notify(settings_apply_flags(changed & ~wifi_mask));
    if (wifi_mask) wifi_apply(&old, wifi_mask);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", pe == ESP_OK);
    if (pe != ESP_OK) cJSON_AddStringToObject(o, "error", "Saving to flash failed");
    cJSON_AddNumberToObject(o, "changed", __builtin_popcountll(changed));
    cJSON_AddBoolToObject(o, "wifi_pending", wifi_mask != 0);
    return web_send_json(req, o);
}

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    if (!content_type_is(req, "application/json"))
        return reply(req, "415 Unsupported Media Type", "{\"ok\":false}");
    ESP_LOGW(TAG, "reboot requested");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return reply(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t ota_handler(httpd_req_t *req) {
    if (!web_check_auth(req)) return ESP_OK;
    if (!content_type_is(req, "application/octet-stream"))
        return reply(req, "415 Unsupported Media Type", "{\"ok\":false}");
    return ota_upload_handler(req);
}

void web_register(const httpd_uri_t *uri) {
    if (s_server && httpd_register_uri_handler(s_server, uri) != ESP_OK)
        ESP_LOGE(TAG, "could not register %s", uri->uri);
}

void web_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }

    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/settings",     .method = HTTP_GET,  .handler = settings_page_handler },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/api/info",     .method = HTTP_GET,  .handler = info_handler },
        { .uri = "/api/power",    .method = HTTP_GET,  .handler = power_handler },
        { .uri = "/api/settings", .method = HTTP_GET,  .handler = settings_get_handler },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/api/reboot",   .method = HTTP_POST, .handler = reboot_handler },
        { .uri = "/api/ota",      .method = HTTP_POST, .handler = ota_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) web_register(&uris[i]);

    ESP_LOGI(TAG, "web server started on http://" IPSTR, IP2STR(wifi_ip()));
}
