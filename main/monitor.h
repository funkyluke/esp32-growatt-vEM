#pragma once

/* Watches the system once a second: Telegram alerts (meter data lost,
 * export above the limit, unexpected reboot, WiFi outage, optionally the
 * inverter not polling in a daytime window) and the daily report. */
void monitor_init(void);
