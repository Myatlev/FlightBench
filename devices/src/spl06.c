/* Goertek SPL06-001 barometer register model. See include/fb/spl06.h. */
#include "fb/spl06.h"
#include "fb/device.h"
#include "baro_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NREGS FB_SPL06_NREGS
#define NONE UINT64_MAX
#define MS 1000000ull

#define SENSOR_RDY_NS (12 * MS)
#define COEF_RDY_NS   (40 * MS)

/* CFG_REG */
#define CFG_INT_TMP 0x20
#define CFG_INT_PRS 0x10
/* INT_STS */
#define INT_TMP 0x02
#define INT_PRS 0x01
/* FIFO_STS */
#define FIFO_EMPTY 0x01

struct fb_spl06 {
    uint8_t regs[NREGS];
    uint8_t ptr;
    bool ptr_next;          /* addressed for writing: the next byte is the register pointer */
    uint64_t t_ns;          /* chip clock */
    uint64_t boot_ns;       /* clock at the last power-on / soft reset */
    uint64_t cmd_due;       /* command-mode measurement completes (NONE = none) */
    uint64_t p_due, t_due;  /* background: the running measurement completes */
    uint64_t p_next, t_next;/* background: the next measurement starts */
    uint64_t samples;
    double pressure_pa, temp_c;
    float noise_k;
    fb_baro_rng_t rng;
    bool frozen, detached, offset_on;
    double offset_pa;
};

static const fb_baro_coef_t coef = {
    .c0 = FB_SPL06_C0, .c1 = FB_SPL06_C1, .c00 = FB_SPL06_C00, .c10 = FB_SPL06_C10,
    .c01 = FB_SPL06_C01, .c11 = FB_SPL06_C11, .c20 = FB_SPL06_C20, .c21 = FB_SPL06_C21, .c30 = FB_SPL06_C30,
};

static uint8_t prc_p(const fb_spl06_t *d) { return d->regs[FB_SPL06_PRS_CFG] & 7; }
static uint8_t prc_t(const fb_spl06_t *d) { return d->regs[FB_SPL06_TMP_CFG] & 7; }
static uint64_t conv_ns(uint8_t prc) { return (uint64_t)fb_baro_conv_us[prc] * 1000u; }

static void power_on_defaults(fb_spl06_t *d)
{
    uint8_t res[6];
    memcpy(res, d->regs, sizeof(res));
    memset(d->regs, 0, sizeof(d->regs));
    if (d->frozen) memcpy(d->regs, res, sizeof(res));   /* frozen: PSR/TMP keep the last measurement */
    d->regs[FB_SPL06_ID] = FB_SPL06_PRODUCT_ID;
    d->regs[FB_SPL06_FIFO_STS] = FIFO_EMPTY;
    fb_baro_pack_coef(&d->regs[FB_SPL06_COEF], &coef);
    d->ptr = 0;
    d->ptr_next = false;
    d->boot_ns = d->t_ns;
    d->cmd_due = d->p_due = d->t_due = d->p_next = d->t_next = NONE;
}

fb_spl06_t *fb_spl06_new(void)
{
    fb_spl06_t *d = calloc(1, sizeof(*d));
    if (d) {
        d->pressure_pa = 101325.0;
        d->temp_c = 25.0;
        d->noise_k = 1.f;
        d->rng.s = 0x5B106E0D3C2A9F11ULL;
        power_on_defaults(d);
    }
    return d;
}

void fb_spl06_free(fb_spl06_t *d) { free(d); }
void fb_spl06_reset(fb_spl06_t *d) { power_on_defaults(d); }

/* ---- measurement ------------------------------------------------------------------------ */

static double t_raw_sc(const fb_spl06_t *d)
{
    return (d->temp_c - coef.c0 * 0.5) / coef.c1;
}

/* the datasheet's pressure compensation, double precision */
static double compensate(double x, double t)
{
    return coef.c00 + x * (coef.c10 + x * (coef.c20 + x * coef.c30)) + t * (coef.c01 + x * (coef.c11 + x * coef.c21));
}

/* Praw_sc for pressure p at Traw_sc t: Newton on the cubic (monotonic over the chip's range) */
static double p_raw_sc(double p, double t)
{
    double x = (p - coef.c00 - t * coef.c01) / coef.c10;
    for (int i = 0; i < 30; i++) {
        double f = compensate(x, t) - p;
        double df = coef.c10 + x * (2.0 * coef.c20 + 3.0 * x * coef.c30) + t * (coef.c11 + 2.0 * x * coef.c21);
        double step = f / df;
        x -= step;
        if (fabs(step) < 1e-12) break;
    }
    return x;
}

static void int_flag(fb_spl06_t *d, uint8_t enable, uint8_t flag)
{
    if (d->regs[FB_SPL06_CFG_REG] & enable) d->regs[FB_SPL06_INT_STS] |= flag;
}

static void latch_pressure(fb_spl06_t *d)
{
    d->samples++;
    if (d->frozen) return;
    double p = d->pressure_pa + (d->offset_on ? d->offset_pa : 0.0);
    if (d->noise_k > 0) p += fb_baro_gauss(&d->rng) * fb_spl06_noise_pa(d);
    double raw = p_raw_sc(p, t_raw_sc(d)) * fb_baro_kscale[prc_p(d)];
    fb_baro_put24(&d->regs[FB_SPL06_PSR_B2], fb_baro_clamp24(raw));
    d->regs[FB_SPL06_MEAS_CFG] |= FB_SPL06_PRS_RDY;
    int_flag(d, CFG_INT_PRS, INT_PRS);
}

static void latch_temperature(fb_spl06_t *d)
{
    d->samples++;
    if (d->frozen) return;
    fb_baro_put24(&d->regs[FB_SPL06_TMP_B2], fb_baro_clamp24(t_raw_sc(d) * fb_baro_kscale[prc_t(d)]));
    d->regs[FB_SPL06_MEAS_CFG] |= FB_SPL06_TMP_RDY;
    int_flag(d, CFG_INT_TMP, INT_TMP);
}

static uint64_t sensor_ready_ns(const fb_spl06_t *d) { return d->boot_ns + SENSOR_RDY_NS; }

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static uint64_t period_ns(uint8_t cfg) { return 1000000000ull / fb_baro_rate_hz[(cfg >> 4) & 7]; }

/* MEAS_CTRL written: start what it asks for (measurements wait for SENSOR_RDY) */
static void meas_ctrl(fb_spl06_t *d, uint8_t old)
{
    uint8_t ctrl = d->regs[FB_SPL06_MEAS_CFG] & 7;
    uint64_t start = max_u64(d->t_ns, sensor_ready_ns(d));
    d->cmd_due = NONE;
    if (ctrl == 1 || ctrl == 2) {
        d->p_due = d->t_due = d->p_next = d->t_next = NONE;
        d->cmd_due = start + conv_ns(ctrl == 1 ? prc_p(d) : prc_t(d));
        return;
    }
    if (ctrl == old && ctrl >= 5) return;                /* background already running */
    d->p_due = d->t_due = d->p_next = d->t_next = NONE;
    if (ctrl == 5 || ctrl == 7) {
        d->p_due = start + conv_ns(prc_p(d));
        d->p_next = start + period_ns(d->regs[FB_SPL06_PRS_CFG]);
    }
    if (ctrl == 6 || ctrl == 7) {
        d->t_due = start + conv_ns(prc_t(d));
        d->t_next = start + period_ns(d->regs[FB_SPL06_TMP_CFG]);
    }
    /* 0 = standby, 3/4 = reserved: nothing runs */
}

void fb_spl06_tick(fb_spl06_t *d, uint64_t dt_ns)
{
    uint64_t end = d->t_ns + dt_ns;
    for (;;) {
        uint64_t next = d->cmd_due;
        if (d->p_due < next) next = d->p_due;
        if (d->t_due < next) next = d->t_due;
        if (next > end) break;
        d->t_ns = next;
        if (next == d->cmd_due) {
            d->cmd_due = NONE;
            if (d->frozen) continue;            /* never completes: no RDY, MEAS_CTRL stays */
            if ((d->regs[FB_SPL06_MEAS_CFG] & 7) == 1) latch_pressure(d);
            else latch_temperature(d);
            d->regs[FB_SPL06_MEAS_CFG] &= 0xF8; /* back to standby */
        } else if (next == d->t_due) {
            latch_temperature(d);
            d->t_due = d->t_next + conv_ns(prc_t(d));
            d->t_next += period_ns(d->regs[FB_SPL06_TMP_CFG]);
        } else {
            latch_pressure(d);
            d->p_due = d->p_next + conv_ns(prc_p(d));
            d->p_next += period_ns(d->regs[FB_SPL06_PRS_CFG]);
        }
    }
    d->t_ns = end;
}

uint64_t fb_spl06_next_due_ns(const fb_spl06_t *d)
{
    uint64_t next = d->cmd_due;
    if (d->p_due < next) next = d->p_due;
    if (d->t_due < next) next = d->t_due;
    if (next == NONE) return NONE;
    return next > d->t_ns ? next - d->t_ns : 0;
}

uint64_t fb_spl06_samples(const fb_spl06_t *d) { return d->samples; }

void fb_spl06_set_environment(fb_spl06_t *d, double pressure_pa, double temp_c)
{
    d->pressure_pa = pressure_pa;
    d->temp_c = temp_c;
}

void fb_spl06_set_noise(fb_spl06_t *d, float scale, uint64_t seed)
{
    d->noise_k = scale < 0 ? 0 : scale;
    d->rng.s = seed ? seed : 0x5B106E0D3C2A9F11ULL;     /* xorshift must not start at 0 */
    d->rng.have = false;
}

float fb_spl06_noise_pa(const fb_spl06_t *d)
{
    return d->noise_k * fb_baro_noise_pa[prc_p(d)];
}

/* ---- I2C -------------------------------------------------------------------------------- */

static uint8_t reg_read(fb_spl06_t *d, uint8_t a)
{
    uint8_t v = d->regs[a];
    switch (a) {
    case FB_SPL06_MEAS_CFG:
        if (d->t_ns >= sensor_ready_ns(d)) v |= FB_SPL06_SENSOR_RDY;
        if (d->t_ns >= d->boot_ns + COEF_RDY_NS) v |= FB_SPL06_COEF_RDY;
        return v;
    case FB_SPL06_PSR_B2 + 2:
        d->regs[FB_SPL06_MEAS_CFG] &= (uint8_t)~FB_SPL06_PRS_RDY;
        return v;
    case FB_SPL06_TMP_B2 + 2:
        d->regs[FB_SPL06_MEAS_CFG] &= (uint8_t)~FB_SPL06_TMP_RDY;
        return v;
    case FB_SPL06_INT_STS:
        d->regs[a] = 0;                                  /* cleared by reading */
        return v;
    default:
        if (a >= FB_SPL06_COEF && d->t_ns < d->boot_ns + COEF_RDY_NS) return 0;   /* not loaded yet */
        return v;
    }
}

static void reg_write(fb_spl06_t *d, uint8_t a, uint8_t v)
{
    switch (a) {
    case FB_SPL06_RESET:
        if ((v & 0x0F) == 0x09) power_on_defaults(d);  /* SOFT_RST = 0b1001; FIFO_FLUSH (bit 7): no FIFO */
        return;
    case FB_SPL06_PRS_CFG:
        d->regs[a] = v & 0x7F;                           /* bit 7 reserved */
        return;
    case FB_SPL06_TMP_CFG: case FB_SPL06_CFG_REG:
        d->regs[a] = v;
        return;
    case FB_SPL06_MEAS_CFG: {
        uint8_t old = d->regs[a] & 7;
        d->regs[a] = (uint8_t)((d->regs[a] & 0x30) | (v & 0x07));   /* only MEAS_CTRL is writable */
        meas_ctrl(d, old);
        return;
    }
    default:
        return;                                          /* results, status, ID, COEF: read-only */
    }
}

bool fb_spl06_i2c_start(fb_spl06_t *d, bool read)
{
    if (d->detached) return false;
    d->ptr_next = !read;
    return true;
}

bool fb_spl06_i2c_write_byte(fb_spl06_t *d, uint8_t b)
{
    if (d->detached) return false;
    if (d->ptr_next) {
        d->ptr_next = false;
        if (b >= NREGS) return false;
        d->ptr = b;
        return true;
    }
    if (d->ptr >= NREGS) return false;
    reg_write(d, d->ptr, b);
    d->ptr++;
    return true;
}

uint8_t fb_spl06_i2c_read_byte(fb_spl06_t *d)
{
    if (d->detached) return 0xFF;                        /* nobody drives SDA */
    d->ptr_next = false;
    if (d->ptr >= NREGS) return 0;
    return reg_read(d, d->ptr++);
}

bool fb_spl06_i2c_write(fb_spl06_t *d, const uint8_t *bytes, int n)
{
    if (!fb_spl06_i2c_start(d, false)) return false;
    for (int i = 0; i < n; i++) {
        if (!fb_spl06_i2c_write_byte(d, bytes[i])) return false;
    }
    return true;
}

bool fb_spl06_i2c_read(fb_spl06_t *d, uint8_t *buf, int n)
{
    if (!fb_spl06_i2c_start(d, true)) return false;
    for (int i = 0; i < n; i++) {
        if (d->ptr >= NREGS) return false;
        buf[i] = fb_spl06_i2c_read_byte(d);
    }
    return true;
}

/* ---- what iNav computes (barometer_spl06.c, float) --------------------------------------- */

float fb_spl06_expected_temp_c(const fb_spl06_t *d)
{
    float t = (float)fb_baro_get24(&d->regs[FB_SPL06_TMP_B2]) / fb_baro_kscale[prc_t(d)];
    return (float)coef.c0 / 2 + t * coef.c1;
}

float fb_spl06_expected_pressure_pa(const fb_spl06_t *d)
{
    const float p = (float)fb_baro_get24(&d->regs[FB_SPL06_PSR_B2]) / fb_baro_kscale[prc_p(d)];
    const float t = (float)fb_baro_get24(&d->regs[FB_SPL06_TMP_B2]) / fb_baro_kscale[prc_t(d)];
    const float cal = (float)coef.c00 + p * ((float)coef.c10 + p * ((float)coef.c20 + p * coef.c30));
    const float tc = t * ((float)coef.c01 + p * ((float)coef.c11 + p * coef.c21));
    return cal + tc;
}

uint8_t fb_spl06_peek(const fb_spl06_t *d, uint8_t reg)
{
    return reg < NREGS ? d->regs[reg] : 0;
}

/* ---- descriptor ------------------------------------------------------------------------- */

static const char *const spl06_signals[] = { "baro.pressure_pa", "baro.temp_c", NULL };
static const fb_param_desc_t offset_params[] = {
    { .id = "offset_pa", .label = "pressure offset Pa (+ = lower altitude)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = -20000, .max = 20000 },
    { 0 },
};
static const fb_param_desc_t spl06_props[] = {
    { .id = "noise_scale", .label = "pressure noise x datasheet precision", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 20, .has_default = true, .def = 1 },
    { 0 },
};
static const fb_fault_desc_t spl06_faults[] = {
    { "freeze", "baro output frozen (result registers stop updating)", NULL },
    { "offset", "pressure offset (a step when turned on)", offset_params },
    { "detached", "baro gone from the bus (every I2C transfer NACKs)", NULL },
    { 0 },
};
static const fb_port_desc_t spl06_ports[] = {
    { .id = "i2c", .kind = "i2c", .dir = "both", .label = "I2C" },
    { 0 },
};
static const fb_device_desc_t spl06_desc = {
    .id = "spl06", .label = "SPL06-001 baro", .category = "baro", .kind = "i2c",
    .props = spl06_props, .faults = spl06_faults, .signals = spl06_signals, .ports = spl06_ports,
};
const fb_device_desc_t *fb_spl06_desc(void) { return &spl06_desc; }

bool fb_spl06_set_fault(fb_spl06_t *d, const char *fault, bool on, const double *params)
{
    if (!strcmp(fault, "freeze")) {
        d->frozen = on;
    } else if (!strcmp(fault, "offset")) {
        if (on && params && !isnan(params[0])) d->offset_pa = params[0];
        d->offset_on = on;
    } else if (!strcmp(fault, "detached")) {
        d->detached = on;
    } else {
        return false;
    }
    return true;
}

bool fb_spl06_fault_active(const fb_spl06_t *d, const char *fault)
{
    if (!strcmp(fault, "freeze")) return d->frozen;
    if (!strcmp(fault, "offset")) return d->offset_on;
    if (!strcmp(fault, "detached")) return d->detached;
    return false;
}
