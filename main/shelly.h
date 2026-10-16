#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float power;     /* W */
    float current;   /* A */
    float voltage;   /* V */
} shelly_phase_t;

typedef struct {
    shelly_phase_t phase[3];
    float total_power;
    bool  valid;
    int64_t last_ok_us;   /* esp_timer time of the last successful poll */
} shelly_snapshot_t;

void shelly_init(void);
void shelly_get_snapshot(shelly_snapshot_t *out);

/* Called when the Shelly settings change: forget the address and readings. */
void shelly_reresolve(void);

/* Diagnostics of the last failed poll (for /api/power). */
int         shelly_diag_status(void);
const char *shelly_diag_url(void);
