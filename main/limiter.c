#include "limiter.h"

void limiter_compute(const limiter_in_t *in, limiter_out_t *out) {
    out->answer = true;
    out->offset_w = 0;

    if (!in->data_fresh) {
        out->state = LIM_STATE_FAILSAFE;
        out->reported_w = LIMITER_FAILSAFE_W;
        out->answer = (in->failsafe != LIM_FAILSAFE_SILENT);
        return;
    }
    if (!in->enabled) {
        out->state = LIM_STATE_BYPASS;
        out->reported_w = in->grid_w;
        return;
    }
    /* Negative when the margin exceeds the limit: regulate to a small import. */
    int target = in->export_limit_w - in->margin_w;
    out->state = LIM_STATE_ACTIVE;
    out->offset_w = (float)target;
    out->reported_w = in->grid_w + (float)target;
    /* A Growatt that sees more than a small export for a few seconds stops
     * feeding in ("backflow timeout") and ramps up again over minutes. With
     * a capped value it backs off gently instead. */
    if (in->max_report_export_w > 0 && out->reported_w < -in->max_report_export_w)
        out->reported_w = (float)-in->max_report_export_w;
}

const char *limiter_state_name(limiter_state_t s) {
    switch (s) {
    case LIM_STATE_ACTIVE:   return "active";
    case LIM_STATE_BYPASS:   return "bypass";
    case LIM_STATE_FAILSAFE: return "failsafe";
    }
    return "?";
}
