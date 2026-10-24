/* QMC5883L magnetometer model. See include/fb/qmc5883l.h. */
#include "fb/qmc5883l.h"
#include "fb/device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct fb_qmc5883l {
    uint8_t reg[FB_QMC5883L_NREGS];
    uint8_t ptr;
    bool ptr_next;                  /* the next written byte is the register pointer */
    double field[3];                /* gauss, chip frame */
    double temp_c;
    uint64_t since_ns;              /* chip time since the last sample (continuous mode) */
    uint64_t samples;
    double noise_mg;
    uint64_t rng, seed;
    double gauss_next;
    bool gauss_have;
    /* faults */
    bool frozen, detached, offset_on, noise_on;
    double offset_mg[3];
    double fault_noise_mg;
};

/* ---- PRNG: xorshift64*, Box-Muller ------------------------------------------------------- */

static double uniform(fb_qmc5883l_t *d)
{
    d->rng ^= d->rng >> 12;
    d->rng ^= d->rng << 25;
    d->rng ^= d->rng >> 27;
    return ((d->rng * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
}

static double gauss(fb_qmc5883l_t *d)
{
    if (d->gauss_have) {
        d->gauss_have = false;
        return d->gauss_next;
    }
    double u = uniform(d), v = uniform(d);
    double r = sqrt(-2.0 * log(u > 1e-300 ? u : 1e-300));
    d->gauss_next = r * sin(2 * M_PI * v);
    d->gauss_have = true;
    return r * cos(2 * M_PI * v);
}

/* ---- registers --------------------------------------------------------------------------- */

static const uint16_t odr_hz[4] = { 10, 50, 100, 200 };

static bool continuous(const fb_qmc5883l_t *d)
{
    return (d->reg[FB_QMC5883L_CTRL1] & 3) == 1;
}

static uint64_t period_ns(const fb_qmc5883l_t *d)
{
    return 1000000000ull / odr_hz[(d->reg[FB_QMC5883L_CTRL1] >> 2) & 3];
}

double fb_qmc5883l_lsb_per_gauss(const fb_qmc5883l_t *d)
{
    return (d->reg[FB_QMC5883L_CTRL1] >> 4 & 3) == 1 ? 3000.0 : 12000.0;   /* RNG 8 G : 2 G */
}

static void put16(fb_qmc5883l_t *d, int reg, int16_t v)
{
    d->reg[reg] = (uint8_t)(v & 0xFF);
    d->reg[reg + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* the chip's registers after power-on / SOFT_RST (the fields and the faults are the world's) */
static void regs_reset(fb_qmc5883l_t *d)
{
    memset(d->reg, 0, sizeof(d->reg));
    d->reg[FB_QMC5883L_ID] = FB_QMC5883L_CHIP_ID;
    d->ptr = 0;
    d->ptr_next = false;
    d->since_ns = 0;
}

static void sample(fb_qmc5883l_t *d)
{
    d->samples++;
    if (d->frozen) {
        d->reg[FB_QMC5883L_STATUS] |= FB_QMC5883L_DRDY;     /* new data every period, the same */
        return;
    }
    double lsb = fb_qmc5883l_lsb_per_gauss(d);
    double range = (d->reg[FB_QMC5883L_CTRL1] >> 4 & 3) == 1 ? 8.0 : 2.0;
    double sigma = d->noise_on ? d->fault_noise_mg : d->noise_mg;
    uint8_t st = d->reg[FB_QMC5883L_STATUS];
    bool ovl = false;
    if (st & FB_QMC5883L_DRDY) {
        st |= FB_QMC5883L_DOR;                  /* the previous sample was never read */
    }
    for (int i = 0; i < 3; i++) {
        double g = d->field[i] + (d->offset_on ? d->offset_mg[i] / 1000.0 : 0.0);
        if (sigma > 0) {
            g += gauss(d) * sigma / 1000.0;
        }
        ovl |= fabs(g) > range;
        double c = round(g * lsb);
        put16(d, FB_QMC5883L_DATA_X + 2 * i, (int16_t)(c > 32767 ? 32767 : c < -32768 ? -32768 : c));
    }
    put16(d, FB_QMC5883L_TOUT, (int16_t)lrint(d->temp_c * 100.0));
    st = (uint8_t)((st & ~FB_QMC5883L_OVL) | FB_QMC5883L_DRDY | (ovl ? FB_QMC5883L_OVL : 0));
    d->reg[FB_QMC5883L_STATUS] = st;
}

/* ---- API --------------------------------------------------------------------------------- */

fb_qmc5883l_t *fb_qmc5883l_new(void)
{
    fb_qmc5883l_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->temp_c = 25.0;
    d->noise_mg = FB_QMC5883L_NOISE_MG;
    d->seed = d->rng = 0x9D2C5680A5B1E3F7ULL;
    regs_reset(d);
    return d;
}

void fb_qmc5883l_free(fb_qmc5883l_t *d) { free(d); }

void fb_qmc5883l_reset(fb_qmc5883l_t *d)
{
    regs_reset(d);
    d->rng = d->seed;
    d->gauss_have = false;
}

bool fb_qmc5883l_i2c_start(fb_qmc5883l_t *d, bool read)
{
    if (d->detached) return false;
    d->ptr_next = !read;
    return true;
}

bool fb_qmc5883l_i2c_write_byte(fb_qmc5883l_t *d, uint8_t b)
{
    if (d->detached) return false;
    if (d->ptr_next) {
        d->ptr = b;
        d->ptr_next = false;
        return true;
    }
    uint8_t r = d->ptr++;
    switch (r) {
    case FB_QMC5883L_CTRL1:
        if (continuous(d) != ((b & 3) == 1) || ((d->reg[r] ^ b) & 0x0C)) {
            d->since_ns = 0;                    /* the mode or the rate changed: a new period */
        }
        d->reg[r] = b;
        break;
    case FB_QMC5883L_CTRL2:
        if (b & FB_QMC5883L_SOFT_RST) {
            regs_reset(d);                      /* the bit clears itself with the rest */
            d->ptr = r + 1;
        } else {
            d->reg[r] = b & (FB_QMC5883L_ROL_PNT | 0x01);
        }
        break;
    case FB_QMC5883L_SET_RESET:
        d->reg[r] = b;
        break;
    default:
        break;                                  /* read-only: ACKed, ignored */
    }
    return true;
}

uint8_t fb_qmc5883l_i2c_read_byte(fb_qmc5883l_t *d)
{
    if (d->detached) return 0xFF;
    uint8_t r = d->ptr;
    uint8_t v = r < FB_QMC5883L_NREGS ? d->reg[r] : 0;
    if (r <= FB_QMC5883L_DATA_X + 5) {
        d->reg[FB_QMC5883L_STATUS] &= (uint8_t)~(FB_QMC5883L_DRDY | FB_QMC5883L_DOR);
    }
    d->ptr = (d->reg[FB_QMC5883L_CTRL2] & FB_QMC5883L_ROL_PNT) && r == FB_QMC5883L_STATUS ? 0
           : (uint8_t)(r + 1);
    return v;
}

void fb_qmc5883l_set_field(fb_qmc5883l_t *d, const double g[3])
{
    memcpy(d->field, g, sizeof(d->field));
}

void fb_qmc5883l_set_temp(fb_qmc5883l_t *d, double temp_c) { d->temp_c = temp_c; }

void fb_qmc5883l_tick(fb_qmc5883l_t *d, uint64_t dt_ns)
{
    if (!continuous(d)) {
        d->since_ns = 0;
        return;
    }
    uint64_t p = period_ns(d);
    d->since_ns += dt_ns;
    if (d->since_ns < p) return;
    uint64_t n = d->since_ns / p;
    d->since_ns -= n * p;
    /* the samples skipped over: only the last one is in the registers (DOR from the earlier) */
    if (n > 1) sample(d);
    sample(d);
    d->samples += n > 2 ? n - 2 : 0;
}

uint64_t fb_qmc5883l_next_due_ns(const fb_qmc5883l_t *d)
{
    return continuous(d) ? period_ns(d) - d->since_ns : UINT64_MAX;
}

uint64_t fb_qmc5883l_samples(const fb_qmc5883l_t *d) { return d->samples; }

void fb_qmc5883l_set_noise(fb_qmc5883l_t *d, double sigma_mg, uint64_t seed)
{
    d->noise_mg = sigma_mg > 0 ? sigma_mg : 0;
    d->seed = d->rng = seed ? seed : 0x9D2C5680A5B1E3F7ULL;    /* xorshift must not start at 0 */
    d->gauss_have = false;
}

uint8_t fb_qmc5883l_peek(const fb_qmc5883l_t *d, uint8_t reg)
{
    return reg < FB_QMC5883L_NREGS ? d->reg[reg] : 0;
}

/* ---- the field --------------------------------------------------------------------------- */

void fb_mag_field_ned(double decl_deg, double incl_deg, double intensity_ut, double out[3])
{
    double g = intensity_ut / 100.0;            /* 1 G = 100 uT */
    double d = decl_deg * M_PI / 180, i = incl_deg * M_PI / 180;
    out[0] = g * cos(i) * cos(d);
    out[1] = g * cos(i) * sin(d);
    out[2] = g * sin(i);
}

void fb_mag_ned_to_body(const double q[4], const double v[3], double o[3])
{
    /* v_body = R^T v_ned, R = rotation body -> NED of q */
    double w = q[0], x = q[1], y = q[2], z = q[3];
    double r[3][3] = {
        { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
        { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
        { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) },
    };
    for (int i = 0; i < 3; i++) {
        o[i] = r[0][i] * v[0] + r[1][i] * v[1] + r[2][i] * v[2];
    }
}

/* board = M chip for each alignment, rows of applySensorAlignment (sensors/boardalignment.c) */
static const signed char align_m[8][3][3] = {
    { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } },      /* CW0: x, y, z */
    { { 0, 1, 0 }, { -1, 0, 0 }, { 0, 0, 1 } },     /* CW90: y, -x, z */
    { { -1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } },    /* CW180: -x, -y, z */
    { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },     /* CW270: -y, x, z */
    { { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, -1 } },    /* CW0_FLIP: -x, y, -z */
    { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },     /* CW90_FLIP: y, x, -z */
    { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } },    /* CW180_FLIP: x, -y, -z */
    { { 0, -1, 0 }, { -1, 0, 0 }, { 0, 0, -1 } },   /* CW270_FLIP: -y, -x, -z */
};

bool fb_mag_body_to_chip(int align, const double frd[3], double chip[3])
{
    if (align < FB_MAG_CW0 || align > FB_MAG_CW270_FLIP) return false;
    const double b[3] = { frd[0], -frd[1], -frd[2] };
    const signed char (*m)[3] = align_m[align - 1];
    for (int i = 0; i < 3; i++) {
        chip[i] = m[0][i] * b[0] + m[1][i] * b[1] + m[2][i] * b[2];     /* M^T: M is orthogonal */
    }
    return true;
}

static const char *const align_names[8] = {
    "CW0", "CW90", "CW180", "CW270", "CW0FLIP", "CW90FLIP", "CW180FLIP", "CW270FLIP",
};

const char *fb_mag_align_name(int align)
{
    return align >= FB_MAG_CW0 && align <= FB_MAG_CW270_FLIP ? align_names[align - 1] : NULL;
}

int fb_mag_align_parse(const char *name)
{
    for (int i = 0; name && i < 8; i++) {
        if (!strcmp(name, align_names[i])) return i + 1;
    }
    return 0;
}

/* ---- descriptor ------------------------------------------------------------------------- */

static const char *const qmc_signals[] = { "mag.x_g", "mag.y_g", "mag.z_g", NULL };
static const char *const align_opts[] = {
    "CW0", "CW90", "CW180", "CW270", "CW0FLIP", "CW90FLIP", "CW180FLIP", "CW270FLIP", NULL,
};
static const fb_param_desc_t qmc_props[] = {
    { .id = "mount", .label = "chip orientation (iNav align_mag; the firmware's DEFAULT = CW270FLIP)",
      .type = FB_PARAM_ENUM, .options = align_opts, .def_str = "CW270FLIP" },
    { .id = "declination_deg", .label = "field declination deg (east +)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -180, .max = 180, .has_default = true, .def = FB_MAG_DECLINATION_DEG },
    { .id = "inclination_deg", .label = "field inclination deg (down +)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -90, .max = 90, .has_default = true, .def = FB_MAG_INCLINATION_DEG },
    { .id = "intensity_ut", .label = "field intensity uT", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 100, .has_default = true, .def = FB_MAG_INTENSITY_UT },
    { .id = "noise_mg", .label = "noise sigma mG per axis", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 1000, .has_default = true, .def = FB_QMC5883L_NOISE_MG },
    { 0 },
};
static const fb_param_desc_t offset_params[] = {
    { .id = "x_mg", .label = "X offset mG (chip frame)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -8000, .max = 8000, .has_default = true, .def = 0 },
    { .id = "y_mg", .label = "Y offset mG (chip frame)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -8000, .max = 8000, .has_default = true, .def = 0 },
    { .id = "z_mg", .label = "Z offset mG (chip frame)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -8000, .max = 8000, .has_default = true, .def = 0 },
    { 0 },
};
static const fb_param_desc_t noise_params[] = {
    { .id = "sigma_mg", .label = "noise sigma mG per axis", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 8000 },
    { 0 },
};
static const fb_fault_desc_t qmc_faults[] = {
    { "freeze", "compass output frozen (the same field every sample)", NULL },
    { "offset", "hard-iron offset (chip frame)", offset_params },
    { "noise", "noisy compass", noise_params },
    { "detached", "compass gone from the bus (every I2C transfer NACKs)", NULL },
    { 0 },
};
static const fb_port_desc_t qmc_ports[] = {
    { .id = "i2c", .kind = "i2c", .dir = "both", .label = "I2C (the GPS module's SDA/SCL)" },
    { 0 },
};
static const fb_device_desc_t qmc_desc = {
    .id = "qmc5883l", .label = "QMC5883L compass (GPS module)", .category = "compass", .kind = "i2c",
    .props = qmc_props, .faults = qmc_faults, .signals = qmc_signals, .ports = qmc_ports,
};
const fb_device_desc_t *fb_qmc5883l_desc(void) { return &qmc_desc; }

bool fb_qmc5883l_set_fault(fb_qmc5883l_t *d, const char *fault, bool on, const double *params)
{
    if (!strcmp(fault, "freeze")) {
        d->frozen = on;
    } else if (!strcmp(fault, "offset")) {
        for (int i = 0; on && params && i < 3; i++) {
            if (!isnan(params[i])) d->offset_mg[i] = params[i];
        }
        d->offset_on = on;
    } else if (!strcmp(fault, "noise")) {
        if (on && params && !isnan(params[0])) d->fault_noise_mg = params[0];
        d->noise_on = on;
    } else if (!strcmp(fault, "detached")) {
        d->detached = on;
    } else {
        return false;
    }
    return true;
}

bool fb_qmc5883l_fault_active(const fb_qmc5883l_t *d, const char *fault)
{
    if (!strcmp(fault, "freeze")) return d->frozen;
    if (!strcmp(fault, "offset")) return d->offset_on;
    if (!strcmp(fault, "noise")) return d->noise_on;
    if (!strcmp(fault, "detached")) return d->detached;
    return false;
}
