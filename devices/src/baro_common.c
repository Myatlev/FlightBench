/* Shared by dps310.c and spl06.c. See baro_common.h. */
#include "baro_common.h"

#include <math.h>
#include <string.h>

/* DPS310 datasheet table 9 = SPL06-001 datasheet table "compensation scale factors" */
const float fb_baro_kscale[8] = { 524288.f, 1572864.f, 3670016.f, 7864320.f, 253952.f, 516096.f, 1040384.f, 2088960.f };
const uint32_t fb_baro_rate_hz[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
/* DPS310 datasheet table 16: pressure precision Pa RMS per PM_PRC 0..7 (128x not in the table: as 64x) */
const float fb_baro_noise_pa[8] = { 2.5f, 1.0f, 0.5f, 0.4f, 0.35f, 0.3f, 0.2f, 0.2f };
const uint32_t fb_baro_conv_us[8] = { 3600, 5200, 8400, 14800, 27600, 53200, 104400, 206800 };

void fb_baro_pack_coef(uint8_t *c, const fb_baro_coef_t *k)
{
    /* c0: 12 bit = c[0]<<4 | c[1]>>4 ; c1: 12 bit = (c[1]&0xF)<<8 | c[2]
     * c00: 20 bit = c[3]<<12 | c[4]<<4 | c[5]>>4 ; c10: 20 bit = (c[5]&0xF)<<16 | c[6]<<8 | c[7]
     * c01, c11, c20, c21, c30: 16 bit big-endian at c[8..17] */
    uint32_t c0 = (uint32_t)k->c0 & 0xFFF, c1 = (uint32_t)k->c1 & 0xFFF;
    uint32_t c00 = (uint32_t)k->c00 & 0xFFFFF, c10 = (uint32_t)k->c10 & 0xFFFFF;
    const int32_t w16[5] = { k->c01, k->c11, k->c20, k->c21, k->c30 };
    memset(c, 0, 18);
    c[0] = (uint8_t)(c0 >> 4);
    c[1] = (uint8_t)(((c0 & 0xF) << 4) | ((c1 >> 8) & 0xF));
    c[2] = (uint8_t)(c1 & 0xFF);
    c[3] = (uint8_t)(c00 >> 12);
    c[4] = (uint8_t)((c00 >> 4) & 0xFF);
    c[5] = (uint8_t)(((c00 & 0xF) << 4) | ((c10 >> 16) & 0xF));
    c[6] = (uint8_t)((c10 >> 8) & 0xFF);
    c[7] = (uint8_t)(c10 & 0xFF);
    for (int i = 0; i < 5; i++) {
        uint32_t u = (uint32_t)w16[i] & 0xFFFF;
        c[8 + 2 * i] = (uint8_t)(u >> 8);
        c[9 + 2 * i] = (uint8_t)u;
    }
}

void fb_baro_put24(uint8_t *p, int32_t v)
{
    uint32_t u = (uint32_t)v & 0xFFFFFF;
    p[0] = (uint8_t)(u >> 16); p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)u;
}

int32_t fb_baro_get24(const uint8_t *p)
{
    uint32_t u = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    return (u & 0x800000) ? (int32_t)(u - 0x1000000) : (int32_t)u;
}

int32_t fb_baro_clamp24(double v)
{
    if (v > 8388607.0) return 8388607;
    if (v < -8388608.0) return -8388608;
    return (int32_t)lrint(v);
}

double fb_baro_gauss(fb_baro_rng_t *r)
{
    if (r->have) {
        r->have = false;
        return r->next;
    }
    double u, v, s;
    do {
        r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17;
        u = (double)(r->s >> 11) / 9007199254740992.0 * 2 - 1;
        r->s ^= r->s << 13; r->s ^= r->s >> 7; r->s ^= r->s << 17;
        v = (double)(r->s >> 11) / 9007199254740992.0 * 2 - 1;
        s = u * u + v * v;
    } while (s >= 1 || s == 0);
    double f = sqrt(-2 * log(s) / s);
    r->next = v * f;
    r->have = true;
    return u * f;
}
