#include <math.h>
#include <string.h>
#include "settings.h"
#include "vem.h"

void vem_compute(vem_t *o) {
    memset(o, 0, sizeof(*o));
    shelly_get_snapshot(&o->snap);
    o->state = shelly_state(&o->snap, &o->age_ms);
    settings_t cfg;
    settings_copy(&cfg);

    float v_sum = 0, imp = 0, exp = 0;
    int v_n = 0;
    for (int i = 0; i < 3; i++) {
        const shelly_phase_t *p = &o->snap.phase[i];
        if (p->voltage > 50) { v_sum += p->voltage; v_n++; }
        imp += p->total_wh;
        exp += p->total_returned_wh;
    }
    o->import_kwh = imp / 1000.0f;
    o->export_kwh = exp / 1000.0f;

    limiter_in_t in = {
        .data_fresh     = (o->state == SHELLY_FRESH),
        .grid_w         = o->snap.total_power,
        .enabled        = cfg.lim_enable != 0,
        .export_limit_w = cfg.lim_export_w,
        .margin_w       = cfg.lim_margin_w,
        .failsafe       = (limiter_failsafe_t)cfg.lim_failsafe,
    };
    limiter_compute(&in, &o->lim);

    float p = o->lim.reported_w * (cfg.mb_invert ? -1.0f : 1.0f);
    float v = v_n ? v_sum / v_n : 230.0f;
    o->mb = (mb_values_t){
        .voltage      = v,
        .current      = p / v,
        .power_w      = p,
        .apparent_va  = fabsf(p),
        .reactive_var = 0,
        .pf           = 1.0f,
        .freq_hz      = 50.0f,
        .import_kwh   = o->import_kwh,
        .export_kwh   = o->export_kwh,
    };
}
