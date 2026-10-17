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
    int target = in->export_limit_w - in->margin_w;
    if (target < 0) target = 0;
    out->state = LIM_STATE_ACTIVE;
    out->offset_w = (float)target;
    out->reported_w = in->grid_w + (float)target;
}

const char *limiter_state_name(limiter_state_t s) {
    switch (s) {
    case LIM_STATE_ACTIVE:   return "active";
    case LIM_STATE_BYPASS:   return "bypass";
    case LIM_STATE_FAILSAFE: return "failsafe";
    }
    return "?";
}
