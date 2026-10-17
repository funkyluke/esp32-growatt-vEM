/* Host unit tests for the pure-C modules (Modbus protocol, export limiter).
 * Expected frames were computed independently (Python struct + CRC).
 * Run: make -C test/host */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mb_proto.h"
#include "limiter.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int frame_eq(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    if (an != bn) { printf("  length %zu != %zu\n", an, bn); return 0; }
    if (memcmp(a, b, an)) {
        printf("  got:");  for (size_t i = 0; i < an; i++) printf(" %02X", a[i]);
        printf("\n  want:"); for (size_t i = 0; i < bn; i++) printf(" %02X", b[i]);
        printf("\n");
        return 0;
    }
    return 1;
}

static void add_crc(uint8_t *f, size_t n) {
    uint16_t c = mb_crc16(f, n);
    f[n] = c & 0xFF;
    f[n + 1] = c >> 8;
}

static const mb_values_t V = {
    .voltage = 230.0f, .current = -770.0f / 230.0f, .power_w = -770.0f, .apparent_va = 770.0f,
    .pf = 1.0f, .freq_hz = 50.0f, .import_kwh = 1234.5f, .export_kwh = 99.25f,
};

static void test_crc(void) {
    const uint8_t f[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01 };
    CHECK(mb_crc16(f, sizeof(f)) == 0x0A84);
}

static void test_sdm120_power(void) {
    const uint8_t req[]  = { 0x01, 0x04, 0x00, 0x0C, 0x00, 0x02, 0xB1, 0xC8 };
    const uint8_t want[] = { 0x01, 0x04, 0x04, 0xC4, 0x40, 0x80, 0x00, 0xA6, 0xA0 };
    uint8_t resp[256]; size_t n; mb_req_info_t info;
    CHECK(mb_process(req, sizeof(req), 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY);
    CHECK(frame_eq(resp, n, want, sizeof(want)));
    CHECK(info.fc == 4 && info.start == 0x0C && info.count == 2 && !info.unmapped);
}

static void test_sdm120_block(void) {
    /* 0x0000..0x000D: voltage at 0, current at 6, power at 12, gaps unmapped */
    const uint8_t req[] = { 0x01, 0x04, 0x00, 0x00, 0x00, 0x0E, 0x71, 0xCE };
    uint8_t resp[256]; size_t n; mb_req_info_t info;
    CHECK(mb_process(req, sizeof(req), 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY);
    CHECK(n == 3 + 28 + 2 && resp[2] == 28);
    CHECK(resp[3] == 0x43 && resp[4] == 0x66 && resp[5] == 0 && resp[6] == 0);   /* 230.0f */
    CHECK(resp[3 + 2 * 2] == 0 && resp[3 + 2 * 2 + 1] == 0);                      /* reg 2: gap */
    CHECK(resp[3 + 12 * 2] == 0xC4 && resp[3 + 12 * 2 + 1] == 0x40);              /* power */
    CHECK(info.unmapped);
    CHECK(mb_crc16(resp, n - 2) == (uint16_t)(resp[n - 2] | resp[n - 1] << 8));
}

static void test_ddsu666_kw(void) {
    const uint8_t req[]  = { 0x01, 0x03, 0x20, 0x04, 0x00, 0x02, 0x8E, 0x0A };
    const uint8_t want[] = { 0x01, 0x03, 0x04, 0xBF, 0x45, 0x1E, 0xB8, 0xC7, 0xE0 };
    uint8_t resp[256]; size_t n;
    CHECK(mb_process(req, sizeof(req), 1, MB_METER_DDSU666, &V, resp, &n, NULL) == MB_R_REPLY);
    CHECK(frame_eq(resp, n, want, sizeof(want)));
}

static void test_wrong_fc_reads_zero(void) {
    /* SDM120 measurements are FC04; an FC03 read of the same address is 0 */
    uint8_t req[8] = { 0x01, 0x03, 0x00, 0x0C, 0x00, 0x02 };
    add_crc(req, 6);
    uint8_t resp[256]; size_t n; mb_req_info_t info;
    CHECK(mb_process(req, 8, 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY);
    CHECK(n == 9 && resp[3] == 0 && resp[4] == 0 && resp[5] == 0 && resp[6] == 0 && info.unmapped);
}

static void test_addressing_and_errors(void) {
    const uint8_t req[] = { 0x01, 0x04, 0x00, 0x0C, 0x00, 0x02, 0xB1, 0xC8 };
    uint8_t resp[256]; size_t n; mb_req_info_t info;
    CHECK(mb_process(req, sizeof(req), 2, MB_METER_SDM120, &V, resp, &n, NULL) == MB_R_NOT_FOR_US && n == 0);

    uint8_t bad[sizeof(req)];
    memcpy(bad, req, sizeof(req));
    bad[7] ^= 1;
    CHECK(mb_process(bad, sizeof(bad), 1, MB_METER_SDM120, &V, resp, &n, NULL) == MB_R_BAD_CRC && n == 0);
    CHECK(mb_process(req, 3, 1, MB_METER_SDM120, &V, resp, &n, NULL) == MB_R_BAD_FRAME);

    uint8_t bc[8] = { 0x00, 0x04, 0x00, 0x0C, 0x00, 0x02 };
    add_crc(bc, 6);
    CHECK(mb_process(bc, 8, 1, MB_METER_SDM120, &V, resp, &n, NULL) == MB_R_BROADCAST && n == 0);

    const uint8_t fc1[]  = { 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0xFD, 0xCA };
    const uint8_t want[] = { 0x01, 0x81, 0x01, 0x81, 0x90 };
    CHECK(mb_process(fc1, sizeof(fc1), 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY);
    CHECK(frame_eq(resp, n, want, sizeof(want)) && info.exception == 1);

    uint8_t big[8] = { 0x01, 0x04, 0x00, 0x00, 0x00, 126 };
    add_crc(big, 6);
    CHECK(mb_process(big, 8, 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY);
    CHECK(n == 5 && resp[1] == 0x84 && resp[2] == 0x03);

    uint8_t max[8] = { 0x01, 0x04, 0x00, 0x00, 0x00, 125 };
    add_crc(max, 6);
    CHECK(mb_process(max, 8, 1, MB_METER_SDM120, &V, resp, &n, &info) == MB_R_REPLY && n == 255);
}

static void test_write_ack(void) {
    uint8_t w[8] = { 0x01, 0x06, 0x00, 0x0A, 0x00, 0x05 };
    add_crc(w, 6);
    uint8_t resp[256]; size_t n;
    CHECK(mb_process(w, 8, 1, MB_METER_SDM120, &V, resp, &n, NULL) == MB_R_REPLY);
    CHECK(frame_eq(resp, n, w, 8));   /* FC06 echoes the request */
}

static void test_limiter(void) {
    limiter_out_t o;
    limiter_in_t in = { .data_fresh = true, .grid_w = 300, .enabled = true,
                        .export_limit_w = 800, .margin_w = 30, .failsafe = LIM_FAILSAFE_THROTTLE };
    limiter_compute(&in, &o);
    CHECK(o.state == LIM_STATE_ACTIVE && o.answer && fabsf(o.reported_w - 1070) < 0.01f && o.offset_w == 770);

    /* exporting exactly the target: reported 0, the inverter holds */
    in.grid_w = -770;
    limiter_compute(&in, &o);
    CHECK(fabsf(o.reported_w) < 0.01f);
    /* exporting more than the target: reported negative, the inverter backs off */
    in.grid_w = -900;
    limiter_compute(&in, &o);
    CHECK(o.reported_w < 0);

    in.margin_w = 900;                      /* margin above the limit clamps to 0 */
    limiter_compute(&in, &o);
    CHECK(o.offset_w == 0);
    in.margin_w = 30;

    in.enabled = false;
    limiter_compute(&in, &o);
    CHECK(o.state == LIM_STATE_BYPASS && o.reported_w == -900 && o.answer);

    in.enabled = true;
    in.data_fresh = false;
    limiter_compute(&in, &o);
    CHECK(o.state == LIM_STATE_FAILSAFE && o.answer && o.reported_w == LIMITER_FAILSAFE_W);
    in.failsafe = LIM_FAILSAFE_SILENT;
    limiter_compute(&in, &o);
    CHECK(o.state == LIM_STATE_FAILSAFE && !o.answer);
    in.enabled = false;                     /* stale data never passes through */
    limiter_compute(&in, &o);
    CHECK(o.state == LIM_STATE_FAILSAFE);
}

int main(void) {
    test_crc();
    test_sdm120_power();
    test_sdm120_block();
    test_ddsu666_kw();
    test_wrong_fc_reads_zero();
    test_addressing_and_errors();
    test_write_ack();
    test_limiter();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
