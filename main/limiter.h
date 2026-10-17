#pragma once

/* Export limiter: turns the measured grid power into the power reported to
 * the inverter. Pure C, unit-tested on the host (test/host).
 *
 * The inverter's own export limit is set to 0 W. Reporting
 *     reported = grid + (limit - margin)
 * makes the inverter regulate `reported` to 0, i.e. grid = -(limit - margin):
 * at most `limit` W are exported. Power sign: + import, - export. */

#include <stdbool.h>

/* Reported in fail-safe "throttle": a large export, so the inverter backs off. */
#define LIMITER_FAILSAFE_W  (-5000.0f)

typedef enum {
    LIM_FAILSAFE_THROTTLE = 0,   /* report LIMITER_FAILSAFE_W */
    LIM_FAILSAFE_SILENT   = 1,   /* stop answering; the inverter's meter-loss logic takes over */
} limiter_failsafe_t;

typedef enum {
    LIM_STATE_ACTIVE,     /* limiting, offset applied */
    LIM_STATE_BYPASS,     /* limiter off: real grid power is reported */
    LIM_STATE_FAILSAFE,   /* no fresh data */
} limiter_state_t;

typedef struct {
    bool  data_fresh;
    float grid_w;
    bool  enabled;
    int   export_limit_w;
    int   margin_w;
    limiter_failsafe_t failsafe;
} limiter_in_t;

typedef struct {
    limiter_state_t state;
    bool  answer;         /* false: do not reply on Modbus */
    float reported_w;
    float offset_w;       /* applied offset (0 when not active) */
} limiter_out_t;

void limiter_compute(const limiter_in_t *in, limiter_out_t *out);
const char *limiter_state_name(limiter_state_t s);
