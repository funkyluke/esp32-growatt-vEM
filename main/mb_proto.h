#pragma once

/* Modbus RTU slave protocol and meter register maps. Pure C (no ESP-IDF), so
 * it can be unit-tested on the host (see test/host). */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    MB_METER_SDM120 = 0,   /* Eastron SDM120-Modbus, FC 0x04 input registers */
    MB_METER_DDSU666,      /* Chint DDSU666, FC 0x03 holding registers */
    MB_METER_N
} mb_meter_t;

/* What the emulated meter reports. Power sign: + import, - export. */
typedef struct {
    float voltage;       /* V */
    float current;       /* A (signed like power) */
    float power_w;       /* W */
    float apparent_va;   /* VA */
    float reactive_var;  /* var */
    float pf;
    float freq_hz;
    float import_kwh;
    float export_kwh;
} mb_values_t;

typedef enum {
    MB_R_REPLY,          /* *resp_len bytes in resp should be sent */
    MB_R_NOT_FOR_US,     /* valid frame for another slave address */
    MB_R_BROADCAST,      /* address 0: handled, never answered */
    MB_R_BAD_CRC,
    MB_R_BAD_FRAME,      /* too short or inconsistent length */
} mb_result_t;

typedef struct {
    uint8_t  addr;
    uint8_t  fc;
    uint16_t start;
    uint16_t count;
    uint8_t  exception;  /* 0 = none, else the Modbus exception code sent */
    bool     unmapped;   /* the read touched registers this meter does not define */
} mb_req_info_t;

uint16_t mb_crc16(const uint8_t *buf, size_t len);

/* Handles one complete RTU frame. `resp` must hold at least 256 bytes.
 * `info` (optional) describes the request for logging. */
mb_result_t mb_process(const uint8_t *req, size_t len, uint8_t my_addr, mb_meter_t meter,
                       const mb_values_t *v, uint8_t *resp, size_t *resp_len,
                       mb_req_info_t *info);

const char *mb_meter_name(mb_meter_t m);
