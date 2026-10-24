/* QMC5883L: iNav 9.1.0's drivers/compass/compass_qmc5883l.c register sequences, the field
 * rotation into the chip frame, faults. */
#include "fb/qmc5883l.h"
#include "fb/device.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define MS 1000000ull

/* busWrite(dev, reg, v): START(w) reg v STOP */
static bool bus_write(fb_qmc5883l_t *d, uint8_t reg, uint8_t v)
{
    return fb_qmc5883l_i2c_start(d, false) && fb_qmc5883l_i2c_write_byte(d, reg) &&
           fb_qmc5883l_i2c_write_byte(d, v);
}

/* busReadBuf(dev, reg, buf, n): START(w) reg, repeated START(r), n bytes, STOP */
static bool bus_read(fb_qmc5883l_t *d, uint8_t reg, uint8_t *buf, int n)
{
    if (!fb_qmc5883l_i2c_start(d, false) || !fb_qmc5883l_i2c_write_byte(d, reg) ||
        !fb_qmc5883l_i2c_start(d, true)) {
        return false;
    }
    for (int i = 0; i < n; i++) buf[i] = fb_qmc5883l_i2c_read_byte(d);
    return true;
}

/* qmc5883Read(): STATUS, DRDY -> 6 bytes from 0x00 */
static bool driver_read(fb_qmc5883l_t *d, int16_t out[3])
{
    uint8_t st, b[6];
    if (!bus_read(d, FB_QMC5883L_STATUS, &st, 1) || !(st & 1)) return false;
    if (!bus_read(d, FB_QMC5883L_DATA_X, b, 6)) return false;
    for (int i = 0; i < 3; i++) out[i] = (int16_t)(b[2 * i + 1] << 8 | b[2 * i]);
    return true;
}

/* deviceDetect() + qmc5883Init() */
static bool driver_detect_init(fb_qmc5883l_t *d)
{
    uint8_t sig = 0;
    bus_write(d, FB_QMC5883L_CTRL2, 0x80);
    fb_qmc5883l_tick(d, 30 * MS);
    if (!bus_read(d, FB_QMC5883L_ID, &sig, 1) || sig != 0xFF) return false;
    if (!bus_read(d, FB_QMC5883L_CTRL1, &sig, 1) || sig != 0) return false;
    return bus_write(d, 0x0B, 0x01) && bus_write(d, FB_QMC5883L_CTRL1, 0x01 | 0x0C | 0x00 | 0x10);
}

/* iNav's sensors/boardalignment.c applySensorAlignment, verbatim cases */
static void apply_align(double *dst, const double *src, int rot)
{
    const double x = src[0], y = src[1], z = src[2];
    switch (rot) {
    default: case 1: dst[0] = x; dst[1] = y; dst[2] = z; break;
    case 2: dst[0] = y; dst[1] = -x; dst[2] = z; break;
    case 3: dst[0] = -x; dst[1] = -y; dst[2] = z; break;
    case 4: dst[0] = -y; dst[1] = x; dst[2] = z; break;
    case 5: dst[0] = -x; dst[1] = y; dst[2] = -z; break;
    case 6: dst[0] = y; dst[1] = x; dst[2] = -z; break;
    case 7: dst[0] = x; dst[1] = -y; dst[2] = -z; break;
    case 8: dst[0] = -y; dst[1] = -x; dst[2] = -z; break;
    }
}

static void yaw_quat(double yaw_deg, double q[4])
{
    double h = yaw_deg * M_PI / 360;
    q[0] = cos(h); q[1] = 0; q[2] = 0; q[3] = sin(h);
}

int main(void)
{
    fb_qmc5883l_t *d = fb_qmc5883l_new();
    int16_t m[3];

    /* power-on: standby, ID, nothing to read */
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_ID) == 0xFF && fb_qmc5883l_peek(d, FB_QMC5883L_CTRL1) == 0);
    CHECK(fb_qmc5883l_next_due_ns(d) == UINT64_MAX);
    CHECK(!driver_read(d, m));

    /* the driver's detection and init: continuous 200 Hz, 8 G */
    CHECK(driver_detect_init(d));
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_CTRL1) == 0x1D && fb_qmc5883l_peek(d, 0x0B) == 0x01);
    CHECK(fb_qmc5883l_lsb_per_gauss(d) == 3000.0);
    CHECK(fb_qmc5883l_next_due_ns(d) == 5 * MS);
    const double f1[3] = { 0.25, -0.1, 0.43 };
    fb_qmc5883l_set_noise(d, 0, 1);
    fb_qmc5883l_set_field(d, f1);
    fb_qmc5883l_tick(d, 4 * MS);
    CHECK(!driver_read(d, m));                  /* the first sample 5 ms after the mode write */
    fb_qmc5883l_tick(d, 1 * MS);
    CHECK(fb_qmc5883l_samples(d) == 1 && (fb_qmc5883l_peek(d, FB_QMC5883L_STATUS) & 1));
    CHECK(driver_read(d, m) && m[0] == 750 && m[1] == -300 && m[2] == 1290);
    CHECK(!(fb_qmc5883l_peek(d, FB_QMC5883L_STATUS) & 1));   /* DRDY cleared by the data read */
    /* the driver reads at 40 Hz, the chip samples at 200 Hz: DOR (skipped samples) */
    fb_qmc5883l_tick(d, 25 * MS);
    CHECK(fb_qmc5883l_samples(d) == 6);
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_STATUS) == (FB_QMC5883L_DRDY | FB_QMC5883L_DOR));
    CHECK(driver_read(d, m) && m[0] == 750);
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_STATUS) == 0);
    /* auto-increment across STATUS (DRDY already cleared by the data bytes) into TOUT, 100 LSB/degC */
    fb_qmc5883l_set_temp(d, 31.5);
    fb_qmc5883l_tick(d, 5 * MS);
    uint8_t b[10];
    CHECK(bus_read(d, FB_QMC5883L_DATA_X, b, 9) && b[6] == 0 && (int16_t)(b[8] << 8 | b[7]) == 3150);
    /* ROL_PNT: 0x06 rolls back to 0x00 */
    CHECK(bus_write(d, FB_QMC5883L_CTRL2, FB_QMC5883L_ROL_PNT));
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(bus_read(d, FB_QMC5883L_DATA_X, b, 9) && b[7] == 0xEE && b[8] == 0x02);   /* 750 again */
    /* range 2 G: 12000 LSB/G; beyond it OVL and saturation */
    CHECK(bus_write(d, FB_QMC5883L_CTRL1, 0x0D));
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, m) && m[0] == 3000 && m[2] == 5160);
    const double big[3] = { 3.0, 0, -3.0 };
    fb_qmc5883l_set_field(d, big);
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_STATUS) & FB_QMC5883L_OVL);
    CHECK(driver_read(d, m) && m[0] == 32767 && m[2] == -32768);
    /* standby keeps the registers, samples stop; SOFT_RST clears everything */
    CHECK(bus_write(d, FB_QMC5883L_CTRL1, 0x00));
    uint64_t n = fb_qmc5883l_samples(d);
    fb_qmc5883l_tick(d, 100 * MS);
    CHECK(fb_qmc5883l_samples(d) == n && fb_qmc5883l_next_due_ns(d) == UINT64_MAX);
    CHECK(bus_write(d, FB_QMC5883L_CTRL2, FB_QMC5883L_SOFT_RST));
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_CTRL2) == 0 && fb_qmc5883l_peek(d, FB_QMC5883L_DATA_X) == 0);
    CHECK(fb_qmc5883l_peek(d, FB_QMC5883L_ID) == 0xFF);
    /* read-only registers ignore writes */
    CHECK(bus_write(d, FB_QMC5883L_ID, 0x12) && fb_qmc5883l_peek(d, FB_QMC5883L_ID) == 0xFF);

    /* ---- the field: heading 0/90/180/270, level, mount CW270FLIP (iNav's default) ---- */
    {
        double ned[3], H, Z;
        fb_mag_field_ned(0, 60, 50, ned);       /* no declination: north exactly */
        H = 0.25, Z = 0.5 * sin(60 * M_PI / 180);
        CHECK(fabs(ned[0] - H) < 1e-12 && fabs(ned[1]) < 1e-12 && fabs(ned[2] - Z) < 1e-12);
        const double want[4][2] = { { 0, -H }, { -H, 0 }, { 0, H }, { H, 0 } };
        for (int k = 0; k < 4; k++) {
            double q[4], frd[3], chip[3], board[3];
            yaw_quat(90.0 * k, q);
            fb_mag_ned_to_body(q, ned, frd);
            CHECK(fb_mag_body_to_chip(FB_MAG_CW270_FLIP, frd, chip));
            CHECK(fabs(chip[0] - want[k][0]) < 1e-12 && fabs(chip[1] - want[k][1]) < 1e-12);
            CHECK(fabs(chip[2] - Z) < 1e-12);
            /* the firmware's alignment turns it back into its board frame (x, -y, -z) of FRD */
            apply_align(board, chip, FB_MAG_CW270_FLIP);
            CHECK(fabs(board[0] - frd[0]) < 1e-12 && fabs(board[1] + frd[1]) < 1e-12 &&
                  fabs(board[2] + frd[2]) < 1e-12);
            /* ... and its magnetic heading is the yaw: atan2 of -east/north in body axes */
            double hdg = atan2(-frd[1], frd[0]) * 180 / M_PI;
            CHECK(fabs(remainder(hdg - 90.0 * k, 360)) < 1e-9);
        }
        /* every mount inverts iNav's alignment exactly */
        for (int a = FB_MAG_CW0; a <= FB_MAG_CW270_FLIP; a++) {
            const double frd[3] = { 0.1, -0.2, 0.3 };
            double chip[3], board[3];
            CHECK(fb_mag_body_to_chip(a, frd, chip));
            apply_align(board, chip, a);
            CHECK(board[0] == 0.1 && board[1] == 0.2 && board[2] == -0.3);
            CHECK(fb_mag_align_parse(fb_mag_align_name(a)) == a);
        }
        CHECK(!fb_mag_body_to_chip(0, ned, ned) && fb_mag_align_parse("CW45") == 0);
        /* declination: east of north */
        fb_mag_field_ned(FB_MAG_DECLINATION_DEG, 0, 100, ned);
        CHECK(fabs(atan2(ned[1], ned[0]) * 180 / M_PI - FB_MAG_DECLINATION_DEG) < 1e-9);
        /* pitch 90 nose up: body X points up, the field's down component is -X */
        const double qp[4] = { cos(M_PI / 4), 0, sin(M_PI / 4), 0 };
        double frd[3];
        fb_mag_field_ned(0, 90, 50, ned);
        fb_mag_ned_to_body(qp, ned, frd);
        CHECK(fabs(frd[0] + 0.5) < 1e-12 && fabs(frd[2]) < 1e-12);
    }

    /* ---- faults ---- */
    fb_qmc5883l_reset(d);
    CHECK(driver_detect_init(d));
    fb_qmc5883l_set_field(d, f1);
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, m) && m[0] == 750);
    /* freeze: DRDY keeps coming, the same values */
    CHECK(fb_qmc5883l_set_fault(d, "freeze", true, NULL) && fb_qmc5883l_fault_active(d, "freeze"));
    const double f2[3] = { -0.2, 0.2, 0.2 };
    fb_qmc5883l_set_field(d, f2);
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, m) && m[0] == 750 && m[1] == -300);
    CHECK(fb_qmc5883l_set_fault(d, "freeze", false, NULL));
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, m) && m[0] == -600 && m[1] == 600);
    /* offset: chip-frame hard iron in mG */
    const double off[3] = { 100, NAN, -50 };
    CHECK(fb_qmc5883l_set_fault(d, "offset", true, off));
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, m) && m[0] == -300 && m[1] == 600 && m[2] == 450);
    CHECK(fb_qmc5883l_set_fault(d, "offset", false, NULL) && !fb_qmc5883l_fault_active(d, "offset"));
    /* noise: sigma in mG, statistics over 4000 samples */
    const double sg[1] = { 10 };
    CHECK(fb_qmc5883l_set_fault(d, "noise", true, sg));
    double s = 0, s2 = 0;
    for (int i = 0; i < 4000; i++) {
        fb_qmc5883l_tick(d, 5 * MS);
        CHECK(driver_read(d, m));
        double e = (m[0] + 600) / 3.0;          /* mG */
        s += e;
        s2 += e * e;
    }
    double mean = s / 4000, sd = sqrt(s2 / 4000 - mean * mean);
    CHECK(fabs(mean) < 1.0 && fabs(sd - 10) < 0.6);
    CHECK(fb_qmc5883l_set_fault(d, "noise", false, NULL));
    /* the configured noise, reproducible from the seed */
    fb_qmc5883l_set_noise(d, 2, 42);
    fb_qmc5883l_tick(d, 5 * MS);
    int16_t a1[3], a2[3];
    CHECK(driver_read(d, a1));
    fb_qmc5883l_set_noise(d, 2, 42);
    fb_qmc5883l_tick(d, 5 * MS);
    CHECK(driver_read(d, a2) && !memcmp(a1, a2, sizeof(a1)));
    /* detached: the address NACKs, the driver's detection fails; faults survive a reset */
    CHECK(fb_qmc5883l_set_fault(d, "detached", true, NULL));
    CHECK(!fb_qmc5883l_i2c_start(d, false) && !driver_read(d, m));
    fb_qmc5883l_reset(d);
    CHECK(!driver_detect_init(d) && fb_qmc5883l_fault_active(d, "detached"));
    CHECK(fb_qmc5883l_set_fault(d, "detached", false, NULL) && driver_detect_init(d));
    CHECK(!fb_qmc5883l_set_fault(d, "nope", true, NULL));

    /* descriptor */
    const fb_device_desc_t *desc = fb_device_find("qmc5883l");
    CHECK(desc && !strcmp(desc->category, "compass") && !strcmp(desc->ports[0].kind, "i2c"));
    CHECK(fb_device_fault(desc, "offset") && fb_fault_nparams(fb_device_fault(desc, "offset")) == 3);
    for (const fb_fault_desc_t *f = desc->faults; f->id; f++) {
        CHECK(fb_qmc5883l_set_fault(d, f->id, false, NULL));
    }

    fb_qmc5883l_free(d);
    printf("test_qmc5883l: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
