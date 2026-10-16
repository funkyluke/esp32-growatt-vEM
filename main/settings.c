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

typedef enum { F_STR, F_INT } field_kind_t;

#define FLD_SECRET  0x01
#define FLD_UPPER   0x02
#define APPLY_WIFI  0x01
#define APPLY_SHELLY 0x02

typedef struct {
    const char *category;
    const char *key;
    const char *label;
    const char *help;
    field_kind_t kind;
    uint8_t flags;
    uint8_t apply;
    size_t offset;
    size_t size;
    int32_t min, max;
    const char *(*validate)(const char *);
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
    if (n < 6 || n > 12) return "Expected 6 to 12 hex digits, e.g. 485519D9D81A";
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return "Only hex digits (0-9, A-F) are allowed";
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

static const setting_def_t SETTINGS[] = {
    { "WiFi", "wifi_ssid", "Network name (SSID)",
      "2.4 GHz networks only. Changing this reconnects the device.",
      F_STR, 0, APPLY_WIFI, offsetof(settings_t, wifi_ssid), FSIZE(wifi_ssid), 1, 32, NULL },
    { "WiFi", "wifi_pass", "Password",
      "8 to 63 characters. Leave empty to keep the current password.",
      F_STR, FLD_SECRET, APPLY_WIFI, offsetof(settings_t, wifi_pass), FSIZE(wifi_pass), 0, 63, val_wifi_pass },
    { "Shelly 3EM", "shelly_id", "Device ID",
      "Printed on the Shelly label, e.g. 485519D9D81A. Used for mDNS (shellyem3-<ID>.local).",
      F_STR, FLD_UPPER, APPLY_SHELLY, offsetof(settings_t, shelly_id), FSIZE(shelly_id), 6, 12, val_hex_id },
    { "Shelly 3EM", "shelly_ip", "Fallback IP address",
      "Only used if mDNS finds nothing. Leave empty to disable.",
      F_STR, 0, APPLY_SHELLY, offsetof(settings_t, shelly_ip), FSIZE(shelly_ip), 0, 15, val_ipv4 },
    { "Shelly 3EM", "shelly_poll_s", "Poll interval (seconds)",
      "How often the Shelly is read, 1 to 60. The fresh/stale thresholds scale with it.",
      F_INT, 0, 0, offsetof(settings_t, shelly_poll_s), 0, 1, 60, NULL },
};
#define SETTINGS_N (sizeof(SETTINGS) / sizeof(SETTINGS[0]))
_Static_assert(SETTINGS_N <= 32, "changed-field bitmask is 32 bits wide");

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
}

esp_err_t settings_persist(uint32_t mask) {
    if (mask == 0) return ESP_OK;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    for (size_t i = 0; i < SETTINGS_N && e == ESP_OK; i++) {
        if (!(mask & (1u << i))) continue;
        const setting_def_t *f = &SETTINGS[i];
        e = (f->kind == F_STR) ? nvs_set_str(h, f->key, fld_str(&s_cfg, f))
                               : nvs_set_i32(h, f->key, fld_int(&s_cfg, f));
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
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        for (size_t i = 0; i < SETTINGS_N; i++) {
            const setting_def_t *f = &SETTINGS[i];
            if (f->kind == F_STR) {
                char tmp[80]; size_t n = sizeof(tmp);
                if (nvs_get_str(h, f->key, tmp, &n) == ESP_OK && (int32_t)strlen(tmp) <= f->max)
                    strlcpy(fld_str_w(&c, f), tmp, f->size);
            } else {
                int32_t v;
                if (nvs_get_i32(h, f->key, &v) == ESP_OK && v >= f->min && v <= f->max)
                    *fld_int_w(&c, f) = v;
            }
        }
        nvs_close(h);
    }

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
        if (f->kind == F_STR) {
            bool secret = (f->flags & FLD_SECRET) != 0;
            cJSON_AddStringToObject(o, "type", secret ? "password" : "text");
            cJSON_AddNumberToObject(o, "maxlen", f->max);
            cJSON_AddBoolToObject(o, "secret", secret);
            if (secret) cJSON_AddBoolToObject(o, "is_set", fld_str(&c, f)[0] != '\0');
            else         cJSON_AddStringToObject(o, "value", fld_str(&c, f));
        } else {
            cJSON_AddStringToObject(o, "type", "number");
            cJSON_AddNumberToObject(o, "value", fld_int(&c, f));
            cJSON_AddNumberToObject(o, "min", f->min);
            cJSON_AddNumberToObject(o, "max", f->max);
        }
        cJSON_AddItemToArray(fields, o);
    }
    cJSON_AddStringToObject(root, "wifi_apply", msg);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

bool settings_apply(const cJSON *obj, settings_t *old, uint32_t *changed,
                    uint32_t *wifi_mask, bool *shelly_changed,
                    char *err, size_t errsz) {
    settings_t cur, nw;
    settings_copy(&cur);
    nw = cur;
    *changed = *wifi_mask = 0;
    *shelly_changed = false;
    if (old) *old = cur;
    err[0] = '\0';

    for (size_t i = 0; i < SETTINGS_N && !err[0]; i++) {
        const setting_def_t *f = &SETTINGS[i];
        cJSON *it = cJSON_GetObjectItem((cJSON *)obj, f->key);
        if (!it) continue;
        if (f->kind == F_STR) {
            if (!cJSON_IsString(it)) { snprintf(err, errsz, "%s: expected text", f->label); break; }
            char tmp[80];
            strlcpy(tmp, it->valuestring, sizeof(tmp));
            if ((f->flags & FLD_SECRET) && tmp[0] == '\0') continue;
            int32_t n = (int32_t)strlen(it->valuestring);
            if (n < f->min || n > f->max || has_ctrl(tmp)) {
                snprintf(err, errsz, "%s: must be %d to %d characters", f->label, (int)f->min, (int)f->max);
                break;
            }
            if (f->flags & FLD_UPPER) for (char *p = tmp; *p; p++) *p = toupper((unsigned char)*p);
            const char *why = f->validate ? f->validate(tmp) : NULL;
            if (why) { snprintf(err, errsz, "%s: %s", f->label, why); break; }
            strlcpy(fld_str_w(&nw, f), tmp, f->size);
        } else {
            if (!cJSON_IsNumber(it) || it->valuedouble < f->min || it->valuedouble > f->max) {
                snprintf(err, errsz, "%s: must be %d to %d", f->label, (int)f->min, (int)f->max);
                break;
            }
            *fld_int_w(&nw, f) = (int32_t)it->valuedouble;
        }
        if (!fld_equal(f, &cur, &nw)) {
            *changed |= 1u << i;
            if (f->apply & APPLY_WIFI) *wifi_mask |= 1u << i;
            if (f->apply & APPLY_SHELLY) *shelly_changed = true;
        }
    }

    if (err[0]) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = nw;
    xSemaphoreGive(s_mutex);
    return true;
}
