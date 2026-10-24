/* Host-side test of the SPL06-001 model: replays iNav 9.1.0's drivers/barometer/barometer_spl06.c
 * (spl06Detect: deviceDetect / read_calibration_coefficients / spl06_configure_measurements, then
 * sensors/barometer.c baroUpdate(): get_ut, start_up, wait up_delay, get_up, start_ut, calculate,
 * wait ut_delay) in virtual time, and checks what the driver computes against the environment. */
#include "fb/atmosphere.h"
#include "fb/dps310.h"
#include "fb/spl06.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull

/* ---- the driver, as iNav has it -------------------------------------------------------- */

typedef struct { int16_t c0, c1; int32_t c00, c10; int16_t c01, c11, c20, c21, c30; } cal_t;
static cal_t cal;
static int32_t p_raw, t_raw;

/* busRead / busReadBuf / busWrite: one I2C transaction each (register pointer write, then read) */
static bool bus_read_buf(fb_spl06_t *d, uint8_t reg, uint8_t *buf, int n)
{
    return fb_spl06_i2c_write(d, &reg, 1) && fb_spl06_i2c_read(d, buf, n);
}
static bool bus_write(fb_spl06_t *d, uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return fb_spl06_i2c_write(d, b, 2);
}

static int32_t spl06_scale(int os)
{
    switch (os) {
    case 1: return 524288; case 2: return 1572864; case 4: return 3670016; case 8: return 7864320;
    case 16: return 253952; case 32: return 516096; case 64: return 1040384; case 128: return 2088960;
    default: return -1;
    }
}

static bool read_calibration(fb_spl06_t *d)
{
    uint8_t st, c[18];
    if (!(bus_read_buf(d, 0x08, &st, 1) && (st & 0x80))) return false;
    if (!bus_read_buf(d, 0x10, c, 18)) return false;
    /* verbatim unpacking of read_calibration_coefficients() */
    cal.c0 = (int16_t)((c[0] & 0x80 ? 0xF000 : 0) | ((uint16_t)c[0] << 4) | (((uint16_t)c[1] & 0xF0) >> 4));
    cal.c1 = (int16_t)(((c[1] & 0x8 ? 0xF000 : 0) | ((uint16_t)c[1] & 0x0F) << 8) | (uint16_t)c[2]);
    cal.c00 = (int32_t)((c[3] & 0x80 ? 0xFFF00000 : 0) | ((uint32_t)c[3] << 12) | ((uint32_t)c[4] << 4) | (((uint32_t)c[5] & 0xF0) >> 4));
    cal.c10 = (int32_t)((c[5] & 0x8 ? 0xFFF00000 : 0) | (((uint32_t)c[5] & 0x0F) << 16) | ((uint32_t)c[6] << 8) | (uint32_t)c[7]);
    cal.c01 = (int16_t)(((uint16_t)c[8] << 8) | ((uint16_t)c[9]));
    cal.c11 = (int16_t)(((uint16_t)c[10] << 8) | (uint16_t)c[11]);
    cal.c20 = (int16_t)(((uint16_t)c[12] << 8) | (uint16_t)c[13]);
    cal.c21 = (int16_t)(((uint16_t)c[14] << 8) | (uint16_t)c[15]);
    cal.c30 = (int16_t)(((uint16_t)c[16] << 8) | (uint16_t)c[17]);
    return true;
}

static int32_t get24(const uint8_t *b)
{
    return (int32_t)((b[0] & 0x80 ? 0xFF000000 : 0) | ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2]);
}

static float comp_temp(int os_t) { return (float)cal.c0 / 2 + ((float)t_raw / spl06_scale(os_t)) * cal.c1; }
static float comp_press(int os_p, int os_t)
{
    const float p = (float)p_raw / spl06_scale(os_p);
    const float t = (float)t_raw / spl06_scale(os_t);
    const float pc = (float)cal.c00 + p * ((float)cal.c10 + p * ((float)cal.c20 + p * cal.c30));
    const float tc = t * ((float)cal.c01 + p * ((float)cal.c11 + p * cal.c21));
    return pc + tc;
}

static int cfg_val(int os) { int v = 0; while ((1 << v) < os) v++; return v; }

/* spl06_configure_measurements() generalised over the oversampling (iNav builds with 8x/8x) */
static bool configure(fb_spl06_t *d, int os_p, int os_t)
{
    if (!bus_write(d, 0x07, (uint8_t)(0x80 | cfg_val(os_t)))) return false;   /* TMP_EXT | TMP_PRC */
    if (!bus_write(d, 0x06, (uint8_t)cfg_val(os_p))) return false;
    uint8_t r = 0;
    if (os_t > 8) r |= 1 << 3;
    if (os_p > 8) r |= 1 << 2;
    return bus_write(d, 0x09, r);
}

static uint64_t meas_time_ns(int os) { return (uint64_t)((2 + lrintf(os * 1.6f)) + 1) * MS; }   /* SPL06_MEASUREMENT_TIME */

/* one baroUpdate() pair: returns the driver's pressure (Pa) and temperature (centi-degC) */
static void baro_cycle(fb_spl06_t *d, int os_p, int os_t, int32_t *pa, int32_t *cdeg)
{
    uint8_t b[3];
    /* BAROMETER_NEEDS_SAMPLES */
    if (bus_read_buf(d, 0x03, b, 3)) t_raw = get24(b);        /* get_ut */
    bus_write(d, 0x08, 0x01);                                 /* start_up */
    fb_spl06_tick(d, meas_time_ns(os_p));                     /* up_delay */
    /* BAROMETER_NEEDS_CALCULATION */
    if (bus_read_buf(d, 0x00, b, 3)) p_raw = get24(b);        /* get_up */
    bus_write(d, 0x08, 0x02);                                 /* start_ut */
    *pa = lrintf(comp_press(os_p, os_t));
    *cdeg = lrintf(comp_temp(os_t) * 100);
    fb_spl06_tick(d, meas_time_ns(os_t));                     /* ut_delay */
}

static bool detect(fb_spl06_t *d)
{
    for (int retry = 0; retry < 5; retry++) {
        fb_spl06_tick(d, 100 * MS);                           /* delay(100) */
        uint8_t id;
        if (bus_read_buf(d, 0x0D, &id, 1) && id == 0x10) return true;
    }
    return false;
}

/* ---- reference: the datasheet formula in double ----------------------------------------- */
static double ref_press(double praw_sc, double traw_sc)
{
    return cal.c00 + praw_sc * (cal.c10 + praw_sc * (cal.c20 + praw_sc * cal.c30))
         + traw_sc * cal.c01 + traw_sc * praw_sc * (cal.c11 + praw_sc * cal.c21);
}

int main(void)
{
    fb_spl06_t *d = fb_spl06_new();
    fb_spl06_set_noise(d, 0, 1);

    /* start-up: SENSOR_RDY at 12 ms, COEF_RDY at 40 ms, coefficients unreadable before */
    uint8_t st, c[18];
    bus_read_buf(d, 0x08, &st, 1);
    CHECK((st & 0xC0) == 0, "power-on status 0x%02x", st);
    bus_read_buf(d, 0x10, c, 1);
    CHECK(c[0] == 0, "coefficients before COEF_RDY read 0x%02x", c[0]);
    fb_spl06_tick(d, 12 * MS);
    bus_read_buf(d, 0x08, &st, 1);
    CHECK((st & 0xC0) == 0x40, "12 ms: SENSOR_RDY only, 0x%02x", st);
    fb_spl06_tick(d, 28 * MS);
    bus_read_buf(d, 0x08, &st, 1);
    CHECK((st & 0xC0) == 0xC0, "40 ms: COEF_RDY too, 0x%02x", st);

    /* spl06Detect */
    CHECK(detect(d), "deviceDetect");
    CHECK(read_calibration(d), "read_calibration_coefficients");
    CHECK(cal.c0 == FB_SPL06_C0 && cal.c1 == FB_SPL06_C1 && cal.c00 == FB_SPL06_C00 && cal.c10 == FB_SPL06_C10
          && cal.c01 == FB_SPL06_C01 && cal.c11 == FB_SPL06_C11 && cal.c20 == FB_SPL06_C20
          && cal.c21 == FB_SPL06_C21 && cal.c30 == FB_SPL06_C30,
          "coefficients c0=%d c1=%d c00=%d c10=%d c01=%d c11=%d c20=%d c21=%d c30=%d",
          cal.c0, cal.c1, cal.c00, cal.c10, cal.c01, cal.c11, cal.c20, cal.c21, cal.c30);
    CHECK(configure(d, 8, 8), "spl06_configure_measurements");
    CHECK(fb_spl06_peek(d, 0x07) == 0x83 && fb_spl06_peek(d, 0x06) == 0x03 && fb_spl06_peek(d, 0x09) == 0,
          "config regs %02x %02x %02x", fb_spl06_peek(d, 0x07), fb_spl06_peek(d, 0x06), fb_spl06_peek(d, 0x09));
    CHECK(fb_spl06_next_due_ns(d) == UINT64_MAX, "standby: nothing due");

    /* the read loop at the field (1000 m ISA, 22 degC sea level): after the first pair the driver
     * reports the environment */
    double P = fb_isa_pressure_pa(FB_ISA_P0_PA, 1000), T = fb_isa_temp_c(22, 1000);
    fb_spl06_set_environment(d, P, T);
    int32_t pa = 0, cdeg = 0;
    for (int i = 0; i < 3; i++) baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(fabs(pa - P) <= 1.0, "iNav pressure %d vs %.2f", pa, P);
    CHECK(fabs(cdeg / 100.0 - T) <= 0.1, "iNav temperature %.2f vs %.2f", cdeg / 100.0, T);
    CHECK(fabsf(fb_spl06_expected_pressure_pa(d) - (float)P) < 1.0f, "expected_pressure helper %.2f", fb_spl06_expected_pressure_pa(d));

    /* climb in steps: every cycle follows (-500 m .. 9 km, sea level 30 degC .. -40 degC) */
    double worst_p = 0, worst_t = 0;
    for (int os = 1; os <= 128; os *= 2) {
        configure(d, os, os);
        for (double h = -500; h <= 9000; h += 250) {
            for (double t0 = -40; t0 <= 30; t0 += 35) {
                P = fb_isa_pressure_pa(FB_ISA_P0_PA, h);
                T = fb_isa_temp_c(t0, h);
                if (T < -40) T = -40;
                fb_spl06_set_environment(d, P, T);
                baro_cycle(d, os, os, &pa, &cdeg);            /* temperature for this pair */
                baro_cycle(d, os, os, &pa, &cdeg);
                if (fabs(pa - P) > worst_p) worst_p = fabs(pa - P);
                if (fabs(cdeg / 100.0 - T) > worst_t) worst_t = fabs(cdeg / 100.0 - T);
                /* reference formula in double on the same raw values: no rounding of the driver */
                double ref = ref_press((double)p_raw / spl06_scale(os), (double)t_raw / spl06_scale(os));
                CHECK(fabs(ref - P) < 0.15, "datasheet formula %.3f vs %.3f at %dx h=%.0f", ref, P, os, h);
            }
        }
    }
    CHECK(worst_p <= 1.0, "worst iNav pressure error %.3f Pa", worst_p);
    CHECK(worst_t <= 0.1, "worst iNav temperature error %.3f degC", worst_t);
    printf("round trip over -500..9000 m, 1x..128x: worst %.3f Pa, %.4f degC\n", worst_p, worst_t);

    /* conversion timing: 8x = 14.8 ms, RDY and the registers follow virtual time */
    configure(d, 8, 8);
    fb_spl06_set_environment(d, 90000, 20);
    baro_cycle(d, 8, 8, &pa, &cdeg);
    uint8_t b[3];
    bus_read_buf(d, 0x00, b, 3);                               /* clears PRS_RDY */
    int32_t before = get24(b);
    fb_spl06_set_environment(d, 95000, 20);
    bus_write(d, 0x08, 0x01);
    CHECK(fb_spl06_next_due_ns(d) == 14800000ull, "next due %llu", (unsigned long long)fb_spl06_next_due_ns(d));
    fb_spl06_tick(d, 14799999);
    bus_read_buf(d, 0x08, &st, 1);
    CHECK(!(st & 0x10) && (st & 7) == 1, "not yet: status 0x%02x", st);
    bus_read_buf(d, 0x00, b, 3);
    CHECK(get24(b) == before, "result unchanged during the conversion");
    fb_spl06_tick(d, 1);
    bus_read_buf(d, 0x08, &st, 1);
    CHECK((st & 0x10) && (st & 7) == 0, "done: PRS_RDY, back to standby, 0x%02x", st);
    bus_read_buf(d, 0x00, b, 3);
    CHECK(get24(b) != before, "new result latched");
    bus_read_buf(d, 0x08, &st, 1);
    CHECK(!(st & 0x10), "PRS_RDY cleared by reading PSR_B0");
    bus_write(d, 0x06, 0x07);                                  /* 128x */
    bus_write(d, 0x08, 0x01);
    CHECK(fb_spl06_next_due_ns(d) == 206800000ull, "128x: %llu", (unsigned long long)fb_spl06_next_due_ns(d));
    fb_spl06_tick(d, 206800000ull);

    /* background mode: 32 Hz pressure + 16 Hz temperature, 16x with the shifts, interrupts on */
    bus_write(d, 0x06, 0x54);
    bus_write(d, 0x07, 0xC4);
    bus_write(d, 0x09, 0x30 | 0x0C);
    uint64_t s0 = fb_spl06_samples(d);
    bus_write(d, 0x08, 0x07);
    uint64_t now = 0;
    int ints_p = 0, ints_t = 0;
    while (now < 1000 * MS) {
        uint64_t dt = fb_spl06_next_due_ns(d);
        if (now + dt > 1000 * MS) break;
        fb_spl06_tick(d, dt);
        now += dt;
        bus_read_buf(d, 0x0A, &st, 1);
        ints_p += st & 1;
        ints_t += (st >> 1) & 1;
    }
    CHECK(fb_spl06_samples(d) - s0 == 48, "background: %llu samples in 1 s", (unsigned long long)(fb_spl06_samples(d) - s0));
    CHECK(ints_p == 32 && ints_t == 16, "INT_STS: %d P, %d T", ints_p, ints_t);
    bus_read_buf(d, 0x0A, &st, 1);
    CHECK(st == 0, "INT_STS cleared by reading");
    uint64_t due = fb_spl06_next_due_ns(d);
    bus_write(d, 0x08, 0x07);                                  /* rewriting the same mode: no restart */
    CHECK(fb_spl06_next_due_ns(d) == due, "background keeps its phase");
    bus_write(d, 0x08, 0x00);
    CHECK(fb_spl06_next_due_ns(d) == UINT64_MAX, "standby stops it");

    /* soft reset: registers back, start-up times again */
    bus_write(d, 0x0C, 0x09);
    bus_read_buf(d, 0x08, &st, 1);
    CHECK((st & 0xC0) == 0 && fb_spl06_peek(d, 0x06) == 0, "after SOFT_RST: status 0x%02x", st);
    fb_spl06_tick(d, 40 * MS);
    CHECK(detect(d) && read_calibration(d) && configure(d, 8, 8), "re-detect after SOFT_RST");

    /* noise: 8x = 0.4 Pa RMS, reproducible from the seed */
    fb_spl06_set_environment(d, 90000, 20);
    bus_write(d, 0x08, 0x02);                                  /* a temperature for the compensation */
    fb_spl06_tick(d, 15 * MS);
    fb_spl06_set_noise(d, 1, 42);
    double sum = 0, sum2 = 0;
    int32_t first[4];
    for (int i = 0; i < 2000; i++) {
        bus_write(d, 0x08, 0x01);
        fb_spl06_tick(d, 15 * MS);
        float e = fb_spl06_expected_pressure_pa(d) - 90000.f;
        sum += e; sum2 += e * e;
        if (i < 4) first[i] = get24((const uint8_t[]){ fb_spl06_peek(d, 0), fb_spl06_peek(d, 1), fb_spl06_peek(d, 2) });
    }
    double mean = sum / 2000, rms = sqrt(sum2 / 2000 - mean * mean);
    CHECK(fabs(mean) < 0.05 && fabs(rms - 0.4) < 0.04, "noise mean %.3f rms %.3f", mean, rms);
    fb_spl06_set_noise(d, 1, 42);
    for (int i = 0; i < 4; i++) {
        bus_write(d, 0x08, 0x01);
        fb_spl06_tick(d, 15 * MS);
        int32_t r = get24((const uint8_t[]){ fb_spl06_peek(d, 0), fb_spl06_peek(d, 1), fb_spl06_peek(d, 2) });
        CHECK(r == first[i], "seeded noise reproducible (%d: %d vs %d)", i, r, first[i]);
    }
    fb_spl06_set_noise(d, 0, 1);

    /* faults */
    fb_spl06_set_environment(d, 90000, 20);
    for (int i = 0; i < 2; i++) baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 90000, "live %d", pa);
    double off = 120;
    CHECK(fb_spl06_set_fault(d, "offset", true, &off) && fb_spl06_fault_active(d, "offset"), "offset on");
    baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 90120, "offset: %d", pa);
    fb_spl06_set_fault(d, "offset", false, NULL);
    baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 90000, "offset off: %d", pa);
    CHECK(fb_spl06_set_fault(d, "freeze", true, NULL), "freeze on");
    fb_spl06_set_environment(d, 85000, 20);
    for (int i = 0; i < 3; i++) baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 90000, "frozen: still %d", pa);
    bus_write(d, 0x0C, 0x09);                                  /* survives a soft reset */
    fb_spl06_reset(d);                                         /* and a power cycle */
    fb_spl06_tick(d, 40 * MS);
    configure(d, 8, 8);
    for (int i = 0; i < 2; i++) baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 90000 && fb_spl06_fault_active(d, "freeze"), "frozen across resets: %d", pa);
    fb_spl06_set_fault(d, "freeze", false, NULL);
    for (int i = 0; i < 2; i++) baro_cycle(d, 8, 8, &pa, &cdeg);
    CHECK(pa == 85000, "live again: %d", pa);
    CHECK(fb_spl06_set_fault(d, "detached", true, NULL), "detached on");
    CHECK(!fb_spl06_i2c_start(d, false) && !fb_spl06_i2c_start(d, true), "address NACK");
    CHECK(!detect(d), "iNav's probe fails (baro NONE)");
    fb_spl06_set_fault(d, "detached", false, NULL);
    CHECK(detect(d), "attached again");
    CHECK(!fb_spl06_set_fault(d, "nope", true, NULL), "unknown fault");

    /* register file edge: pointer past 0x21 NACKs; DPS310-only COEF_SRCE (0x28) does not exist */
    uint8_t ptr = 0x22;
    CHECK(!fb_spl06_i2c_write(d, &ptr, 1), "pointer 0x22 NACK");
    ptr = 0x28;
    CHECK(!fb_spl06_i2c_write(d, &ptr, 1), "no COEF_SRCE");

    /* the same altitude reads the same on a dps310 fed from the same atmosphere (16x as
     * RotorFlight programs it: the dps310's synthetic coefficients clamp below ~73 kPa at 8x) */
    fb_dps310_t *dps = fb_dps310_new();
    fb_dps310_set_noise_scale(dps, 0);
    uint8_t w[2] = { 0x06, 0x04 };
    fb_dps310_i2c_write(dps, w, 2);
    w[0] = 0x07; w[1] = 0x04;
    fb_dps310_i2c_write(dps, w, 2);
    w[0] = 0x08; w[1] = 0x07;
    fb_dps310_i2c_write(dps, w, 2);
    for (double h = 0; h <= 3000; h += 500) {
        P = fb_isa_pressure_pa(FB_ISA_P0_PA, h);
        T = fb_isa_temp_c(22, h);
        fb_dps310_set_environment(dps, (float)P, (float)T);
        fb_dps310_sample(dps);
        fb_spl06_set_environment(d, P, T);
        baro_cycle(d, 8, 8, &pa, &cdeg);
        baro_cycle(d, 8, 8, &pa, &cdeg);
        CHECK(fabs(fb_dps310_expected_pressure_pa(dps) - pa) <= 1.5, "h=%.0f: dps310 %.1f spl06 %d", h, fb_dps310_expected_pressure_pa(dps), pa);
    }
    CHECK(fabs(fb_isa_pressure_pa(FB_ISA_P0_PA, 1000) - 89874.6) < 0.1, "ISA 1000 m = %.2f", fb_isa_pressure_pa(FB_ISA_P0_PA, 1000));
    fb_dps310_free(dps);

    fb_spl06_free(d);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
    return fails != 0;
}
