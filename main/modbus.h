#pragma once

#include <stdint.h>

/* Modbus RTU slave on the RS485 port, emulating the meter the inverter
 * expects (see mb_proto.c for the register maps). */

typedef enum {
    MODBUS_MODE_OFF = 0,
    MODBUS_MODE_EMULATE = 1,   /* answer the inverter */
    MODBUS_MODE_SNIFF = 2,     /* listen only: log every frame, never transmit */
} modbus_mode_t;

typedef struct {
    uint32_t requests;         /* valid frames for our address (or broadcast) */
    uint32_t replies;
    uint32_t crc_errors;
    uint32_t bad_frames;
    uint32_t line_errors;
    uint32_t other_addr;
    uint32_t unmapped;         /* reads touching registers the meter does not define */
    uint32_t exceptions;
    int64_t  last_request_us;  /* 0 = never */
    uint8_t  last_fc;
    uint16_t last_start, last_count;
} modbus_stats_t;

/* Call after web_start (registers GET /api/modbus). */
void modbus_init(void);

void modbus_get_stats(modbus_stats_t *out);

/* Time since the inverter last polled us; -1 if it never did. */
int64_t modbus_last_request_age_ms(void);
