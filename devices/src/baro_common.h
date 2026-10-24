/* Private helpers shared by the Infineon DPS310 and the Goertek SPL06-001 models (the two chips
 * share the result/config register layout, the coefficient block packing, the compensation scale
 * factors and the conversion times). Not part of the library's API. */
#ifndef FB_BARO_COMMON_H
#define FB_BARO_COMMON_H

#include <stdbool.h>
#include <stdint.h>

/* compensation scale factor kP/kT per oversampling field PRC 0..7 (1x..128x) */
extern const float fb_baro_kscale[8];
/* measurement rate per RATE field 0..7 */
extern const uint32_t fb_baro_rate_hz[8];
/* pressure noise Pa RMS per PRC 0..7 */
extern const float fb_baro_noise_pa[8];
/* conversion time in us per PRC 0..7 (pressure and temperature alike) */
extern const uint32_t fb_baro_conv_us[8];

/* coefficients as the drivers unpack them from the 18-byte block at 0x10 */
typedef struct {
    int32_t c0, c1;                 /* 12 bit */
    int32_t c00, c10;               /* 20 bit */
    int32_t c01, c11, c20, c21, c30; /* 16 bit */
} fb_baro_coef_t;

void fb_baro_pack_coef(uint8_t *c, const fb_baro_coef_t *k);   /* c: 18 bytes */
void fb_baro_put24(uint8_t *p, int32_t v);
int32_t fb_baro_get24(const uint8_t *p);
int32_t fb_baro_clamp24(double v);

/* Gaussian noise: xorshift64 + Marsaglia polar method, reproducible from the seed */
typedef struct {
    uint64_t s;
    double next;
    bool have;
} fb_baro_rng_t;
double fb_baro_gauss(fb_baro_rng_t *r);

#endif
