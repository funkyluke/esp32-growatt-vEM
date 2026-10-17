#pragma once

#include <stdbool.h>

/* MQTT publishing with Home Assistant discovery. Topics (base = setting):
 *   <base>/status        online / offline (retained, last will)
 *   <base>/state         JSON with all values (retained)
 *   <base>/limit/set     export limit in W (command)
 *   <base>/limiter/set   ON / OFF (command)
 * Discovery configs go to <discovery prefix>/<component>/<node>/<obj>/config. */
void mqtt_ha_init(void);
bool mqtt_ha_connected(void);
