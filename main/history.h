#pragma once

/* Power history in PSRAM for the live graph: 1 s samples for 1 hour and
 * 1 min averages (with min/max) for 7 days. Lost on reboot.
 * GET /api/history?res=s|m&since=<seq> (call after web_start). */

#define HIST_F_STALE     0x01   /* Shelly data not fresh */
#define HIST_F_LIMITING  0x02   /* export limiter active */
#define HIST_F_FAILSAFE  0x04   /* inverter told to throttle / not answered */
#define HIST_F_INVERTER  0x08   /* inverter polled in the last 10 s */

void history_init(void);
