#pragma once

/* The virtual energy meter's current view: Shelly data + freshness +
 * export limiter + the values the emulated meter reports. One function so
 * that Modbus, the web UI, history and MQTT all agree. */

#include "shelly.h"
#include "limiter.h"
#include "mb_proto.h"

typedef struct {
    shelly_snapshot_t snap;
    shelly_state_t    state;
    int64_t           age_ms;
    limiter_out_t     lim;
    mb_values_t       mb;        /* what Modbus answers (sign inversion applied) */
    float             import_kwh;
    float             export_kwh;
} vem_t;

void vem_compute(vem_t *o);
