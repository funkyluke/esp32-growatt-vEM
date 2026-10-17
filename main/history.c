#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "settings.h"
#include "modbus.h"
#include "vem.h"
#include "web.h"
#include "history.h"

static const char *TAG = "history";

/* 1 s samples for 1 h, 1 min aggregates for 7 days (RAM only). */
#define FINE_N    3600
#define COARSE_N  (7 * 24 * 60)

typedef struct {
    int16_t grid, l1, l2, l3, rep, lim;
    uint8_t flags;
} fine_t;

typedef struct {
    int16_t grid, gmin, gmax, l1, l2, l3, rep, lim;
    uint8_t flags;           /* OR over the minute */
} coarse_t;

static fine_t   *s_fine;
static coarse_t *s_coarse;
static uint32_t  s_fine_seq;     /* number of fine samples ever written */
static uint32_t  s_coarse_seq;
static SemaphoreHandle_t s_mutex;

/* minute accumulator (sampler task only) */
static struct {
    float grid, l1, l2, l3, rep, lim, gmin, gmax;
    uint8_t flags;
    int n;
} s_acc;

static int16_t c16(float f) {
    if (f > 32767) return 32767;
    if (f < -32768) return -32768;
    return (int16_t)lrintf(f);
}

static void sample(void) {
    vem_t v;
    vem_compute(&v);
    settings_t cfg;
    settings_copy(&cfg);

    uint8_t fl = 0;
    if (v.state != SHELLY_FRESH)              fl |= HIST_F_STALE;
    if (v.lim.state == LIM_STATE_ACTIVE)      fl |= HIST_F_LIMITING;
    if (v.lim.state == LIM_STATE_FAILSAFE)    fl |= HIST_F_FAILSAFE;
    int64_t poll = modbus_last_request_age_ms();
    if (poll >= 0 && poll < 10000)            fl |= HIST_F_INVERTER;

    bool have = v.state != SHELLY_NONE;
    fine_t f = {
        .grid = have ? c16(v.snap.total_power) : 0,
        .l1 = have ? c16(v.snap.phase[0].power) : 0,
        .l2 = have ? c16(v.snap.phase[1].power) : 0,
        .l3 = have ? c16(v.snap.phase[2].power) : 0,
        .rep = c16(v.lim.reported_w),
        .lim = (int16_t)(cfg.lim_enable ? cfg.lim_export_w : 0),
        .flags = fl,
    };

    if (s_acc.n == 0) { s_acc.gmin = f.grid; s_acc.gmax = f.grid; }
    s_acc.grid += f.grid; s_acc.l1 += f.l1; s_acc.l2 += f.l2; s_acc.l3 += f.l3;
    s_acc.rep += f.rep; s_acc.lim += f.lim;
    if (f.grid < s_acc.gmin) s_acc.gmin = f.grid;
    if (f.grid > s_acc.gmax) s_acc.gmax = f.grid;
    s_acc.flags |= fl;
    s_acc.n++;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_fine[s_fine_seq % FINE_N] = f;
    s_fine_seq++;
    if (s_acc.n >= 60) {
        float n = (float)s_acc.n;
        coarse_t c = {
            .grid = c16(s_acc.grid / n), .gmin = c16(s_acc.gmin), .gmax = c16(s_acc.gmax),
            .l1 = c16(s_acc.l1 / n), .l2 = c16(s_acc.l2 / n), .l3 = c16(s_acc.l3 / n),
            .rep = c16(s_acc.rep / n), .lim = c16(s_acc.lim / n), .flags = s_acc.flags,
        };
        s_coarse[s_coarse_seq % COARSE_N] = c;
        s_coarse_seq++;
        memset(&s_acc, 0, sizeof(s_acc));
    }
    xSemaphoreGive(s_mutex);
}

static void sampler_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));
        sample();
    }
}

/* ---- streaming JSON writer ---- */

typedef struct {
    httpd_req_t *req;
    char buf[1024];
    size_t n;
    esp_err_t err;
} out_t;

static void out_flush(out_t *o) {
    if (o->n && o->err == ESP_OK) o->err = httpd_resp_send_chunk(o->req, o->buf, o->n);
    o->n = 0;
}

static void out_str(out_t *o, const char *s) {
    size_t l = strlen(s);
    if (o->n + l > sizeof(o->buf)) out_flush(o);
    memcpy(o->buf + o->n, s, l);
    o->n += l;
}

static void out_int(out_t *o, int v, bool comma) {
    char t[12];
    snprintf(t, sizeof(t), comma ? ",%d" : "%d", v);
    out_str(o, t);
}

/* Column extractors over a copied window. */
#define FINE_COL(name, field) \
    static int name(const void *p) { return ((const fine_t *)p)->field; }
#define COARSE_COL(name, field) \
    static int name(const void *p) { return ((const coarse_t *)p)->field; }
FINE_COL(f_grid, grid) FINE_COL(f_l1, l1) FINE_COL(f_l2, l2) FINE_COL(f_l3, l3)
FINE_COL(f_rep, rep) FINE_COL(f_lim, lim) FINE_COL(f_fl, flags)
COARSE_COL(c_grid, grid) COARSE_COL(c_gmin, gmin) COARSE_COL(c_gmax, gmax)
COARSE_COL(c_l1, l1) COARSE_COL(c_l2, l2) COARSE_COL(c_l3, l3)
COARSE_COL(c_rep, rep) COARSE_COL(c_lim, lim) COARSE_COL(c_fl, flags)

typedef struct { const char *name; int (*get)(const void *); } col_t;

static const col_t FINE_COLS[] = {
    { "grid", f_grid }, { "l1", f_l1 }, { "l2", f_l2 }, { "l3", f_l3 },
    { "rep", f_rep }, { "lim", f_lim }, { "fl", f_fl },
};
static const col_t COARSE_COLS[] = {
    { "grid", c_grid }, { "gmin", c_gmin }, { "gmax", c_gmax }, { "l1", c_l1 }, { "l2", c_l2 },
    { "l3", c_l3 }, { "rep", c_rep }, { "lim", c_lim }, { "fl", c_fl },
};

/* GET /api/history?res=s|m&since=<seq>
 * Returns the samples with sequence number >= since (all kept ones if 0),
 * as columns. "next" is the value to pass as `since` next time; the newest
 * sample is next-1 and is less than one step old. */
static esp_err_t history_handler(httpd_req_t *req) {
    char q[64] = "", v[16];
    httpd_req_get_url_query_str(req, q, sizeof(q));
    bool coarse = httpd_query_key_value(q, "res", v, sizeof(v)) == ESP_OK && v[0] == 'm';
    uint32_t since = 0;
    if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) since = strtoul(v, NULL, 10);

    const size_t cap = coarse ? COARSE_N : FINE_N;
    const size_t esz = coarse ? sizeof(coarse_t) : sizeof(fine_t);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t next = coarse ? s_coarse_seq : s_fine_seq;
    uint32_t first = next > cap ? next - cap : 0;
    if (since > first) first = since;
    if (first > next) first = next;
    size_t n = next - first;
    uint8_t *win = n ? heap_caps_malloc(n * esz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (n && !win) n = 0;
    const uint8_t *ring = coarse ? (const uint8_t *)s_coarse : (const uint8_t *)s_fine;
    for (size_t i = 0; i < n; i++) memcpy(win + i * esz, ring + ((first + i) % cap) * esz, esz);
    xSemaphoreGive(s_mutex);
    if (n == 0) first = next;

    httpd_resp_set_type(req, "application/json");
    out_t *o = malloc(sizeof(out_t));
    if (!o) { free(win); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); }
    o->req = req; o->n = 0; o->err = ESP_OK;

    char head[96];
    snprintf(head, sizeof(head), "{\"step\":%d,\"first\":%lu,\"next\":%lu",
             coarse ? 60 : 1, (unsigned long)first, (unsigned long)next);
    out_str(o, head);
    const col_t *cols = coarse ? COARSE_COLS : FINE_COLS;
    size_t ncols = coarse ? sizeof(COARSE_COLS) / sizeof(col_t) : sizeof(FINE_COLS) / sizeof(col_t);
    for (size_t c = 0; c < ncols && o->err == ESP_OK; c++) {
        out_str(o, ",\"");
        out_str(o, cols[c].name);
        out_str(o, "\":[");
        for (size_t i = 0; i < n; i++) out_int(o, cols[c].get(win + i * esz), i > 0);
        out_str(o, "]");
    }
    out_str(o, "}");
    out_flush(o);
    esp_err_t err = o->err;
    free(o);
    free(win);
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

void history_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    s_fine = heap_caps_calloc(FINE_N, sizeof(fine_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_coarse = heap_caps_calloc(COARSE_N, sizeof(coarse_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fine || !s_coarse) { ESP_LOGE(TAG, "no memory for history"); return; }
    ESP_LOGI(TAG, "%u + %u bytes in PSRAM", (unsigned)(FINE_N * sizeof(fine_t)),
             (unsigned)(COARSE_N * sizeof(coarse_t)));
    xTaskCreate(sampler_task, "history", 4096, NULL, 4, NULL);

    static const httpd_uri_t api = { .uri = "/api/history", .method = HTTP_GET, .handler = history_handler };
    web_register(&api);
}
