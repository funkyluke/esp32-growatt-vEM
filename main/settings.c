#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "wifi_credentials.h"
#include "shelly_config.h"
#include "settings.h"

static const char *TAG = "settings";

#define NVS_NS "cfg"

typedef enum { F_STR, F_INT, F_BOOL, F_SELECT } field_kind_t;

#define FLD_SECRET  0x01
#define FLD_UPPER   0x02

typedef struct {
    const char *category;
    const char *key;          /* also the NVS key: at most 15 characters */
    const char *label;
    const char *help;
    field_kind_t kind;
    uint8_t flags;
    uint8_t apply;
    size_t offset;
    size_t size;
    int32_t min, max;         /* string length or integer range */
    const char *(*validate)(const char *);
    const char *options;      /* F_SELECT: "a|b|c" */
} setting_def_t;

static settings_t s_cfg;
static SemaphoreHandle_t s_mutex;
static char s_wifi_apply_msg[160];

static bool ip_is_set(const char *s) { return s[0] != '\0' && strcmp(s, "0.0.0.0") != 0; }

static bool has_ctrl(const char *s) {
    for (; *s; s++) if ((unsigned char)*s < 0x20 || *s == 0x7f) return true;
    return false;
}

static const char *val_wifi_pass(const char *s) {
    size_t n = strlen(s);
    return (n != 0 && n < 8) ? "WPA2 passwords need 8 to 63 characters" : NULL;
}

static const char *val_hex_id(const char *s) {
    size_t n = strlen(s);
    if (n < 6 || n > 12) return "Expected 6 to 12 hex digits, e.g. A1B2C3D4E5F6";
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return "Only hex digits (0-9, A-F) are allowed";
    return NULL;
}

static const char *val_mqtt_uri(const char *s) {
    if (!*s) return NULL;
    if (strncmp(s, "mqtt://", 7) && strncmp(s, "mqtts://", 8) && strncmp(s, "ws://", 5) && strncmp(s, "wss://", 6))
        return "Must start with mqtt://, mqtts://, ws:// or wss://, e.g. mqtt://192.168.1.10:1883";
    return NULL;
}

static const char *val_topic(const char *s) {
    for (; *s; s++) if (*s == '+' || *s == '#' || *s == ' ') return "No spaces, '+' or '#'";
    return NULL;
}

static const char *val_ipv4(const char *s) {
    if (!*s) return NULL;
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') dots++;
        else if (!isdigit((unsigned char)*p)) return "Not a valid IPv4 address";
    }
    unsigned a, b, c, d; char tail;
    if (dots != 3 || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) return "Not a valid IPv4 address";
    return NULL;
}

#define FSIZE(m) sizeof(((settings_t *)0)->m)
#define STR(cat, key, label, help, flags, apply, mn, mx, val) \
    { cat, #key, label, help, F_STR, flags, apply, offsetof(settings_t, key), FSIZE(key), mn, mx, val, NULL }
#define INT(cat, key, label, help, apply, mn, mx) \
    { cat, #key, label, help, F_INT, 0, apply, offsetof(settings_t, key), FSIZE(key), mn, mx, NULL, NULL }
#define BOOL(cat, key, label, help, apply) \
    { cat, #key, label, help, F_BOOL, 0, apply, offsetof(settings_t, key), FSIZE(key), 0, 1, NULL, NULL }
#define SELECT(cat, key, label, help, apply, n, opts) \
    { cat, #key, label, help, F_SELECT, 0, apply, offsetof(settings_t, key), FSIZE(key), 0, (n) - 1, NULL, opts }

static const setting_def_t SETTINGS[] = {
    STR("WiFi", wifi_ssid, "Network name (SSID)",
        "2.4 GHz networks only. Changing this reconnects the device.",
        0, APPLY_WIFI, 1, 32, NULL),
    STR("WiFi", wifi_pass, "Password",
        "8 to 63 characters. Leave empty to keep the current password.",
        FLD_SECRET, APPLY_WIFI, 0, 63, val_wifi_pass),

    STR("Shelly 3EM", shelly_id, "Device ID",
        "Printed on the Shelly label, e.g. A1B2C3D4E5F6. Used for mDNS (shellyem3-<ID>.local).",
        FLD_UPPER, APPLY_SHELLY, 6, 12, val_hex_id),
    STR("Shelly 3EM", shelly_ip, "Fallback IP address",
        "Only used if mDNS finds nothing. Leave empty to disable.",
        0, APPLY_SHELLY, 0, 15, val_ipv4),
    INT("Shelly 3EM", shelly_poll_s, "Poll interval (seconds)",
        "How often the Shelly is read, 1 to 60. Use 1 or 2 for export limiting: the inverter "
        "only gets data fresher than max(5 s, 2.5 polls), otherwise it is throttled.",
        0, 1, 60),

    SELECT("Meter emulation", mb_mode, "Mode",
        "Emulate: answer the inverter. Listen only: log the inverter's requests without "
        "answering (for diagnosis, see /api/modbus).", APPLY_MODBUS, 3,
        "Off|Emulate meter|Listen only"),
    SELECT("Meter emulation", mb_meter, "Emulated meter",
        "Meter model the inverter is set to. The Growatt MIC accepts the Eastron SDM120 "
        "and the Chint DDSU666.", APPLY_MODBUS, 2, "Eastron SDM120|Chint DDSU666"),
    INT("Meter emulation", mb_addr, "Modbus address",
        "Meter address the inverter polls, 1 to 247 (Growatt default: 1).", APPLY_MODBUS, 1, 247),
    SELECT("Meter emulation", mb_baud, "Baud rate",
        "Serial speed of the meter port (Growatt default: 9600, 8N1).", APPLY_MODBUS, 4,
        "2400|4800|9600|19200"),
    BOOL("Meter emulation", mb_invert, "Invert power sign",
        "Only if the inverter reacts the wrong way round (exports more when it should limit).",
        APPLY_MODBUS),

    BOOL("Export limit", lim_enable, "Limit export",
        "On: the reported power is shifted so the inverter (set to 0 W export) exports at most "
        "the limit below. Off: the real grid power is reported.", APPLY_LIMIT),
    INT("Export limit", lim_export_w, "Maximum export (W)",
        "Power that may flow into the grid, 0 to 5000 W.", APPLY_LIMIT, 0, 5000),
    INT("Export limit", lim_margin_w, "Safety margin (W)",
        "Regulate this much below the limit to absorb measurement delay, 0 to 500 W.", APPLY_LIMIT, 0, 500),
    SELECT("Export limit", lim_failsafe, "When meter data is stale",
        "Throttle: report a large export so the inverter backs off. Stop answering: the "
        "inverter's own meter-loss behaviour applies.", APPLY_LIMIT, 2, "Throttle|Stop answering"),

    BOOL("MQTT", mqtt_enable, "Enable MQTT",
        "Publish the values to an MQTT broker (e.g. the Home Assistant Mosquitto add-on).", APPLY_MQTT),
    STR("MQTT", mqtt_uri, "Broker URI",
        "e.g. mqtt://192.168.1.10:1883 (mqtts:// verifies against public CAs).",
        0, APPLY_MQTT, 0, 95, val_mqtt_uri),
    STR("MQTT", mqtt_user, "User name", "Leave empty for anonymous access.", 0, APPLY_MQTT, 0, 32, NULL),
    STR("MQTT", mqtt_pass, "Password", "Leave empty to keep the current password.",
        FLD_SECRET, APPLY_MQTT, 0, 64, NULL),
    STR("MQTT", mqtt_base, "Base topic", "State goes to <base>/state, commands to <base>/limit/set.",
        0, APPLY_MQTT, 1, 32, val_topic),
    BOOL("MQTT", mqtt_ha, "Home Assistant discovery",
        "Create the device and its entities in Home Assistant automatically. Turning it off removes them.",
        APPLY_MQTT),
    STR("MQTT", mqtt_disc, "Discovery prefix", "Home Assistant default: homeassistant.",
        0, APPLY_MQTT, 1, 32, val_topic),
    INT("MQTT", mqtt_period_s, "Publish interval (seconds)",
        "How often the state is published, 1 to 300. Changes from HA are confirmed at once.",
        APPLY_MQTT, 1, 300),

    STR("System", web_pass, "Web password",
        "Protects the settings and firmware update (user name: admin). Empty = no password.",
        FLD_SECRET, APPLY_SYSTEM, 0, 32, NULL),
};
#define SETTINGS_N (sizeof(SETTINGS) / sizeof(SETTINGS[0]))
_Static_assert(SETTINGS_N <= 64, "changed-field bitmask is 64 bits wide");

#define FBIT(i) (1ULL << (i))

static inline char *fld_str_w(settings_t *c, const setting_def_t *f) { return (char *)c + f->offset; }
static inline const char *fld_str(const settings_t *c, const setting_def_t *f) { return (const char *)c + f->offset; }
static inline int32_t *fld_int_w(settings_t *c, const setting_def_t *f) { return (int32_t *)((char *)c + f->offset); }
static inline int32_t fld_int(const settings_t *c, const setting_def_t *f) { return *(const int32_t *)((const char *)c + f->offset); }

static bool fld_equal(const setting_def_t *f, const settings_t *a, const settings_t *b) {
    return f->kind == F_STR ? strcmp(fld_str(a, f), fld_str(b, f)) == 0
                            : fld_int(a, f) == fld_int(b, f);
}

void settings_copy(settings_t *out) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

void settings_wifi_msg_set(const char *msg) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_wifi_apply_msg, msg, sizeof(s_wifi_apply_msg));
    xSemaphoreGive(s_mutex);
}

void settings_wifi_msg_get(char *out, size_t n) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(out, s_wifi_apply_msg, n);
    xSemaphoreGive(s_mutex);
}

bool settings_ip_is_set(const char *s) { return ip_is_set(s); }

void settings_update_wifi(const char *ssid, const char *pass) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass));
    xSemaphoreGive(s_mutex);
}

static void defaults(settings_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->wifi_ssid, WIFI_SSID, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, WIFI_PASSWORD, sizeof(c->wifi_pass));
    strlcpy(c->shelly_id, SHELLY_DEVICE_ID, sizeof(c->shelly_id));
    if (ip_is_set(SHELLY_FALLBACK_IP)) strlcpy(c->shelly_ip, SHELLY_FALLBACK_IP, sizeof(c->shelly_ip));
    c->shelly_poll_s = 2;
    c->mb_mode = 1;           /* emulate */
    c->mb_meter = 0;          /* SDM120 */
    c->mb_addr = 1;
    c->mb_baud = 2;           /* 9600 */
    c->lim_enable = 1;
    c->lim_export_w = 800;
    c->lim_margin_w = 30;
    c->lim_failsafe = 0;      /* throttle */
    strlcpy(c->mqtt_base, "growatt_vem", sizeof(c->mqtt_base));
    strlcpy(c->mqtt_disc, "homeassistant", sizeof(c->mqtt_disc));
    c->mqtt_ha = 1;
    c->mqtt_period_s = 10;
}

uint32_t settings_apply_flags(uint64_t changed) {
    uint32_t fl = 0;
    for (size_t i = 0; i < SETTINGS_N; i++) if (changed & FBIT(i)) fl |= SETTINGS[i].apply;
    return fl;
}

uint64_t settings_mask(uint32_t apply_flag) {
    uint64_t m = 0;
    for (size_t i = 0; i < SETTINGS_N; i++) if (SETTINGS[i].apply & apply_flag) m |= FBIT(i);
    return m;
}

#define MAX_SUBS 12
static struct { uint32_t flag; settings_cb_t cb; } s_subs[MAX_SUBS];
static int s_nsubs;

void settings_subscribe(uint32_t apply_flag, settings_cb_t cb) {
    if (s_nsubs < MAX_SUBS) { s_subs[s_nsubs].flag = apply_flag; s_subs[s_nsubs].cb = cb; s_nsubs++; }
    else ESP_LOGE(TAG, "too many subscribers");
}

void settings_notify(uint32_t apply_flags) {
    for (int i = 0; i < s_nsubs; i++) if (s_subs[i].flag & apply_flags) s_subs[i].cb();
}

esp_err_t settings_persist(uint64_t mask) {
    if (mask == 0) return ESP_OK;
    settings_t c;
    settings_copy(&c);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (size_t i = 0; i < SETTINGS_N && e == ESP_OK; i++) {
        if (!(mask & FBIT(i))) continue;
        const setting_def_t *f = &SETTINGS[i];
        e = (f->kind == F_STR) ? nvs_set_str(h, f->key, fld_str(&c, f))
                               : nvs_set_i32(h, f->key, fld_int(&c, f));
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void settings_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    settings_t c;
    defaults(&c);

    nvs_handle_t h;
    bool have_nvs = nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK;
    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_def_t *f = &SETTINGS[i];
        if (strlen(f->key) > NVS_KEY_NAME_MAX_SIZE - 1) ESP_LOGE(TAG, "NVS key too long: %s", f->key);
        if (!have_nvs) continue;
        if (f->kind == F_STR) {
            char tmp[128]; size_t n = sizeof(tmp);
            if (nvs_get_str(h, f->key, tmp, &n) == ESP_OK && (int32_t)strlen(tmp) <= f->max)
                strlcpy(fld_str_w(&c, f), tmp, f->size);
        } else {
            int32_t v;
            if (nvs_get_i32(h, f->key, &v) == ESP_OK && v >= f->min && v <= f->max)
                *fld_int_w(&c, f) = v;
        }
    }
    if (have_nvs) nvs_close(h);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = c;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Loaded: SSID \"%s\", Shelly %s, poll %d s",
             c.wifi_ssid, c.shelly_id, (int)c.shelly_poll_s);
}

char *settings_to_json(void) {
    settings_t c;
    settings_copy(&c);
    char msg[sizeof(s_wifi_apply_msg)];
    settings_wifi_msg_get(msg, sizeof(msg));

    cJSON *root = cJSON_CreateObject();
    cJSON *cats = root ? cJSON_AddArrayToObject(root, "categories") : NULL;
    if (!cats) { cJSON_Delete(root); return NULL; }

    cJSON *fields = NULL;
    const char *last = NULL;
    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_def_t *f = &SETTINGS[i];
        if (!last || strcmp(last, f->category) != 0) {
            cJSON *cat = cJSON_CreateObject();
            if (!cat) { cJSON_Delete(root); return NULL; }
            cJSON_AddStringToObject(cat, "name", f->category);
            fields = cJSON_AddArrayToObject(cat, "fields");
            cJSON_AddItemToArray(cats, cat);
            last = f->category;
        }
        cJSON *o = cJSON_CreateObject();
        if (!o || !fields) { cJSON_Delete(o); cJSON_Delete(root); return NULL; }
        cJSON_AddStringToObject(o, "key", f->key);
        cJSON_AddStringToObject(o, "label", f->label);
        cJSON_AddStringToObject(o, "help", f->help);
        switch (f->kind) {
        case F_STR: {
            bool secret = (f->flags & FLD_SECRET) != 0;
            cJSON_AddStringToObject(o, "type", secret ? "password" : "text");
            cJSON_AddNumberToObject(o, "maxlen", f->max);
            cJSON_AddBoolToObject(o, "secret", secret);
            if (secret) cJSON_AddBoolToObject(o, "is_set", fld_str(&c, f)[0] != '\0');
            else         cJSON_AddStringToObject(o, "value", fld_str(&c, f));
            break;
        }
        case F_INT:
            cJSON_AddStringToObject(o, "type", "number");
            cJSON_AddNumberToObject(o, "value", fld_int(&c, f));
            cJSON_AddNumberToObject(o, "min", f->min);
            cJSON_AddNumberToObject(o, "max", f->max);
            break;
        case F_BOOL:
            cJSON_AddStringToObject(o, "type", "bool");
            cJSON_AddBoolToObject(o, "value", fld_int(&c, f) != 0);
            break;
        case F_SELECT: {
            cJSON_AddStringToObject(o, "type", "select");
            cJSON_AddNumberToObject(o, "value", fld_int(&c, f));
            cJSON *opts = cJSON_AddArrayToObject(o, "options");
            const char *p = f->options;
            while (opts && p && *p) {
                const char *bar = strchr(p, '|');
                size_t n = bar ? (size_t)(bar - p) : strlen(p);
                char tmp[48];
                snprintf(tmp, sizeof(tmp), "%.*s", (int)n, p);
                cJSON_AddItemToArray(opts, cJSON_CreateString(tmp));
                p = bar ? bar + 1 : NULL;
            }
            break;
        }
        }
        cJSON_AddItemToArray(fields, o);
    }
    cJSON_AddStringToObject(root, "wifi_apply", msg);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Validates one JSON value into `nw`. Returns false with `err` set. */
static bool apply_field(const setting_def_t *f, const cJSON *it, settings_t *nw, char *err, size_t errsz) {
    if (f->kind == F_STR) {
        if ((f->flags & FLD_SECRET) && cJSON_IsNull(it)) { fld_str_w(nw, f)[0] = '\0'; return true; }
        if (!cJSON_IsString(it)) { snprintf(err, errsz, "%s: expected text", f->label); return false; }
        if ((f->flags & FLD_SECRET) && it->valuestring[0] == '\0') return true;   /* keep */
        int32_t n = (int32_t)strlen(it->valuestring);
        if (n < f->min || n > f->max || has_ctrl(it->valuestring)) {
            snprintf(err, errsz, "%s: must be %d to %d characters", f->label, (int)f->min, (int)f->max);
            return false;
        }
        char tmp[128];
        strlcpy(tmp, it->valuestring, sizeof(tmp));
        if (f->flags & FLD_UPPER) for (char *p = tmp; *p; p++) *p = toupper((unsigned char)*p);
        const char *why = f->validate ? f->validate(tmp) : NULL;
        if (why) { snprintf(err, errsz, "%s: %s", f->label, why); return false; }
        strlcpy(fld_str_w(nw, f), tmp, f->size);
        return true;
    }
    double v;
    if (f->kind == F_BOOL && cJSON_IsBool(it)) v = cJSON_IsTrue(it) ? 1 : 0;
    else if (cJSON_IsNumber(it)) v = it->valuedouble;
    else { snprintf(err, errsz, "%s: expected a number", f->label); return false; }
    if (v < f->min || v > f->max || v != (double)(int32_t)v) {
        snprintf(err, errsz, "%s: must be a whole number from %d to %d", f->label, (int)f->min, (int)f->max);
        return false;
    }
    *fld_int_w(nw, f) = (int32_t)v;
    return true;
}

bool settings_apply(const cJSON *obj, settings_t *old, uint64_t *changed,
                    char *err, size_t errsz) {
    settings_t cur, nw;
    settings_copy(&cur);
    nw = cur;
    *changed = 0;
    if (old) *old = cur;
    err[0] = '\0';

    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_def_t *f = &SETTINGS[i];
        const cJSON *it = cJSON_GetObjectItem(obj, f->key);
        if (!it) continue;
        if (!apply_field(f, it, &nw, err, errsz)) return false;
        if (!fld_equal(f, &cur, &nw)) *changed |= FBIT(i);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = nw;
    xSemaphoreGive(s_mutex);
    return true;
}

esp_err_t settings_set_int(const char *key, int32_t value) {
    for (size_t i = 0; i < SETTINGS_N; i++) {
        const setting_def_t *f = &SETTINGS[i];
        if (strcmp(f->key, key) != 0) continue;
        if (f->kind == F_STR || value < f->min || value > f->max) return ESP_ERR_INVALID_ARG;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool same = fld_int(&s_cfg, f) == value;
        *fld_int_w(&s_cfg, f) = value;
        xSemaphoreGive(s_mutex);
        if (same) return ESP_OK;
        esp_err_t e = settings_persist(FBIT(i));
        settings_notify(f->apply);
        return e;
    }
    return ESP_ERR_NOT_FOUND;
}
