#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    float power;             /* W, negative = export */
    float current;           /* A */
    float voltage;           /* V */
    float total_wh;          /* imported energy counter, Wh (0 if unknown) */
    float total_returned_wh; /* exported energy counter, Wh (0 if unknown) */
} shelly_phase_t;

typedef struct {
    shelly_phase_t phase[3];
    float total_power;
    bool  valid;
    int64_t last_ok_us;   /* esp_timer time of the last successful poll */
} shelly_snapshot_t;

typedef enum { SHELLY_NONE, SHELLY_FRESH, SHELLY_AGING, SHELLY_STALE } shelly_state_t;

void shelly_init(void);
void shelly_get_snapshot(shelly_snapshot_t *out);

/* Called when the Shelly settings change: forget the address and readings. */
void shelly_reresolve(void);

/* Diagnostics of the last failed poll: returns the HTTP status or error code
 * (0 = none) and copies the URL into `url` if non-NULL. */
int shelly_diag(char *url, size_t n);

/* Freshness of a snapshot, from the age of its last good poll (fresh: below
 * the data timeout setting, aging: below 3 times that). `age_ms` gets -1 if
 * there was no poll yet. */
shelly_state_t shelly_state(const shelly_snapshot_t *s, int64_t *age_ms);
const char *shelly_state_name(shelly_state_t st);
