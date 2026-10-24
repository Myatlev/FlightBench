/* DPS310 / SPL06-001 barometer register model. See include/fb/dps310.h. */
#include "fb/dps310.h"
#include "fb/device.h"
#include "baro_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define R_PSR_B2    0x00
#define R_TMP_B2    0x03
#define R_PRS_CFG   0x06
#define R_TMP_CFG   0x07
#define R_MEAS_CFG  0x08
#define R_CFG_REG   0x09
#define R_INT_STS   0x0A
#define R_FIFO_STS  0x0B
#define R_RESET     0x0C
#define R_ID        0x0D
#define R_COEF      0x10
#define R_COEF_END  0x21
#define R_COEF_SRCE 0x28
#define NREGS       0x29

#define MEAS_CFG_COEF_RDY   (1u << 7)
#define MEAS_CFG_SENSOR_RDY (1u << 6)
#define MEAS_CFG_TMP_RDY    (1u << 5)
#define MEAS_CFG_PRS_RDY    (1u << 4)

struct fb_dps310 {
    uint8_t regs[NREGS];
    uint8_t ptr;
    bool ptr_next;      /* addressed for writing: the next byte is the register pointer */
    bool frozen;        /* fault "freeze" (kept across reset) */
    float noise_k;      /* noise scale (1 = datasheet precision) */
    fb_baro_rng_t rng;
    float pressure_pa;
    float temp_c;
};


static void put_coef(uint8_t *c)
{
    const fb_baro_coef_t k = { .c1 = FB_DPS310_C1, .c00 = FB_DPS310_C00, .c10 = FB_DPS310_C10 };
    fb_baro_pack_coef(c, &k);
}

static void power_on_defaults(fb_dps310_t *d)
{
    uint8_t res[6];
    memcpy(res, d->regs, sizeof(res));
    memset(d->regs, 0, sizeof(d->regs));
    if (d->frozen) {
        memcpy(d->regs, res, sizeof(res));  /* frozen: PSR/TMP keep the last measurement */
    }
    d->ptr = 0;
    d->ptr_next = false;
    d->regs[R_ID] = FB_DPS310_PRODUCT_ID;
    d->regs[R_MEAS_CFG] = MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY;   /* the driver waits 40 ms; we are ready at once */
    d->regs[R_COEF_SRCE] = 0x80;                                     /* TMP_COEF_SRCE: external (MEMS) sensor */
    put_coef(&d->regs[R_COEF]);
}

fb_dps310_t *fb_dps310_new(void)
{
    fb_dps310_t *d = calloc(1, sizeof(*d));
    if (d) {
        d->pressure_pa = 101325.f;
        d->temp_c = 25.f;
        d->noise_k = 1.f;
        d->rng.s = 0xD1B54A32D192ED03ULL;
        power_on_defaults(d);
    }
    return d;
}

void fb_dps310_free(fb_dps310_t *d) { free(d); }
void fb_dps310_reset(fb_dps310_t *d) { power_on_defaults(d); }

/* ---- I2C -------------------------------------------------------------------------------- */

static void reg_write(fb_dps310_t *d, uint8_t a, uint8_t v)
{
    switch (a) {
    case R_RESET:
        if ((v & 0x0F) == 0x09) power_on_defaults(d);      /* SOFT_RST = 0b1001 */
        return;
    case R_PRS_CFG: case R_TMP_CFG: case R_CFG_REG: case R_INT_STS: case R_FIFO_STS:
        d->regs[a] = v;
        return;
    case R_MEAS_CFG:
        /* only MEAS_CTRL (bits 2:0) is writable; status bits are ours */
        d->regs[a] = (uint8_t)((d->regs[a] & 0xF8) | (v & 0x07));
        return;
    default:
        return;                                             /* data, ID, COEF: read-only */
    }
}

void fb_dps310_i2c_start(fb_dps310_t *d)
{
    d->ptr_next = true;
}

bool fb_dps310_i2c_write_byte(fb_dps310_t *d, uint8_t b)
{
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

uint8_t fb_dps310_i2c_read_byte(fb_dps310_t *d)
{
    d->ptr_next = false;
    if (d->ptr >= NREGS) return 0;          /* past the register file: reads 0, pointer stays */
    uint8_t v = d->regs[d->ptr];
    if (d->ptr == R_PSR_B2 + 2) d->regs[R_MEAS_CFG] &= (uint8_t)~MEAS_CFG_PRS_RDY;   /* reading the LSB clears RDY */
    if (d->ptr == R_TMP_B2 + 2) d->regs[R_MEAS_CFG] &= (uint8_t)~MEAS_CFG_TMP_RDY;
    d->ptr++;
    return v;
}

bool fb_dps310_i2c_write(fb_dps310_t *d, const uint8_t *bytes, int n)
{
    if (n < 1) return true;
    fb_dps310_i2c_start(d);
    for (int i = 0; i < n; i++) {
        if (!fb_dps310_i2c_write_byte(d, bytes[i])) return false;
    }
    return true;
}

bool fb_dps310_i2c_read(fb_dps310_t *d, uint8_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        if (d->ptr >= NREGS) return false;
        buf[i] = fb_dps310_i2c_read_byte(d);
    }
    return true;
}

/* ---- physics / time --------------------------------------------------------------------- */

void fb_dps310_set_environment(fb_dps310_t *d, float pressure_pa, float temp_c)
{
    d->pressure_pa = pressure_pa;
    d->temp_c = temp_c;
}

uint32_t fb_dps310_rate_hz(const fb_dps310_t *d)
{
    uint8_t ctrl = d->regs[R_MEAS_CFG] & 0x07;
    if (ctrl < 5) return 0;                                 /* 0 idle, 1/2 one-shot, 5..7 continuous */
    uint32_t p = (ctrl & 1) ? fb_baro_rate_hz[(d->regs[R_PRS_CFG] >> 4) & 7] : 0;
    uint32_t t = (ctrl & 2) ? fb_baro_rate_hz[(d->regs[R_TMP_CFG] >> 4) & 7] : 0;
    return p > t ? p : t;
}

uint32_t fb_dps310_oneshot_us(const fb_dps310_t *d)
{
    switch (d->regs[R_MEAS_CFG] & 0x07) {
    case 1: return fb_baro_conv_us[d->regs[R_PRS_CFG] & 7];      /* one pressure measurement */
    case 2: return fb_baro_conv_us[d->regs[R_TMP_CFG] & 7];      /* one temperature measurement */
    default: return 0;
    }
}

void fb_dps310_set_noise_scale(fb_dps310_t *d, float k) { d->noise_k = k < 0 ? 0 : k; }

float fb_dps310_noise_pa(const fb_dps310_t *d)
{
    return d->noise_k * fb_baro_noise_pa[d->regs[R_PRS_CFG] & 7];
}

void fb_dps310_sample(fb_dps310_t *d)
{
    uint8_t ctrl = d->regs[R_MEAS_CFG] & 0x07;
    if (ctrl == 0 || d->frozen) return;     /* frozen: the result registers keep the last values */
    float kP = fb_baro_kscale[d->regs[R_PRS_CFG] & 7];
    float kT = fb_baro_kscale[d->regs[R_TMP_CFG] & 7];
    /* inverse of the driver's formulas with our coefficients:
     * P = c00 + c10 * Praw/kP  ->  Praw = (P - c00)/c10*kP ;  T = c1 * Traw/kT  ->  Traw = T/c1*kT */
    if (ctrl & 1) {
        double p = d->pressure_pa;
        if (d->noise_k > 0) p += fb_baro_gauss(&d->rng) * fb_dps310_noise_pa(d);
        fb_baro_put24(&d->regs[R_PSR_B2], fb_baro_clamp24((p - FB_DPS310_C00) / FB_DPS310_C10 * kP));
        d->regs[R_MEAS_CFG] |= MEAS_CFG_PRS_RDY;
    }
    if (ctrl & 2) {
        fb_baro_put24(&d->regs[R_TMP_B2], fb_baro_clamp24((double)d->temp_c / FB_DPS310_C1 * kT));
        d->regs[R_MEAS_CFG] |= MEAS_CFG_TMP_RDY;
    }
    if (ctrl == 1 || ctrl == 2) d->regs[R_MEAS_CFG] &= 0xF8;   /* one-shot returns to idle */
}

float fb_dps310_expected_pressure_pa(const fb_dps310_t *d)
{
    float kP = fb_baro_kscale[d->regs[R_PRS_CFG] & 7];
    return (float)FB_DPS310_C00 + (float)FB_DPS310_C10 * ((float)fb_baro_get24(&d->regs[R_PSR_B2]) / kP);
}

float fb_dps310_expected_temp_c(const fb_dps310_t *d)
{
    float kT = fb_baro_kscale[d->regs[R_TMP_CFG] & 7];
    return (float)FB_DPS310_C1 * ((float)fb_baro_get24(&d->regs[R_TMP_B2]) / kT);
}

uint8_t fb_dps310_peek(const fb_dps310_t *d, uint8_t reg)
{
    return reg < NREGS ? d->regs[reg] : 0;
}

/* ---- descriptor ------------------------------------------------------------------------ */

static const char *const dps310_signals[] = { "baro.pressure_pa", "baro.temp_c", NULL };
static const fb_fault_desc_t dps310_faults[] = {
    { "freeze", "baro output frozen (result registers stop updating)", NULL },
    { 0 },
};
static const fb_port_desc_t dps310_ports[] = {
    { .id = "i2c", .kind = "i2c", .dir = "both", .label = "I2C" },
    { 0 },
};
static const fb_device_desc_t dps310_desc = {
    .id = "dps310", .label = "DPS310 baro", .category = "baro", .kind = "i2c",
    .faults = dps310_faults, .signals = dps310_signals, .ports = dps310_ports,
};
const fb_device_desc_t *fb_dps310_desc(void) { return &dps310_desc; }

bool fb_dps310_set_fault(fb_dps310_t *d, const char *fault, bool on, const double *params)
{
    (void)params;
    if (strcmp(fault, "freeze")) return false;
    d->frozen = on;
    return true;
}

bool fb_dps310_fault_active(const fb_dps310_t *d, const char *fault)
{
    return !strcmp(fault, "freeze") && d->frozen;
}
