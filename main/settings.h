#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "cJSON.h"

/* Every user-editable setting lives here and is described by one row in the
 * SETTINGS[] table in settings.c. Factory defaults come from
 * wifi_credentials.h and shelly_config.h; NVS overrides them after first save. */
typedef struct {
    char    wifi_ssid[33];
    char    wifi_pass[65];
    char    shelly_id[24];
    char    shelly_ip[16];
    int32_t shelly_poll_s;
} settings_t;

void settings_init(void);

/* Copy the current live config (thread-safe). */
void settings_copy(settings_t *out);

/* Persist the fields selected by `mask` (bit i = SETTINGS[i]) from the live
 * config into NVS. */
esp_err_t settings_persist(uint32_t mask);

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

/* Apply a parsed JSON object of settings. Returns false and fills `err` on a
 * validation failure. On success commits to the live config and reports what
 * changed: `*old` is the pre-change config (for the WiFi revert path),
 * `*changed`/`*wifi_mask` are SETTINGS[] bitmasks, `*shelly_changed` tells the
 * caller to re-discover the Shelly. */
bool settings_apply(const cJSON *obj, settings_t *old, uint32_t *changed,
                    uint32_t *wifi_mask, bool *shelly_changed,
                    char *err, size_t errsz);
