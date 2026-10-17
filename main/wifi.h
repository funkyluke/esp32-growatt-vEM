#pragma once

#include "esp_netif.h"
#include "settings.h"

void wifi_init_sta(void);

/* The device's own IPv4 address (set once DHCP completes). */
const esp_ip4_addr_t *wifi_ip(void);

/* True while the station has an IP address. */
bool wifi_is_connected(void);

/* True while a WiFi change is being applied in the background. */
bool wifi_apply_busy(void);

/* Switch to the current settings' WiFi credentials. `old_cfg` is the config to
 * revert to if the new network cannot be joined within 30 s. */
void wifi_apply(const settings_t *old_cfg, uint32_t wifi_mask);
