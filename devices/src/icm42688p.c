/* ICM-42688-P register model. See include/fb/icm42688p.h. */
#include "fb/icm42688p.h"
#include "fb/device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Bank 0 */
#define R_DEVICE_CONFIG   0x11
#define R_INT_CONFIG      0x14
#define R_TEMP_DATA1      0x1D
#define R_ACCEL_DATA_X1   0x1F
#define R_GYRO_DATA_X1    0x25
#define R_INT_STATUS      0x2D
#define R_INTF_CONFIG1    0x4D
#define R_PWR_MGMT0       0x4E
#define R_GYRO_CONFIG0    0x4F
#define R_ACCEL_CONFIG0   0x50
#define R_INT_CONFIG0     0x63
#define R_INT_CONFIG1     0x64
#define R_INT_SOURCE0     0x65
#define R_WHO_AM_I        0x75
#define R_REG_BANK_SEL    0x76

#define NBANKS 5

struct fb_icm42688p {
    uint8_t regs[NBANKS][128];
    uint8_t bank;

    /* SPI transaction state */
    bool cs;
    bool have_addr;
    bool read;
    uint8_t addr;

    /* physical inputs, chip frame */
    float gyro_dps[3];
    float accel_g[3];
    float temp_c;

    bool data_rdy;      /* INT_STATUS.DATA_RDY_INT */

    bool frozen;        /* fault "freeze" (kept across reset) */
};

static const uint32_t odr_table[16] = {
    /* GYRO_ODR / ACCEL_ODR field (datasheet 14.37 / 14.38) */
    [1] = 32000, [2] = 16000, [3] = 8000, [4] = 4000, [5] = 2000, [6] = 1000,
    [7] = 200, [8] = 100, [9] = 50, [10] = 25, [11] = 12, [15] = 500,
};

static void power_on_defaults(fb_icm42688p_t *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    d->bank = 0;
    d->regs[0][R_WHO_AM_I]      = FB_ICM42688P_WHO_AM_I;
    d->regs[0][R_DEVICE_CONFIG] = 0x00;
    d->regs[0][R_INT_CONFIG]    = 0x00;
    d->regs[0][R_PWR_MGMT0]     = 0x00;   /* gyro/accel off */
    d->regs[0][R_GYRO_CONFIG0]  = 0x06;   /* 2000 dps, 1 kHz */
    d->regs[0][R_ACCEL_CONFIG0] = 0x06;   /* 16 g, 1 kHz */
    d->regs[0][R_INT_CONFIG1]   = 0x10;   /* INT_ASYNC_RESET default per datasheet */
    d->regs[0][R_INT_SOURCE0]   = 0x10;   /* UI_FSYNC_INT1_EN default; DATA_RDY off */
    d->regs[0][R_INTF_CONFIG1]  = 0x91;
    d->regs[1][0x0C] = 0x0B; d->regs[1][0x0D] = 0x79; d->regs[1][0x0E] = 0x80; /* GYRO_CONFIG_STATIC3..5 */
    d->regs[2][0x03] = 0x0B; d->regs[2][0x04] = 0x79; d->regs[2][0x05] = 0x80; /* ACCEL_CONFIG_STATIC2..4 */
    d->data_rdy = false;
    /* not the SPI transaction state (cs, have_addr, addr): a soft reset arrives as the data
     * byte of a DEVICE_CONFIG write and the transaction it came in goes on */
}

fb_icm42688p_t *fb_icm42688p_new(void)
{
    fb_icm42688p_t *d = calloc(1, sizeof(*d));
    if (d) {
        d->accel_g[2] = 1.0f;
        d->temp_c = 25.0f;
        power_on_defaults(d);
    }
    return d;
}

void fb_icm42688p_free(fb_icm42688p_t *d) { free(d); }

void fb_icm42688p_reset(fb_icm42688p_t *d)
{
    power_on_defaults(d);
    d->have_addr = false;       /* power cycle: a transaction in flight is gone; CS is the MCU's pin */
}

/* ---- register access -------------------------------------------------------------------- */

static uint8_t reg_read(fb_icm42688p_t *d, uint8_t a)
{
    a &= 0x7F;
    if (d->bank == 0) {
        switch (a) {
        case R_INT_STATUS: {
            uint8_t v = d->data_rdy ? (1u << 3) : 0;
            d->data_rdy = false;             /* clear on read (INT_CONFIG0 default, what the driver uses) */
            return v;
        }
        case R_REG_BANK_SEL:
            return d->bank;
        default:
            break;
        }
    } else if (a == R_REG_BANK_SEL) {
        return d->bank;                      /* bank select is visible from every bank */
    }
    return d->regs[d->bank][a];
}

static void reg_write(fb_icm42688p_t *d, uint8_t a, uint8_t v)
{
    a &= 0x7F;
    if (a == R_REG_BANK_SEL) {
        d->bank = v & 0x07;
        if (d->bank >= NBANKS) d->bank = 0;
        return;
    }
    if (d->bank == 0) {
        switch (a) {
        case R_DEVICE_CONFIG:
            if (v & 0x01) {                  /* SOFT_RESET_CONFIG: reset, bit self-clears */
                power_on_defaults(d);
                return;
            }
            break;
        case R_WHO_AM_I:
        case R_INT_STATUS:
        case R_TEMP_DATA1: case R_TEMP_DATA1 + 1:
            return;                          /* read-only */
        default:
            break;
        }
        if (a >= R_ACCEL_DATA_X1 && a < R_ACCEL_DATA_X1 + 12) return;   /* data regs read-only */
    }
    d->regs[d->bank][a] = v;
}

/* ---- SPI -------------------------------------------------------------------------------- */

void fb_icm42688p_cs(fb_icm42688p_t *d, bool asserted)
{
    d->cs = asserted;
    d->have_addr = false;
}

uint8_t fb_icm42688p_xfer(fb_icm42688p_t *d, uint8_t mosi)
{
    if (!d->cs) return 0xFF;
    if (!d->have_addr) {
        d->addr = mosi & 0x7F;
        d->read = (mosi & 0x80) != 0;
        d->have_addr = true;
        return 0x00;                         /* address phase: MISO undefined, chip drives 0 */
    }
    uint8_t out = 0xFF;
    if (d->read) {
        out = reg_read(d, d->addr);
    } else {
        reg_write(d, d->addr, mosi);
    }
    d->addr = (d->addr + 1) & 0x7F;          /* auto-increment */
    return out;
}

/* ---- physics / time --------------------------------------------------------------------- */

void fb_icm42688p_set_motion(fb_icm42688p_t *d, const float gyro_dps[3], const float accel_g[3], float temp_c)
{
    memcpy(d->gyro_dps, gyro_dps, sizeof(d->gyro_dps));
    memcpy(d->accel_g, accel_g, sizeof(d->accel_g));
    d->temp_c = temp_c;
}

bool fb_icm42688p_is_on(const fb_icm42688p_t *d)
{
    uint8_t pm = d->regs[0][R_PWR_MGMT0];
    bool gyro_ln  = (pm & 0x0C) == 0x0C;
    bool accel_on = (pm & 0x03) >= 0x02;     /* LP or LN */
    return gyro_ln || accel_on;
}

uint32_t fb_icm42688p_odr_hz(const fb_icm42688p_t *d)
{
    if (!fb_icm42688p_is_on(d)) return 0;
    uint8_t pm = d->regs[0][R_PWR_MGMT0];
    uint8_t cfg = ((pm & 0x0C) == 0x0C) ? d->regs[0][R_GYRO_CONFIG0] : d->regs[0][R_ACCEL_CONFIG0];
    return odr_table[cfg & 0x0F];
}

static int16_t clamp16(float v)
{
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)lrintf(v);
}

static void put_be16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v >> 8);
    p[1] = (uint8_t)((uint16_t)v & 0xFF);
}

bool fb_icm42688p_sample(fb_icm42688p_t *d)
{
    if (!fb_icm42688p_is_on(d)) return false;
    uint8_t *b0 = d->regs[0];

    /* full-scale: field 0 = 2000 dps / 16 g, 1 = 1000 / 8, 2 = 500 / 4, 3 = 250 / 2, ... */
    unsigned gfs = (b0[R_GYRO_CONFIG0] >> 5) & 0x07;
    unsigned afs = (b0[R_ACCEL_CONFIG0] >> 5) & 0x07;
    float gyro_lsb_per_dps = 16.4f * (float)(1u << gfs);
    float accel_lsb_per_g  = 2048.0f * (float)(1u << afs);

    if (!d->frozen) {   /* frozen: data registers keep their values, DATA_RDY/INT1 carry on */
        put_be16(&b0[R_TEMP_DATA1], clamp16((d->temp_c - 25.0f) * 132.48f));
        for (int i = 0; i < 3; i++) {
            put_be16(&b0[R_ACCEL_DATA_X1 + 2 * i], clamp16(d->accel_g[i] * accel_lsb_per_g));
            put_be16(&b0[R_GYRO_DATA_X1 + 2 * i],  clamp16(d->gyro_dps[i] * gyro_lsb_per_dps));
        }
    }
    d->data_rdy = true;
    return (b0[R_INT_SOURCE0] & (1u << 3)) != 0;   /* UI_DRDY_INT1_EN */
}

uint32_t fb_icm42688p_int1_pulse_ns(const fb_icm42688p_t *d)
{
    return (d->regs[0][R_INT_CONFIG1] & (1u << 6)) ? 8000u : 100000u;
}

bool fb_icm42688p_int1_active_high(const fb_icm42688p_t *d)
{
    return (d->regs[0][R_INT_CONFIG] & 0x01) != 0;
}

uint8_t fb_icm42688p_peek(const fb_icm42688p_t *d, uint8_t bank, uint8_t reg)
{
    if (bank >= NBANKS) return 0;
    if (bank == 0 && (reg & 0x7F) == R_INT_STATUS) return d->data_rdy ? (1u << 3) : 0;
    return d->regs[bank][reg & 0x7F];
}

/* ---- descriptor and faults ------------------------------------------------------------- */

/* props: the motion source's noise (applied by the emulator before set_motion) */
static const fb_param_desc_t icm_props[] = {
    { .id = "gyro_noise_dps", .label = "gyro noise RMS deg/s", .type = FB_PARAM_FLOAT, .has_range = true,
      .min = 0, .max = 50, .has_default = true, .def = 0.05 },
    { .id = "accel_noise_g", .label = "accel noise RMS g", .type = FB_PARAM_FLOAT, .has_range = true,
      .min = 0, .max = 2, .has_default = true, .def = 0.002 },
    /* a static accelerometer error (the same on every axis): the chip reads
     * accel * (1 + scale_pct / 100) + offset_mg; what a 6-point calibration removes */
    { .id = "accel_offset_mg", .label = "accel offset mg", .type = FB_PARAM_FLOAT, .has_range = true,
      .min = -500, .max = 500, .has_default = true, .def = 0 },
    { .id = "accel_scale_pct", .label = "accel sensitivity error %", .type = FB_PARAM_FLOAT, .has_range = true,
      .min = -10, .max = 10, .has_default = true, .def = 0 },
    { 0 },
};
static const fb_fault_desc_t icm_faults[] = {
    { "freeze", "IMU output frozen", NULL },
    { 0 },
};
static const char *const icm_signals[] = {
    "imu.gyro_x_dps", "imu.gyro_y_dps", "imu.gyro_z_dps", "imu.accel_x_g", "imu.accel_y_g",
    "imu.accel_z_g", "imu.noisy.gyro_x_dps", "imu.noisy.gyro_y_dps", "imu.noisy.gyro_z_dps",
    "imu.noisy.accel_x_g", "imu.noisy.accel_y_g", "imu.noisy.accel_z_g", "imu.noisy.temp_c",
    "imu.odr_hz", "imu.samples", NULL,
};
static const fb_port_desc_t icm_ports[] = {
    { .id = "spi", .kind = "spi", .dir = "both", .label = "SPI" },
    { .id = "int", .kind = "gpio", .dir = "out", .label = "INT1 (data ready)", .bundled = true },
    { 0 },
};
static const fb_device_desc_t icm_desc = {
    .id = "icm42688p", .label = "ICM-42688-P gyro", .category = "imu", .kind = "spi",
    .props = icm_props, .faults = icm_faults, .signals = icm_signals, .ports = icm_ports,
};
const fb_device_desc_t *fb_icm42688p_desc(void) { return &icm_desc; }

bool fb_icm42688p_set_fault(fb_icm42688p_t *d, const char *fault, bool on, const double *params)
{
    (void)params;
    if (strcmp(fault, "freeze")) return false;
    d->frozen = on;
    return true;
}

bool fb_icm42688p_fault_active(const fb_icm42688p_t *d, const char *fault)
{
    return !strcmp(fault, "freeze") && d->frozen;
}
