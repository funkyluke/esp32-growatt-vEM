#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "settings.h"
#include "modbus.h"
#include "vem.h"
#include "wifi.h"
#include "mqtt_ha.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_restart = true;     /* (re)create the client from settings */
static volatile bool s_connected;
static volatile bool s_discovery_due;
static volatile bool s_state_due;
static char s_node[32];                    /* growatt_vem_<mac> */
static settings_t s_cfg;                   /* settings the client was started with */

bool mqtt_ha_connected(void) { return s_connected; }

static void topic(char *out, size_t n, const char *suffix) {
    snprintf(out, n, "%s/%s", s_cfg.mqtt_base, suffix);
}

static double r2(float f) { return round((double)f * 100.0) / 100.0; }

/* ---- Home Assistant discovery ---- */

typedef struct {
    const char *component;    /* sensor, binary_sensor, number, switch */
    const char *obj;          /* object id, also the JSON key in the state */
    const char *name;
    const char *unit;
    const char *dev_class;
    const char *state_class;
    const char *entity_cat;   /* NULL or "diagnostic" */
    const char *icon;
} ha_entity_t;

static const ha_entity_t ENTITIES[] = {
    { "sensor", "grid_power",     "Grid power",          "W",   "power",   "measurement", NULL, NULL },
    { "sensor", "l1_power",       "L1 power",            "W",   "power",   "measurement", NULL, NULL },
    { "sensor", "l2_power",       "L2 power",            "W",   "power",   "measurement", NULL, NULL },
    { "sensor", "l3_power",       "L3 power",            "W",   "power",   "measurement", NULL, NULL },
    { "sensor", "l1_voltage",     "L1 voltage",          "V",   "voltage", "measurement", "diagnostic", NULL },
    { "sensor", "l2_voltage",     "L2 voltage",          "V",   "voltage", "measurement", "diagnostic", NULL },
    { "sensor", "l3_voltage",     "L3 voltage",          "V",   "voltage", "measurement", "diagnostic", NULL },
    { "sensor", "reported_power", "Reported to inverter","W",   "power",   "measurement", NULL, "mdi:transmission-tower-export" },
    { "sensor", "import_energy",  "Grid import",         "kWh", "energy",  "total_increasing", NULL, NULL },
    { "sensor", "export_energy",  "Grid export",         "kWh", "energy",  "total_increasing", NULL, NULL },
    { "sensor", "limiter",        "Limiter state",       NULL,  NULL,      NULL, NULL, "mdi:speedometer" },
    { "sensor", "meter_data",     "Meter data",          NULL,  NULL,      NULL, "diagnostic", "mdi:wifi-check" },
    { "binary_sensor", "inverter_polling", "Inverter polling", NULL, "connectivity", NULL, "diagnostic", NULL },
    { "sensor", "rssi",           "WiFi signal",         "dBm", "signal_strength", "measurement", "diagnostic", NULL },
    { "sensor", "uptime",         "Uptime",              "s",   "duration", NULL, "diagnostic", NULL },
    { "number", "export_limit",   "Export limit",        "W",   "power",   NULL, NULL, "mdi:transmission-tower-export" },
    { "switch", "limiter_enabled","Export limiter",      NULL,  NULL,      NULL, NULL, "mdi:speedometer" },
};
#define ENTITIES_N (sizeof(ENTITIES) / sizeof(ENTITIES[0]))

static void publish_discovery(void) {
    char ip[16], url[40], avail[64], state[64], t[160];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(wifi_ip()));
    snprintf(url, sizeof(url), "http://%s/", ip);
    topic(avail, sizeof(avail), "status");
    topic(state, sizeof(state), "state");

    for (size_t i = 0; i < ENTITIES_N; i++) {
        const ha_entity_t *e = &ENTITIES[i];
        cJSON *o = cJSON_CreateObject();
        char uid[64], tpl[96];
        snprintf(uid, sizeof(uid), "%s_%s", s_node, e->obj);
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "unique_id", uid);
        cJSON_AddStringToObject(o, "object_id", uid);
        cJSON_AddStringToObject(o, "state_topic", state);
        cJSON_AddStringToObject(o, "availability_topic", avail);
        if (e->unit)        cJSON_AddStringToObject(o, "unit_of_measurement", e->unit);
        if (e->dev_class)   cJSON_AddStringToObject(o, "device_class", e->dev_class);
        if (e->state_class) cJSON_AddStringToObject(o, "state_class", e->state_class);
        if (e->entity_cat)  cJSON_AddStringToObject(o, "entity_category", e->entity_cat);
        if (e->icon)        cJSON_AddStringToObject(o, "icon", e->icon);

        if (!strcmp(e->component, "binary_sensor") || !strcmp(e->component, "switch")) {
            snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json.%s else 'OFF' }}", e->obj);
        } else {
            snprintf(tpl, sizeof(tpl), "{{ value_json.%s }}", e->obj);
        }
        cJSON_AddStringToObject(o, "value_template", tpl);

        if (!strcmp(e->component, "number")) {
            char cmd[64];
            topic(cmd, sizeof(cmd), "limit/set");
            cJSON_AddStringToObject(o, "command_topic", cmd);
            cJSON_AddNumberToObject(o, "min", 0);
            cJSON_AddNumberToObject(o, "max", 5000);
            cJSON_AddNumberToObject(o, "step", 10);
            cJSON_AddStringToObject(o, "mode", "box");
        } else if (!strcmp(e->component, "switch")) {
            char cmd[64];
            topic(cmd, sizeof(cmd), "limiter/set");
            cJSON_AddStringToObject(o, "command_topic", cmd);
        }

        cJSON *dev = cJSON_AddObjectToObject(o, "device");
        cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
        cJSON_AddItemToArray(ids, cJSON_CreateString(s_node));
        cJSON_AddStringToObject(dev, "name", "Growatt vEM");
        cJSON_AddStringToObject(dev, "manufacturer", "DIY");
        cJSON_AddStringToObject(dev, "model", "ESP32-S3 virtual energy meter");
        cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
        cJSON_AddStringToObject(dev, "configuration_url", url);

        char *json = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (!json) continue;
        snprintf(t, sizeof(t), "%s/%s/%s/%s/config", s_cfg.mqtt_disc, e->component, s_node, e->obj);
        esp_mqtt_client_enqueue(s_client, t, json, 0, 1, 1, true);
        cJSON_free(json);
    }
    ESP_LOGI(TAG, "published discovery for %d entities", (int)ENTITIES_N);
}

/* Remove our entities from HA (empty retained config), e.g. when discovery is turned off. */
static void clear_discovery(void) {
    char t[160];
    for (size_t i = 0; i < ENTITIES_N; i++) {
        snprintf(t, sizeof(t), "%s/%s/%s/%s/config", s_cfg.mqtt_disc, ENTITIES[i].component, s_node, ENTITIES[i].obj);
        esp_mqtt_client_enqueue(s_client, t, "", 0, 1, 1, true);
    }
}

/* ---- state ---- */

static void publish_state(void) {
    vem_t v;
    vem_compute(&v);
    settings_t cfg;
    settings_copy(&cfg);

    cJSON *o = cJSON_CreateObject();
    bool have = v.state != SHELLY_NONE;
    if (have) {
        cJSON_AddNumberToObject(o, "grid_power", r2(v.snap.total_power));
        cJSON_AddNumberToObject(o, "l1_power", r2(v.snap.phase[0].power));
        cJSON_AddNumberToObject(o, "l2_power", r2(v.snap.phase[1].power));
        cJSON_AddNumberToObject(o, "l3_power", r2(v.snap.phase[2].power));
        cJSON_AddNumberToObject(o, "l1_voltage", r2(v.snap.phase[0].voltage));
        cJSON_AddNumberToObject(o, "l2_voltage", r2(v.snap.phase[1].voltage));
        cJSON_AddNumberToObject(o, "l3_voltage", r2(v.snap.phase[2].voltage));
        if (v.import_kwh > 0) cJSON_AddNumberToObject(o, "import_energy", r2(v.import_kwh));
        if (v.export_kwh > 0) cJSON_AddNumberToObject(o, "export_energy", r2(v.export_kwh));
    } else {
        /* HA shows "unknown" rather than a made-up 0 */
        const char *keys[] = { "grid_power", "l1_power", "l2_power", "l3_power",
                               "l1_voltage", "l2_voltage", "l3_voltage" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) cJSON_AddNullToObject(o, keys[i]);
    }
    cJSON_AddNumberToObject(o, "reported_power", r2(v.lim.reported_w));
    cJSON_AddStringToObject(o, "limiter", limiter_state_name(v.lim.state));
    cJSON_AddStringToObject(o, "meter_data", shelly_state_name(v.state));
    int64_t poll = modbus_last_request_age_ms();
    cJSON_AddBoolToObject(o, "inverter_polling", poll >= 0 && poll < 10000);
    cJSON_AddNumberToObject(o, "export_limit", cfg.lim_export_w);
    cJSON_AddBoolToObject(o, "limiter_enabled", cfg.lim_enable != 0);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(o, "rssi", ap.rssi);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    char t[64];
    topic(t, sizeof(t), "state");
    esp_mqtt_client_enqueue(s_client, t, json, 0, 0, 0, true);
    cJSON_free(json);
}

/* ---- commands ---- */

static void handle_command(const char *t, int tlen, const char *data, int dlen) {
    char tp[96], payload[16], want[96];
    snprintf(tp, sizeof(tp), "%.*s", tlen, t);
    snprintf(payload, sizeof(payload), "%.*s", dlen, data);

    snprintf(want, sizeof(want), "%s/status", s_cfg.mqtt_disc);
    if (!strcmp(tp, want)) {                       /* HA (re)started */
        if (!strcmp(payload, "online")) s_discovery_due = true;
        return;
    }
    topic(want, sizeof(want), "limit/set");
    if (!strcmp(tp, want)) {
        char *end;
        double w = strtod(payload, &end);
        esp_err_t e = (end != payload) ? settings_set_int("lim_export_w", (int32_t)lround(w)) : ESP_ERR_INVALID_ARG;
        ESP_LOGI(TAG, "export limit -> %s: %s", payload, esp_err_to_name(e));
        s_state_due = true;
        return;
    }
    topic(want, sizeof(want), "limiter/set");
    if (!strcmp(tp, want)) {
        bool on = !strcasecmp(payload, "ON") || !strcmp(payload, "1") || !strcasecmp(payload, "true");
        bool off = !strcasecmp(payload, "OFF") || !strcmp(payload, "0") || !strcasecmp(payload, "false");
        if (on || off) settings_set_int("lim_enable", on ? 1 : 0);
        ESP_LOGI(TAG, "limiter -> %s", payload);
        s_state_due = true;
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to %s", s_cfg.mqtt_uri);
        char t[96];
        topic(t, sizeof(t), "status");
        esp_mqtt_client_publish(s_client, t, "online", 0, 1, 1);
        topic(t, sizeof(t), "limit/set");
        esp_mqtt_client_subscribe(s_client, t, 1);
        topic(t, sizeof(t), "limiter/set");
        esp_mqtt_client_subscribe(s_client, t, 1);
        snprintf(t, sizeof(t), "%s/status", s_cfg.mqtt_disc);
        esp_mqtt_client_subscribe(s_client, t, 1);
        s_connected = true;
        s_discovery_due = true;
        s_state_due = true;
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) ESP_LOGW(TAG, "disconnected");
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_command(ev->topic, ev->topic_len, ev->data, ev->data_len);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
            ESP_LOGW(TAG, "connection refused (code %d): check user/password",
                     ev->error_handle->connect_return_code);
        break;
    default:
        break;
    }
}

static void stop_client(void) {
    if (!s_client) return;
    if (s_connected) {
        char t[96];
        topic(t, sizeof(t), "status");
        esp_mqtt_client_publish(s_client, t, "offline", 0, 1, 1);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

static void start_client(void) {
    settings_copy(&s_cfg);
    if (!s_cfg.mqtt_enable || !s_cfg.mqtt_uri[0]) { ESP_LOGI(TAG, "disabled"); return; }

    static char lwt_topic[64];
    topic(lwt_topic, sizeof(lwt_topic), "status");
    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_cfg.mqtt_uri,
        .credentials.username = s_cfg.mqtt_user[0] ? s_cfg.mqtt_user : NULL,
        .credentials.client_id = s_node,
        .credentials.authentication.password = s_cfg.mqtt_pass[0] ? s_cfg.mqtt_pass : NULL,
        .session.last_will = { .topic = lwt_topic, .msg = "offline", .qos = 1, .retain = 1 },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .buffer.size = 1536,
        .buffer.out_size = 1536,
    };
    if (!strncmp(s_cfg.mqtt_uri, "mqtts://", 8) || !strncmp(s_cfg.mqtt_uri, "wss://", 6))
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) { ESP_LOGE(TAG, "client init failed (bad URI?)"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) { ESP_LOGE(TAG, "start failed"); stop_client(); }
}

static void mqtt_task(void *arg) {
    int64_t last_state = 0;
    bool had_discovery = false;
    while (1) {
        if (s_restart) {
            s_restart = false;
            /* turning discovery off removes the entities from HA */
            settings_t nw;
            settings_copy(&nw);
            if (s_client && s_connected && had_discovery && !nw.mqtt_ha) clear_discovery();
            stop_client();
            start_client();
        }
        if (s_client && s_connected) {
            if (s_discovery_due) {
                s_discovery_due = false;
                if (s_cfg.mqtt_ha) { publish_discovery(); had_discovery = true; }
            }
            settings_t cfg;
            settings_copy(&cfg);
            int64_t now = esp_timer_get_time();
            if (s_state_due || now - last_state >= (int64_t)cfg.mqtt_period_s * 1000000) {
                s_state_due = false;
                last_state = now;
                publish_state();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void on_mqtt_settings(void) { s_restart = true; }
static void on_limit_settings(void) { s_state_due = true; }

void mqtt_ha_init(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node, sizeof(s_node), "growatt_vem_%02x%02x%02x", mac[3], mac[4], mac[5]);
    settings_subscribe(APPLY_MQTT, on_mqtt_settings);
    settings_subscribe(APPLY_LIMIT, on_limit_settings);
    xTaskCreate(mqtt_task, "mqtt_ha", 6144, NULL, 4, NULL);
}
