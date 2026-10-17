#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "settings.h"
#include "modbus.h"
#include "vem.h"
#include "wifi.h"
#include "clock.h"
#include "telegram.h"
#include "monitor.h"

static const char *TAG = "monitor";

#define NVS_NS            "stats"
#define EXPORT_HOLD_S     60      /* export above the limit for this long -> alert */
#define EXPORT_TOL_W      50      /* tolerance above the limit */
#define AT_LIMIT_BAND_W   50      /* "at the limit" = within this of the target */
#define STALE_REBOOT_S    (30 * 60) /* restart after this long without Shelly data ... */
static bool s_had_fresh;           /* ... but only if we ever had data since boot */

/* Alert state (monitor task only). */
static struct {
    int64_t meter_bad_since;  bool meter_alerted;
    int     over_s, under_s;  bool export_alerted;
    int64_t wifi_down_since;  time_t wifi_down_wall;
    bool    inv_alerted;
} A;

/* Statistics for the daily report, reset after each report. */
static struct {
    float    peak_imp, peak_exp;
    uint32_t at_limit_s, stale_s, failsafe_s, alerts;
    time_t   inv_first, inv_last;
    time_t   since;          /* start of the stats window (0 = boot, clock not set yet) */
} S;

/* Energy baseline (Shelly counters at the last report), persisted. */
static struct {
    uint32_t imp_wh, exp_wh;
    int32_t  last_day;       /* yyyymmdd of the last report */
    bool     loaded, have_base;
} B;

static void fmt_dur(char *out, size_t n, int64_t s) {
    if (s < 0) s = 0;
    int d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d)      snprintf(out, n, "%dd %dh", d, h);
    else if (h) snprintf(out, n, "%dh %02dm", h, m);
    else if (m) snprintf(out, n, "%d min", m);
    else        snprintf(out, n, "%d s", (int)s);
}

static void fmt_hm(char *out, size_t n, time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%H:%M", &tm);
}

static void alert(bool enabled, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void alert(bool enabled, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", buf);
    S.alerts++;
    if (enabled) telegram_send(buf);
}

/* ---- persistence of the energy baseline ---- */

static void baseline_load(void) {
    nvs_handle_t h;
    B.loaded = true;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    B.have_base = nvs_get_u32(h, "imp_wh", &B.imp_wh) == ESP_OK &&
                  nvs_get_u32(h, "exp_wh", &B.exp_wh) == ESP_OK;
    nvs_get_i32(h, "last_day", &B.last_day);
    nvs_close(h);
}

static void baseline_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "imp_wh", B.imp_wh);
    nvs_set_u32(h, "exp_wh", B.exp_wh);
    nvs_set_i32(h, "last_day", B.last_day);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- daily report ---- */

static int32_t yyyymmdd(const struct tm *tm) {
    return (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}

static void send_report(const vem_t *v, const struct tm *now_tm) {
    char buf[900], t1[16], t2[16], d1[24], d2[24];
    int n = 0;
    char date[16];
    strftime(date, sizeof(date), "%d.%m.%Y", now_tm);
    n += snprintf(buf + n, sizeof(buf) - n, "Growatt vEM daily report %s\n\n", date);

    uint32_t imp = (uint32_t)(v->import_kwh * 1000.0f), exp = (uint32_t)(v->export_kwh * 1000.0f);
    if (B.have_base && imp >= B.imp_wh && exp >= B.exp_wh && v->state != SHELLY_NONE) {
        n += snprintf(buf + n, sizeof(buf) - n, "Grid import: %.2f kWh\nGrid export: %.2f kWh\n",
                      (imp - B.imp_wh) / 1000.0, (exp - B.exp_wh) / 1000.0);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "Grid import/export: n/a (no baseline yet)\n");
    }
    n += snprintf(buf + n, sizeof(buf) - n, "Peak import: %.0f W, peak export: %.0f W\n",
                  S.peak_imp, S.peak_exp);
    fmt_dur(d1, sizeof(d1), S.at_limit_s);
    fmt_dur(d2, sizeof(d2), S.stale_s);
    n += snprintf(buf + n, sizeof(buf) - n, "At the export limit: %s\nNo fresh meter data: %s\n", d1, d2);
    if (S.inv_first) {
        fmt_hm(t1, sizeof(t1), S.inv_first);
        fmt_hm(t2, sizeof(t2), S.inv_last);
        n += snprintf(buf + n, sizeof(buf) - n, "Inverter active: %s - %s\n", t1, t2);
    } else {
        n += snprintf(buf + n, sizeof(buf) - n, "Inverter: did not poll the meter\n");
    }
    fmt_dur(d1, sizeof(d1), esp_timer_get_time() / 1000000);
    n += snprintf(buf + n, sizeof(buf) - n, "Alerts: %u\nUptime: %s", (unsigned)S.alerts, d1);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) n += snprintf(buf + n, sizeof(buf) - n, ", WiFi %d dBm", ap.rssi);
    if (!S.since) n += snprintf(buf + n, sizeof(buf) - n, "\n(peaks and times since the last restart)");

    telegram_send(buf);
    ESP_LOGI(TAG, "daily report queued");

    /* new window */
    if (v->state != SHELLY_NONE && v->import_kwh > 0) {
        B.imp_wh = imp; B.exp_wh = exp; B.have_base = true;
    }
    B.last_day = yyyymmdd(now_tm);
    baseline_save();
    memset(&S, 0, sizeof(S));
    S.since = time(NULL);
}

static bool parse_hhmm(const char *s, int *h, int *m) {
    return sscanf(s, "%d:%d", h, m) == 2 && *h >= 0 && *h < 24 && *m >= 0 && *m < 60;
}

/* ---- the 1 s check ---- */

static void tick(void) {
    settings_t cfg;
    settings_copy(&cfg);
    vem_t v;
    vem_compute(&v);
    int64_t now_us = esp_timer_get_time();
    time_t now = time(NULL);
    bool tg = telegram_enabled();
    int hold_s = cfg.tg_alert_min * 60;
    char d[24], t[16];

    /* statistics */
    if (v.state == SHELLY_FRESH) {
        float g = v.snap.total_power;
        if (g > S.peak_imp) S.peak_imp = g;
        if (-g > S.peak_exp) S.peak_exp = -g;
        int target = cfg.lim_export_w - cfg.lim_margin_w;
        if (v.lim.state == LIM_STATE_ACTIVE && -g >= target - AT_LIMIT_BAND_W) S.at_limit_s++;
    } else {
        S.stale_s++;
    }
    if (v.lim.state == LIM_STATE_FAILSAFE) S.failsafe_s++;
    int64_t poll_ms = modbus_last_request_age_ms();
    bool polling = poll_ms >= 0 && poll_ms < 10000;
    if (polling && clock_valid()) {
        if (!S.inv_first) S.inv_first = now;
        S.inv_last = now;
    }

    /* meter data lost */
    if (v.state != SHELLY_FRESH) {
        if (!A.meter_bad_since) A.meter_bad_since = now_us;
        if (!A.meter_alerted && now_us - A.meter_bad_since >= (int64_t)hold_s * 1000000) {
            char url[96];
            int st = shelly_diag(url, sizeof(url));
            fmt_dur(d, sizeof(d), (now_us - A.meter_bad_since) / 1000000);
            alert(tg && cfg.tg_al_meter, "Growatt vEM: no fresh data from the Shelly 3EM for %s "
                  "(last error %d). The inverter is %s.", d, st,
                  cfg.lim_failsafe == LIM_FAILSAFE_SILENT ? "not answered" : "told to throttle");
            A.meter_alerted = true;
        }
        /* Self-heal a stuck network stack. A Shelly that is really offline
         * never gives fresh data after the restart, so this cannot loop. */
        if (s_had_fresh && wifi_is_connected() &&
            now_us - A.meter_bad_since >= STALE_REBOOT_S * 1000000LL) {
            ESP_LOGE(TAG, "no Shelly data for %d min, restarting", STALE_REBOOT_S / 60);
            telegram_send("Growatt vEM: no Shelly data for 30 min although WiFi is up, restarting.");
            vTaskDelay(pdMS_TO_TICKS(8000));   /* let the message go out */
            esp_restart();
        }
    } else {
        s_had_fresh = true;
        if (A.meter_alerted) {
            fmt_dur(d, sizeof(d), (now_us - A.meter_bad_since) / 1000000);
            alert(tg && cfg.tg_al_meter, "Growatt vEM: Shelly data is back after %s.", d);
        }
        A.meter_bad_since = 0;
        A.meter_alerted = false;
    }

    /* export above the limit while limiting */
    bool over = v.state == SHELLY_FRESH && v.lim.state == LIM_STATE_ACTIVE &&
                -v.snap.total_power > cfg.lim_export_w + EXPORT_TOL_W;
    if (over) { A.over_s++; A.under_s = 0; } else { A.under_s++; if (A.under_s >= EXPORT_HOLD_S) A.over_s = 0; }
    if (!A.export_alerted && A.over_s >= EXPORT_HOLD_S) {
        alert(tg && cfg.tg_al_export, "Growatt vEM: exporting %.0f W, above the %d W limit for over %d s. "
              "Check the inverter's export limit setting (0 W) and the meter emulation.",
              -v.snap.total_power, (int)cfg.lim_export_w, EXPORT_HOLD_S);
        A.export_alerted = true;
    } else if (A.export_alerted && A.under_s >= EXPORT_HOLD_S) {
        alert(tg && cfg.tg_al_export, "Growatt vEM: export is back below the limit.");
        A.export_alerted = false;
    }

    /* WiFi outage: reported once we are back online */
    if (!wifi_is_connected()) {
        if (!A.wifi_down_since) { A.wifi_down_since = now_us; A.wifi_down_wall = now; }
    } else if (A.wifi_down_since) {
        int64_t dur = (now_us - A.wifi_down_since) / 1000000;
        if (dur >= hold_s) {
            fmt_dur(d, sizeof(d), dur);
            fmt_hm(t, sizeof(t), A.wifi_down_wall);
            alert(tg && cfg.tg_al_wifi, "Growatt vEM: WiFi was down for %s (from %s).", d,
                  clock_valid() ? t : "?");
        }
        A.wifi_down_since = 0;
    }

    /* inverter not polling, only inside the daytime window (it sleeps without PV) */
    if (cfg.tg_al_inv && clock_valid() && cfg.mb_mode != MODBUS_MODE_OFF) {
        struct tm tm;
        localtime_r(&now, &tm);
        bool day = tm.tm_hour >= cfg.tg_inv_from && tm.tm_hour < cfg.tg_inv_to;
        bool silent = poll_ms < 0 || poll_ms >= (int64_t)hold_s * 1000;
        if (day && silent && !A.inv_alerted) {
            if (poll_ms < 0) strlcpy(d, "since the restart", sizeof(d));
            else fmt_dur(d, sizeof(d), poll_ms / 1000);
            alert(tg, "Growatt vEM: the inverter has not polled the meter for %s (daytime window %02d-%02d h).",
                  d, (int)cfg.tg_inv_from, (int)cfg.tg_inv_to);
            A.inv_alerted = true;
        } else if (A.inv_alerted && polling) {
            alert(tg, "Growatt vEM: the inverter is polling again.");
            A.inv_alerted = false;
        }
    }

    /* energy baseline on the first good reading */
    if (!B.have_base && v.state == SHELLY_FRESH && v.import_kwh > 0) {
        B.imp_wh = (uint32_t)(v.import_kwh * 1000.0f);
        B.exp_wh = (uint32_t)(v.export_kwh * 1000.0f);
        B.have_base = true;
        baseline_save();
    }

    /* daily report */
    int rh, rm;
    if (cfg.tg_report && clock_valid() && parse_hhmm(cfg.tg_report_at, &rh, &rm)) {
        struct tm tm;
        localtime_r(&now, &tm);
        bool due = tm.tm_hour * 60 + tm.tm_min >= rh * 60 + rm;
        if (due && B.last_day != yyyymmdd(&tm)) {
            /* first run ever, after today's report time: start with tomorrow */
            if (B.last_day == 0) { B.last_day = yyyymmdd(&tm); baseline_save(); S.since = now; }
            else if (tg) send_report(&v, &tm);
            else { B.last_day = yyyymmdd(&tm); baseline_save(); }
        }
    }
}

static const char *unexpected_reset(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_PANIC:    return "a crash (panic)";
    case ESP_RST_INT_WDT:  return "an interrupt watchdog reset";
    case ESP_RST_TASK_WDT: return "a task watchdog reset (a task hung)";
    case ESP_RST_WDT:      return "a watchdog reset";
    case ESP_RST_BROWNOUT: return "a brownout (supply voltage dropped)";
    default:               return NULL;
    }
}

static void monitor_task(void *arg) {
    baseline_load();
    settings_t cfg;
    settings_copy(&cfg);
    const char *why = unexpected_reset(esp_reset_reason());
    if (why) alert(telegram_enabled() && cfg.tg_al_reboot, "Growatt vEM restarted after %s. Firmware %s.",
                   why, esp_app_get_description()->version);

    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        tick();
    }
}

void monitor_init(void) {
    xTaskCreate(monitor_task, "monitor", 6144, NULL, 3, NULL);
}
