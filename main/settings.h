#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* Every user-editable setting lives here and is described by one row in the
 * SETTINGS[] table in settings.c. Factory defaults come from
 * wifi_credentials.h and shelly_config.h; NVS overrides them after first save.
 * Bool and select fields are int32_t (select = index into its options). */
typedef struct {
    /* WiFi */
    char    wifi_ssid[33];
    char    wifi_pass[65];
    /* Shelly 3EM */
    char    shelly_id[24];
    char    shelly_ip[16];
    int32_t shelly_poll_s;
    /* Meter emulation (Modbus) */
    int32_t mb_mode;          /* modbus_mode_t */
    int32_t mb_meter;         /* mb_meter_t */
    int32_t mb_addr;
    int32_t mb_baud;          /* index: 2400, 4800, 9600, 19200 */
    int32_t mb_invert;
    /* Export limit */
    int32_t lim_enable;
    int32_t lim_export_w;
    int32_t lim_margin_w;
    int32_t lim_failsafe;     /* limiter_failsafe_t */
    /* MQTT / Home Assistant */
    int32_t mqtt_enable;
    char    mqtt_uri[96];
    char    mqtt_user[33];
    char    mqtt_pass[65];
    char    mqtt_base[33];
    char    mqtt_disc[33];
    int32_t mqtt_ha;
    int32_t mqtt_period_s;
    /* System */
    char    web_pass[33];
} settings_t;

/* What has to be re-applied when a field changes (SETTINGS[].apply). */
#define APPLY_WIFI     0x01
#define APPLY_SHELLY   0x02
#define APPLY_MODBUS   0x04
#define APPLY_MQTT     0x08
#define APPLY_TELEGRAM 0x10
#define APPLY_SYSTEM   0x20
#define APPLY_LIMIT    0x40

void settings_init(void);

/* Copy the current live config (thread-safe). */
void settings_copy(settings_t *out);

/* Persist the fields selected by `mask` (bit i = SETTINGS[i]) from the live
 * config into NVS. */
esp_err_t settings_persist(uint64_t mask);

/* JSON description of all settings grouped by category. Caller frees with
 * cJSON_free(). Secrets are reported only as "is_set", never their value. */
char *settings_to_json(void);

/* Result of the last WiFi change, shown on the settings page. */
void settings_wifi_msg_set(const char *msg);
void settings_wifi_msg_get(char *out, size_t n);

/* Empty string and "0.0.0.0" both mean "not set". */
bool settings_ip_is_set(const char *s);

/* Write new WiFi credentials into the live config (used by the revert path). */
void settings_update_wifi(const char *ssid, const char *pass);

/* Apply a parsed JSON object of settings (any subset of the keys; a secret
 * given as "" is kept, given as null is cleared). Returns false and fills
 * `err` on a validation failure. On success commits to the live config,
 * stores the pre-change config in `*old` and the changed fields as a
 * SETTINGS[] bitmask in `*changed`. */
bool settings_apply(const cJSON *obj, settings_t *old, uint64_t *changed,
                    char *err, size_t errsz);

/* Change a single integer/bool/select setting by key, persist it and run the
 * change callbacks (used by MQTT commands). */
esp_err_t settings_set_int(const char *key, int32_t value);

/* OR of the APPLY_* flags of the fields in `changed`. */
uint32_t settings_apply_flags(uint64_t changed);

/* Bitmask of the fields that carry `apply_flag`. */
uint64_t settings_mask(uint32_t apply_flag);

/* Change notification: `cb` runs (in the caller's task) after fields with
 * `apply_flag` changed and were persisted. WiFi is handled separately. */
typedef void (*settings_cb_t)(void);
void settings_subscribe(uint32_t apply_flag, settings_cb_t cb);
void settings_notify(uint32_t apply_flags);
