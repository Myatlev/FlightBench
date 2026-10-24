/* Host-side test of the ICM-42688-P model: replays the RotorFlight driver's init sequence
 * (accgyro_spi_icm426xx.c) over the byte interface and checks what the driver would see. */
#include "fb/icm42688p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr(fb_icm42688p_t *d, uint8_t reg, uint8_t v)
{
    fb_icm42688p_cs(d, true); fb_icm42688p_xfer(d, reg & 0x7F); fb_icm42688p_xfer(d, v); fb_icm42688p_cs(d, false);
}
static uint8_t rd(fb_icm42688p_t *d, uint8_t reg)
{
    fb_icm42688p_cs(d, true); fb_icm42688p_xfer(d, reg | 0x80); uint8_t v = fb_icm42688p_xfer(d, 0xFF); fb_icm42688p_cs(d, false);
    return v;
}
static void rdbuf(fb_icm42688p_t *d, uint8_t reg, uint8_t *buf, int n)
{
    fb_icm42688p_cs(d, true); fb_icm42688p_xfer(d, reg | 0x80);
    for (int i = 0; i < n; i++) buf[i] = fb_icm42688p_xfer(d, 0xFF);
    fb_icm42688p_cs(d, false);
}
static int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

int main(void)
{
    fb_icm42688p_t *d = fb_icm42688p_new();

    /* --- detect: bank 0, soft reset, WHO_AM_I --- */
    wr(d, 0x76, 0);                 /* REG_BANK_SEL */
    wr(d, 0x11, 0x01);              /* DEVICE_CONFIG soft reset */
    wr(d, 0x4E, 0x00);              /* PWR_MGMT0 off */
    CHECK(rd(d, 0x75) == 0x47, "WHO_AM_I = 0x%02x", rd(d, 0x75));
    CHECK(fb_icm42688p_odr_hz(d) == 0, "odr while off = %u", fb_icm42688p_odr_hz(d));
    CHECK(!fb_icm42688p_sample(d), "sample while off must not raise INT");

    /* --- init as the driver does --- */
    wr(d, 0x4E, 0x00);                          /* off */
    wr(d, 0x4E, 0x0F);                          /* TEMP on, ACCEL LN, GYRO LN */
    wr(d, 0x76, 1); wr(d, 0x0C, 4); wr(d, 0x0D, 16); wr(d, 0x0E, 0x00); /* GYRO AAF */
    wr(d, 0x76, 2); wr(d, 0x03, 8); wr(d, 0x04, 16); wr(d, 0x05, 0x00); /* ACCEL AAF */
    wr(d, 0x76, 0);
    wr(d, 0x52, 0x00);                          /* GYRO_ACCEL_CONFIG0 low latency */
    wr(d, 0x14, (0 << 2) | (1 << 1) | 1);       /* INT_CONFIG: pulsed, push-pull, active high */
    wr(d, 0x63, 0x00);                          /* INT_CONFIG0: clear on status read */
    wr(d, 0x65, 1 << 3);                        /* INT_SOURCE0: DRDY -> INT1 */
    uint8_t ic1 = rd(d, 0x64);
    ic1 &= ~(1 << 4); ic1 |= (1 << 5) | (1 << 6);  /* async reset off, tdeassert disabled, 8 µs */
    wr(d, 0x64, ic1);
    wr(d, 0x4F, (3 - 3) << 5 | 4);              /* GYRO_CONFIG0: 2000 dps, ODR 4 kHz */
    wr(d, 0x50, (3 - 3) << 5 | 4);              /* ACCEL_CONFIG0: 16 g, ODR 4 kHz */

    CHECK(fb_icm42688p_is_on(d), "device on after PWR_MGMT0");
    CHECK(fb_icm42688p_odr_hz(d) == 4000, "odr = %u, want 4000", fb_icm42688p_odr_hz(d));
    CHECK(fb_icm42688p_int1_pulse_ns(d) == 8000, "pulse = %u ns", fb_icm42688p_int1_pulse_ns(d));
    CHECK(fb_icm42688p_int1_active_high(d), "INT1 active high");
    CHECK(rd(d, 0x76) == 0, "bank readback = %u", rd(d, 0x76));
    wr(d, 0x76, 1); CHECK(rd(d, 0x0C) == 4, "bank1 GYRO_CONFIG_STATIC3 = %u", rd(d, 0x0C)); wr(d, 0x76, 0);

    /* --- a sample: 100 dps on X, 1 g on Z, 30 degC --- */
    float g[3] = {100.f, -50.f, 0.f}, a[3] = {0.f, 0.f, 1.f};
    fb_icm42688p_set_motion(d, g, a, 30.f);
    CHECK(fb_icm42688p_sample(d), "sample must request INT1 pulse");
    CHECK(fb_icm42688p_peek(d, 0, 0x2D) & (1 << 3), "DATA_RDY set");

    /* driver reads 7 bytes from GYRO_DATA_X1|0x80 into rxBuf[1..]; data starts at rxBuf[1] */
    uint8_t buf[14];
    rdbuf(d, 0x25, buf, 6);
    CHECK(be16(&buf[0]) == 1640, "gyro X raw = %d (want 1640 = 100 dps * 16.4)", be16(&buf[0]));
    CHECK(be16(&buf[2]) == -820, "gyro Y raw = %d", be16(&buf[2]));
    CHECK(be16(&buf[4]) == 0, "gyro Z raw = %d", be16(&buf[4]));
    rdbuf(d, 0x1F, buf, 12);              /* accel X..Z then gyro X..Z contiguous (DMA mode reads 14) */
    CHECK(be16(&buf[4]) == 2048, "accel Z raw = %d (want 2048 = 1 g)", be16(&buf[4]));
    CHECK(be16(&buf[6]) == 1640, "gyro X via contiguous read = %d", be16(&buf[6]));
    rdbuf(d, 0x1D, buf, 2);
    CHECK(be16(&buf[0]) == 662, "temp raw = %d (want (30-25)*132.48)", be16(&buf[0]));

    /* INT_STATUS clears on read */
    CHECK(rd(d, 0x2D) & (1 << 3), "INT_STATUS DATA_RDY on first read");
    CHECK(!(rd(d, 0x2D) & (1 << 3)), "INT_STATUS cleared after read");

    /* FSR change: 500 dps -> 65.6 LSB/dps */
    wr(d, 0x4F, (2 << 5) | 4);
    fb_icm42688p_sample(d);
    rdbuf(d, 0x25, buf, 2);
    CHECK(be16(&buf[0]) == 6560, "gyro X raw at 500 dps FSR = %d (want 6560)", be16(&buf[0]));

    /* soft reset returns defaults and drops the bank */
    wr(d, 0x76, 2); wr(d, 0x76, 0); wr(d, 0x11, 0x01);   /* DEVICE_CONFIG is a bank-0 register */
    CHECK(rd(d, 0x76) == 0 && rd(d, 0x4E) == 0 && fb_icm42688p_odr_hz(d) == 0, "soft reset state");

    /* writes to read-only registers are ignored; no CS -> bus idle 0xFF */
    wr(d, 0x75, 0x00); CHECK(rd(d, 0x75) == 0x47, "WHO_AM_I read-only");
    fb_icm42688p_cs(d, false); CHECK(fb_icm42688p_xfer(d, 0xAA) == 0xFF, "idle bus reads 0xFF");

    /* --- a soft reset resets the registers and leaves the SPI interface usable. What happens to
     * further bytes of the same transaction is not defined (the datasheet asks for 1 ms before
     * the next register access), so this only checks that the reset happened, that the write's
     * data phase keeps MISO idle, and that the next transaction works --- */
    wr(d, 0x76, 0);
    wr(d, 0x4E, 0x0F);
    fb_icm42688p_cs(d, true);
    fb_icm42688p_xfer(d, 0x11);                 /* DEVICE_CONFIG, write */
    fb_icm42688p_xfer(d, 0x01);                 /* SOFT_RESET_CONFIG */
    uint8_t miso = fb_icm42688p_xfer(d, 0x5A);  /* -> register 0x12 */
    fb_icm42688p_cs(d, false);
    CHECK(miso == 0xFF, "data phase of a write: MISO 0x%02x", miso);
    CHECK(rd(d, 0x4E) == 0x00, "soft reset happened: PWR_MGMT0 0x%02x", rd(d, 0x4E));
    CHECK(rd(d, 0x75) == 0x47, "WHO_AM_I after soft reset");
    /* a soft reset alone in its transaction, then a read in the next one */
    fb_icm42688p_cs(d, true);
    fb_icm42688p_xfer(d, 0x11);
    fb_icm42688p_xfer(d, 0x01);
    fb_icm42688p_cs(d, false);
    fb_icm42688p_cs(d, true);
    fb_icm42688p_xfer(d, 0x75 | 0x80);
    CHECK(fb_icm42688p_xfer(d, 0xFF) == 0x47, "read right after a soft reset");
    fb_icm42688p_cs(d, false);
    /* a power cycle (fb_icm42688p_reset) does end a transaction in flight */
    fb_icm42688p_cs(d, true);
    fb_icm42688p_xfer(d, 0x12);
    fb_icm42688p_reset(d);
    fb_icm42688p_xfer(d, 0x75 | 0x80);          /* an address again */
    CHECK(fb_icm42688p_xfer(d, 0xFF) == 0x47, "after power cycle the next byte is an address");
    fb_icm42688p_cs(d, false);

    fb_icm42688p_free(d);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
