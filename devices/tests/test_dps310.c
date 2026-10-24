/* Host-side test of the DPS310 model: replays RotorFlight's barometer_dps310.c deviceDetect /
 * deviceConfigure / read sequence and recomputes what the driver would report. */
#include "fb/dps310.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t rd(fb_dps310_t *d, uint8_t reg) { uint8_t p = reg, v = 0; fb_dps310_i2c_write(d, &p, 1); fb_dps310_i2c_read(d, &v, 1); return v; }
static void wr(fb_dps310_t *d, uint8_t reg, uint8_t v) { uint8_t b[2] = { reg, v }; fb_dps310_i2c_write(d, b, 2); }
static void setbits(fb_dps310_t *d, uint8_t reg, uint8_t bits) { wr(d, reg, rd(d, reg) | bits); }
static void rdbuf(fb_dps310_t *d, uint8_t reg, uint8_t *buf, int n) { fb_dps310_i2c_write(d, &reg, 1); fb_dps310_i2c_read(d, buf, n); }

static int32_t twos(uint32_t raw, int len) { return (raw & (1u << (len - 1))) ? (int32_t)(raw - (1u << len)) : (int32_t)raw; }

int main(void)
{
    fb_dps310_t *d = fb_dps310_new();
    fb_dps310_set_noise_scale(d, 0);                          /* exact values; noise tested below */

    /* deviceDetect: ID */
    CHECK(rd(d, 0x0D) == 0x10, "product id = 0x%02x", rd(d, 0x0D));

    /* deviceConfigure */
    setbits(d, 0x0C, 0x09);                                   /* soft reset */
    uint8_t st = rd(d, 0x08);
    CHECK(st & (1 << 7), "COEF_RDY after reset");
    CHECK(st & (1 << 6), "SENSOR_RDY after reset");
    uint8_t coef[18];
    rdbuf(d, 0x10, coef, 9); rdbuf(d, 0x10 + 9, coef + 9, 9);
    int32_t c0  = twos(((uint32_t)coef[0] << 4) | ((coef[1] >> 4) & 0xF), 12);
    int32_t c1  = twos((((uint32_t)coef[1] & 0xF) << 8) | coef[2], 12);
    int32_t c00 = twos(((uint32_t)coef[3] << 12) | ((uint32_t)coef[4] << 4) | ((coef[5] >> 4) & 0xF), 20);
    int32_t c10 = twos((((uint32_t)coef[5] & 0xF) << 16) | ((uint32_t)coef[6] << 8) | coef[7], 20);
    int32_t c01 = twos(((uint32_t)coef[8] << 8) | coef[9], 16);
    CHECK(c0 == 0 && c1 == FB_DPS310_C1, "c0=%d c1=%d", c0, c1);
    CHECK(c00 == FB_DPS310_C00 && c10 == FB_DPS310_C10 && c01 == 0, "c00=%d c10=%d c01=%d", c00, c10, c01);

    setbits(d, 0x06, 0x50 | 0x04);                            /* PRS_CFG: 32 Hz, 16x */
    uint8_t src = rd(d, 0x28) & 0x80;
    setbits(d, 0x07, 0x50 | 0x04 | src);                      /* TMP_CFG: 32 Hz, 16x, ext */
    setbits(d, 0x09, 0x08 | 0x04);                            /* CFG_REG shifts */
    CHECK(fb_dps310_rate_hz(d) == 0, "idle before MEAS_CTRL, got %u", fb_dps310_rate_hz(d));
    setbits(d, 0x08, 0x07);                                   /* continuous P+T */
    CHECK(fb_dps310_rate_hz(d) == 32, "rate = %u", fb_dps310_rate_hz(d));
    CHECK((rd(d, 0x08) & 0xC0) == 0xC0, "status bits survive MEAS_CFG write");

    /* environment: Yerevan-ish, 1000 m -> ~89.9 kPa, 22 degC */
    fb_dps310_set_environment(d, 89874.f, 22.f);
    fb_dps310_sample(d);
    uint8_t buf[6];
    rdbuf(d, 0x00, buf, 6);
    int32_t Praw = twos(((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2], 24);
    int32_t Traw = twos(((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 8) | buf[5], 24);
    float kP = 253952.f, kT = 253952.f;
    float P = (float)c00 + (Praw / kP) * (float)c10;          /* driver formula, higher terms are 0 */
    float T = c0 * 0.5f + c1 * (Traw / kT);
    CHECK(fabsf(P - 89874.f) < 1.0f, "pressure via driver formula = %.2f", P);
    CHECK(fabsf(T - 22.f) < 0.01f, "temperature via driver formula = %.3f", T);
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 89874.f) < 1.0f, "expected_pressure helper");

    /* RDY flags set by sample, cleared by reading the LSBs */
    fb_dps310_sample(d);
    CHECK((rd(d, 0x08) & 0x30) == 0x30, "PRS/TMP RDY set after sample");
    rdbuf(d, 0x00, buf, 6);
    CHECK((rd(d, 0x08) & 0x30) == 0, "RDY cleared after reading data");

    /* sea level, hot */
    fb_dps310_set_environment(d, 101325.f, 45.f);
    fb_dps310_sample(d);
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 101325.f) < 1.0f, "sea level = %.2f", fb_dps310_expected_pressure_pa(d));
    CHECK(fabsf(fb_dps310_expected_temp_c(d) - 45.f) < 0.01f, "45C = %.3f", fb_dps310_expected_temp_c(d));

    /* byte-level interface (what the emulator's I2C controller uses): the same transactions */
    fb_dps310_i2c_start(d);
    CHECK(fb_dps310_i2c_write_byte(d, 0x0D), "pointer byte ACKed");
    CHECK(fb_dps310_i2c_read_byte(d) == 0x10, "ID via byte reads");
    fb_dps310_i2c_start(d);
    CHECK(fb_dps310_i2c_write_byte(d, 0x00), "pointer 0x00");
    uint8_t bb[6];
    for (int i = 0; i < 6; i++) bb[i] = fb_dps310_i2c_read_byte(d);
    CHECK(fb_dps310_peek(d, 0x00) == bb[0] && fb_dps310_peek(d, 0x02) == bb[2] && fb_dps310_peek(d, 0x05) == bb[5],
          "byte reads = register file");
    fb_dps310_i2c_start(d);
    CHECK(!fb_dps310_i2c_write_byte(d, 0x40), "pointer 0x40 NACKed byte-wise");
    fb_dps310_i2c_start(d);
    CHECK(fb_dps310_i2c_write_byte(d, 0x06) && fb_dps310_i2c_write_byte(d, 0x54) &&
          fb_dps310_i2c_write_byte(d, 0x54 | 0x80), "PRS_CFG + TMP_CFG in one write (auto-increment)");
    CHECK(fb_dps310_peek(d, 0x06) == 0x54 && fb_dps310_peek(d, 0x07) == 0xD4, "auto-increment on write");

    /* fault freeze: registers keep the last measurement, back to live when cleared */
    fb_dps310_set_environment(d, 100000.f, 20.f);
    fb_dps310_sample(d);
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 100000.f) < 1.0f, "live before freeze");
    CHECK(fb_dps310_set_fault(d, "freeze", true, NULL) && fb_dps310_fault_active(d, "freeze"), "freeze on");
    CHECK(!fb_dps310_set_fault(d, "nope", true, NULL), "unknown fault refused");
    fb_dps310_set_environment(d, 95000.f, 20.f);
    fb_dps310_sample(d);
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 100000.f) < 1.0f, "frozen: still %.1f", fb_dps310_expected_pressure_pa(d));
    fb_dps310_reset(d);
    CHECK(fb_dps310_fault_active(d, "freeze"), "freeze survives reset");
    setbits(d, 0x06, 0x54); setbits(d, 0x07, 0x54); setbits(d, 0x08, 0x07);
    fb_dps310_set_fault(d, "freeze", false, NULL);
    fb_dps310_sample(d);
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 95000.f) < 1.0f, "live again after off: %.1f", fb_dps310_expected_pressure_pa(d));

    /* datasheet noise: 16x oversampling = 0.35 Pa RMS, mean unbiased */
    fb_dps310_set_noise_scale(d, 1);
    CHECK(fabsf(fb_dps310_noise_pa(d) - 0.35f) < 1e-6f, "16x noise = %.3f", fb_dps310_noise_pa(d));
    fb_dps310_set_environment(d, 101325.f, 22.f);
    double sum = 0, sum2 = 0;
    const int N = 4000;
    for (int i = 0; i < N; i++) {
        fb_dps310_sample(d);
        double e = fb_dps310_expected_pressure_pa(d) - 101325.0;
        sum += e; sum2 += e * e;
    }
    double mean = sum / N, rms = sqrt(sum2 / N - mean * mean);
    CHECK(fabs(mean) < 0.05, "noise mean %.3f Pa", mean);
    CHECK(fabs(rms - 0.35) < 0.05, "noise rms %.3f Pa (raw step %.3f Pa included)", rms, (double)FB_DPS310_C10 / 253952.0);
    fb_dps310_set_noise_scale(d, 0);

    /* RDY per result: reading PSR (3 bytes) clears PRS_RDY only, the 6-byte burst clears both */
    fb_dps310_sample(d);
    CHECK((rd(d, 0x08) & 0x30) == 0x30, "both RDY after sample");
    rdbuf(d, 0x00, buf, 3);
    CHECK((rd(d, 0x08) & 0x30) == 0x20, "PRS_RDY cleared by the PSR read, TMP_RDY kept: 0x%02x", rd(d, 0x08));
    fb_dps310_sample(d);
    fb_dps310_i2c_start(d);
    fb_dps310_i2c_write_byte(d, 0x00);
    for (int i = 0; i < 6; i++) buf[i] = fb_dps310_i2c_read_byte(d);
    CHECK((fb_dps310_peek(d, 0x08) & 0x30) == 0, "RDY cleared after the 6-byte byte-level read: 0x%02x", fb_dps310_peek(d, 0x08));

    /* the end of the register file: 0x28 is the last register, a read past it NACKs (false) */
    uint8_t two[2] = { 0xAA, 0xAA };
    fb_dps310_i2c_start(d);
    CHECK(fb_dps310_i2c_write_byte(d, 0x28), "pointer 0x28 ACKed");
    CHECK(fb_dps310_i2c_read(d, two, 1) && two[0] == 0x80, "COEF_SRCE = 0x%02x", two[0]);
    CHECK(!fb_dps310_i2c_read(d, two, 1), "read past 0x28 NACKs");
    fb_dps310_i2c_start(d);
    CHECK(!fb_dps310_i2c_write_byte(d, 0x29), "pointer 0x29 NACKed");
    uint8_t w29[2] = { 0x28, 0x00 };
    CHECK(fb_dps310_i2c_write(d, w29, 2) && fb_dps310_peek(d, 0x28) == 0x80, "COEF_SRCE is read-only");
    uint8_t w2[3] = { 0x28, 0x00, 0x00 };
    CHECK(!fb_dps310_i2c_write(d, w2, 3), "a write past 0x28 NACKs");

    /* freeze across a soft reset and a power cycle: the result registers survive */
    fb_dps310_set_environment(d, 97000.f, 31.f);
    fb_dps310_sample(d);
    uint8_t before[6], after[6];
    for (int i = 0; i < 6; i++) before[i] = fb_dps310_peek(d, (uint8_t)i);
    fb_dps310_set_fault(d, "freeze", true, NULL);
    fb_dps310_set_environment(d, 90000.f, 10.f);
    wr(d, 0x0C, 0x09);                                        /* SOFT_RST under freeze */
    for (int i = 0; i < 6; i++) after[i] = fb_dps310_peek(d, (uint8_t)i);
    CHECK(!memcmp(before, after, 6), "frozen PSR/TMP survive a soft reset");
    CHECK(fb_dps310_peek(d, 0x08) == 0xC0 && fb_dps310_peek(d, 0x06) == 0, "the rest is at power-on defaults");
    setbits(d, 0x06, 0x54); setbits(d, 0x07, 0x54); setbits(d, 0x08, 0x07);
    fb_dps310_sample(d);
    fb_dps310_reset(d);                                       /* power cycle under freeze */
    for (int i = 0; i < 6; i++) after[i] = fb_dps310_peek(d, (uint8_t)i);
    CHECK(!memcmp(before, after, 6), "frozen PSR/TMP survive a power cycle");
    setbits(d, 0x06, 0x54);                                   /* the helper scales with PM_PRC */
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 97000.f) < 1.0f, "still 97000 Pa: %.1f", fb_dps310_expected_pressure_pa(d));
    fb_dps310_set_fault(d, "freeze", false, NULL);
    fb_dps310_reset(d);
    CHECK(fb_dps310_peek(d, 0x00) == 0 && fb_dps310_peek(d, 0x05) == 0, "not frozen: a reset clears them");

    /* one-shot (MEAS_CTRL 1/2): conversion time per oversampling, one sample, RDY, back to idle */
    setbits(d, 0x06, 0x54); setbits(d, 0x07, 0x51);           /* 16x pressure, 2x temperature */
    fb_dps310_set_environment(d, 88000.f, 18.f);
    CHECK(fb_dps310_oneshot_us(d) == 0, "no one-shot pending while idle");
    setbits(d, 0x08, 0x01);
    CHECK(fb_dps310_rate_hz(d) == 0 && fb_dps310_oneshot_us(d) == 27600, "pressure one-shot 16x: %u us", fb_dps310_oneshot_us(d));
    fb_dps310_sample(d);
    CHECK((rd(d, 0x08) & 0x37) == 0x10, "PRS_RDY, idle again: 0x%02x", rd(d, 0x08));
    CHECK(fabsf(fb_dps310_expected_pressure_pa(d) - 88000.f) < 1.0f, "one-shot pressure %.1f", fb_dps310_expected_pressure_pa(d));
    CHECK(fb_dps310_oneshot_us(d) == 0, "nothing pending after the one-shot");
    setbits(d, 0x08, 0x02);
    CHECK(fb_dps310_oneshot_us(d) == 5200, "temperature one-shot 2x: %u us", fb_dps310_oneshot_us(d));
    fb_dps310_sample(d);
    CHECK((rd(d, 0x08) & 0x27) == 0x20 && fabsf(fb_dps310_expected_temp_c(d) - 18.f) < 0.01f, "TMP_RDY, 18 C: %.3f",
          fb_dps310_expected_temp_c(d));
    fb_dps310_set_fault(d, "freeze", true, NULL);
    wr(d, 0x08, 0x01);
    fb_dps310_sample(d);
    CHECK((fb_dps310_peek(d, 0x08) & 0x07) == 0x01, "frozen one-shot never completes: 0x%02x", fb_dps310_peek(d, 0x08));
    fb_dps310_set_fault(d, "freeze", false, NULL);

    /* golden: the noise is reproducible - a fresh chip, the driver's configuration, 100 samples at
     * 89874 Pa / 22 C with the datasheet noise, then the raw result registers are these bytes */
    {
        fb_dps310_t *g = fb_dps310_new();
        wr(g, 0x0C, 0x09);
        setbits(g, 0x06, 0x54); setbits(g, 0x07, 0xD4); setbits(g, 0x09, 0x0C); setbits(g, 0x08, 0x07);
        fb_dps310_set_environment(g, 89874.f, 22.f);
        for (int i = 0; i < 100; i++) fb_dps310_sample(g);
        uint8_t raw[6];
        rdbuf(g, 0x00, raw, 6);
        static const uint8_t golden[6] = { 0xfe, 0x6e, 0x30, 0x0a, 0xa8, 0x00 };
        CHECK(!memcmp(raw, golden, 6), "golden raw after 100 samples: %02x %02x %02x %02x %02x %02x",
              raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
        fb_dps310_free(g);
    }

    /* NACK on out-of-range register */
    uint8_t bad = 0x40;
    CHECK(!fb_dps310_i2c_write(d, &bad, 1), "register 0x40 must NACK");

    fb_dps310_free(d);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
