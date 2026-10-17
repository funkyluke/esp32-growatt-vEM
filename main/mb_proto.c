#include <string.h>
#include "mb_proto.h"

/* Quantities a register pair can hold (all encoded as IEEE754 float32,
 * big-endian, high word first, like the real meters). */
typedef enum {
    Q_VOLT, Q_CURR, Q_P_W, Q_P_KW, Q_S_VA, Q_S_KVA, Q_Q_VAR, Q_Q_KVAR,
    Q_PF, Q_PHASE_DEG, Q_FREQ, Q_IMP_KWH, Q_EXP_KWH, Q_TOT_KWH, Q_ZERO,
} quantity_t;

typedef struct {
    uint16_t   reg;      /* first of the two registers */
    quantity_t q;
} reg_def_t;

typedef struct {
    const char      *name;
    uint8_t          fc;        /* function code for measurements */
    const reg_def_t *regs;
    size_t           n;
} meter_def_t;

/* Eastron SDM120-Modbus, input registers (FC 0x04). */
static const reg_def_t SDM120_REGS[] = {
    { 0x0000, Q_VOLT },      { 0x0006, Q_CURR },      { 0x000C, Q_P_W },
    { 0x0012, Q_S_VA },      { 0x0018, Q_Q_VAR },     { 0x001E, Q_PF },
    { 0x0024, Q_PHASE_DEG }, { 0x0046, Q_FREQ },      { 0x0048, Q_IMP_KWH },
    { 0x004A, Q_EXP_KWH },   { 0x004C, Q_ZERO },      { 0x004E, Q_ZERO },
    { 0x0156, Q_TOT_KWH },   { 0x0158, Q_ZERO },
};

/* Chint DDSU666, holding registers (FC 0x03); power in kW. */
static const reg_def_t DDSU666_REGS[] = {
    { 0x2000, Q_VOLT },   { 0x2002, Q_CURR },   { 0x2004, Q_P_KW },
    { 0x2006, Q_Q_KVAR }, { 0x2008, Q_S_KVA },  { 0x200A, Q_PF },
    { 0x200E, Q_FREQ },   { 0x4000, Q_IMP_KWH },{ 0x400A, Q_EXP_KWH },
};

static const meter_def_t METERS[MB_METER_N] = {
    [MB_METER_SDM120]  = { "Eastron SDM120", 0x04, SDM120_REGS,  sizeof(SDM120_REGS) / sizeof(SDM120_REGS[0]) },
    [MB_METER_DDSU666] = { "Chint DDSU666",  0x03, DDSU666_REGS, sizeof(DDSU666_REGS) / sizeof(DDSU666_REGS[0]) },
};

const char *mb_meter_name(mb_meter_t m) { return m < MB_METER_N ? METERS[m].name : "?"; }

uint16_t mb_crc16(const uint8_t *buf, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static float quantity(quantity_t q, const mb_values_t *v) {
    switch (q) {
    case Q_VOLT:      return v->voltage;
    case Q_CURR:      return v->current;
    case Q_P_W:       return v->power_w;
    case Q_P_KW:      return v->power_w / 1000.0f;
    case Q_S_VA:      return v->apparent_va;
    case Q_S_KVA:     return v->apparent_va / 1000.0f;
    case Q_Q_VAR:     return v->reactive_var;
    case Q_Q_KVAR:    return v->reactive_var / 1000.0f;
    case Q_PF:        return v->pf;
    case Q_FREQ:      return v->freq_hz;
    case Q_IMP_KWH:   return v->import_kwh;
    case Q_EXP_KWH:   return v->export_kwh;
    case Q_TOT_KWH:   return v->import_kwh + v->export_kwh;
    case Q_PHASE_DEG:
    case Q_ZERO:
    default:          return 0.0f;
    }
}

/* Value of one 16-bit register; false if the meter does not define it. */
static bool reg_value(const meter_def_t *m, uint16_t reg, const mb_values_t *v, uint16_t *out) {
    for (size_t i = 0; i < m->n; i++) {
        if (reg != m->regs[i].reg && reg != m->regs[i].reg + 1) continue;
        float f = quantity(m->regs[i].q, v);
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        *out = (reg == m->regs[i].reg) ? (uint16_t)(u >> 16) : (uint16_t)u;
        return true;
    }
    *out = 0;
    return false;
}

static size_t finish(uint8_t *resp, size_t n) {
    uint16_t crc = mb_crc16(resp, n);
    resp[n++] = crc & 0xFF;     /* CRC is sent low byte first */
    resp[n++] = crc >> 8;
    return n;
}

static size_t exception(uint8_t *resp, uint8_t addr, uint8_t fc, uint8_t code, mb_req_info_t *info) {
    if (info) info->exception = code;
    resp[0] = addr;
    resp[1] = fc | 0x80;
    resp[2] = code;
    return finish(resp, 3);
}

mb_result_t mb_process(const uint8_t *req, size_t len, uint8_t my_addr, mb_meter_t meter,
                       const mb_values_t *v, uint8_t *resp, size_t *resp_len,
                       mb_req_info_t *info) {
    mb_req_info_t dummy;
    if (!info) info = &dummy;
    memset(info, 0, sizeof(*info));
    *resp_len = 0;

    if (len < 4) return MB_R_BAD_FRAME;
    uint16_t crc = mb_crc16(req, len - 2);
    if (req[len - 2] != (crc & 0xFF) || req[len - 1] != (crc >> 8)) return MB_R_BAD_CRC;

    uint8_t addr = req[0], fc = req[1];
    info->addr = addr;
    info->fc = fc;
    if (len >= 6) {
        info->start = (uint16_t)(req[2] << 8 | req[3]);
        info->count = (uint16_t)(req[4] << 8 | req[5]);
    }
    if (addr != 0 && addr != my_addr) return MB_R_NOT_FOR_US;
    bool bcast = (addr == 0);

    const meter_def_t *m = &METERS[meter < MB_METER_N ? meter : 0];
    size_t n = 0;

    switch (fc) {
    case 0x03:
    case 0x04: {
        if (len != 8) return MB_R_BAD_FRAME;
        if (info->count < 1 || info->count > 125) { n = exception(resp, addr, fc, 0x03, info); break; }
        if ((uint32_t)info->start + info->count > 0x10000) { n = exception(resp, addr, fc, 0x02, info); break; }
        resp[0] = addr;
        resp[1] = fc;
        resp[2] = (uint8_t)(info->count * 2);
        n = 3;
        for (uint16_t i = 0; i < info->count; i++) {
            uint16_t r;
            /* Registers of the other function code read as 0 (real meters
             * keep configuration there; the inverter may probe it). */
            if (fc != m->fc || !reg_value(m, info->start + i, v, &r)) { r = 0; info->unmapped = true; }
            resp[n++] = r >> 8;
            resp[n++] = r & 0xFF;
        }
        n = finish(resp, n);
        break;
    }
    case 0x06:                       /* write single register: acknowledge, ignore */
        if (len != 8) return MB_R_BAD_FRAME;
        memcpy(resp, req, 6);
        n = finish(resp, 6);
        break;
    case 0x10:                       /* write multiple registers: acknowledge, ignore */
        if (len < 9 || len != (size_t)9 + req[6]) return MB_R_BAD_FRAME;
        memcpy(resp, req, 6);
        n = finish(resp, 6);
        break;
    default:
        n = exception(resp, addr, fc, 0x01, info);   /* illegal function */
        break;
    }

    if (bcast) return MB_R_BROADCAST;
    *resp_len = n;
    return MB_R_REPLY;
}
