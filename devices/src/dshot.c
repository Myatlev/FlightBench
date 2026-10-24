/* DShot codec + bidirectional eRPM reply. See include/fb/dshot.h. */
#include "fb/dshot.h"

static uint8_t csum4(uint16_t data12, bool bidir)
{
    unsigned csum = 0, d = data12;
    for (int i = 0; i < 3; i++) { csum ^= d; d >>= 4; }
    if (bidir) csum = ~csum;
    return (uint8_t)(csum & 0xF);
}

uint16_t fb_dshot_encode(uint16_t value, bool request_telemetry, bool bidir)
{
    uint16_t packet = (uint16_t)(((value & 0x7FF) << 1) | (request_telemetry ? 1 : 0));
    return (uint16_t)((packet << 4) | csum4(packet, bidir));
}

bool fb_dshot_decode_packet(uint16_t packet, bool bidir, uint16_t *value, bool *request_telemetry)
{
    uint16_t data12 = packet >> 4;
    if (csum4(data12, bidir) != (packet & 0xF)) return false;
    if (value) *value = data12 >> 1;
    if (request_telemetry) *request_telemetry = (data12 & 1) != 0;
    return true;
}

bool fb_dshot_decode_ccr(const uint32_t ccr[16], uint32_t period, bool bidir, uint16_t *value, bool *request_telemetry)
{
    uint16_t packet = 0;
    for (int i = 0; i < 16; i++) {
        packet = (uint16_t)((packet << 1) | (ccr[i] * 2 > period ? 1 : 0));
    }
    return fb_dshot_decode_packet(packet, bidir, value, request_telemetry);
}

float fb_dshot_value_to_throttle(uint16_t value)
{
    if (value < FB_DSHOT_MIN_THROTTLE) return 0.0f;
    if (value > FB_DSHOT_MAX_THROTTLE) value = FB_DSHOT_MAX_THROTTLE;
    return (float)(value - FB_DSHOT_MIN_THROTTLE) / (float)(FB_DSHOT_MAX_THROTTLE - FB_DSHOT_MIN_THROTTLE);
}

/* ---- eRPM reply ------------------------------------------------------------------------- */

static const uint8_t gcr_encode[16] = {
    0x19, 0x1B, 0x12, 0x13, 0x1D, 0x15, 0x16, 0x17, 0x1A, 0x09, 0x0A, 0x0B, 0x1E, 0x0D, 0x0E, 0x0F
};

uint16_t fb_dshot_erpm_to_value16(uint32_t erpm)
{
    uint16_t code12;
    if (erpm == 0) {
        code12 = 0x0FFF;
    } else {
        uint32_t period_us = (60000000u + erpm / 2) / erpm;     /* one electrical revolution */
        if (period_us < 1) period_us = 1;
        uint32_t e = 0;
        while ((period_us >> e) > 0x1FF && e < 7) e++;
        uint32_t m = period_us >> e;
        if (m > 0x1FF) m = 0x1FF;
        if (e == 7 && m == 0x1FF) m = 0x1FE;   /* 0x0FFF is the idle code; below ~917 eRPM is not representable */
        code12 = (uint16_t)((e << 9) | m);
    }
    /* checksum nibble: firmware requires (v ^ v>>8 ^ v>>4) & 0xf == 0xf over the 16-bit value */
    unsigned x = (code12 ^ (code12 >> 4) ^ (code12 >> 8)) & 0xF;
    uint8_t crc = (uint8_t)((~x) & 0xF);
    return (uint16_t)((code12 << 4) | crc);
}

uint32_t fb_dshot_value16_to_stream21(uint16_t v16)
{
    uint32_t gcr = 0;
    for (int n = 3; n >= 0; n--) {
        gcr = (gcr << 5) | gcr_encode[(v16 >> (4 * n)) & 0xF];
    }
    return (1u << 20) | gcr;
}

int fb_dshot_stream21_to_edges(uint32_t stream21, uint32_t t0, uint32_t bit_ticks, uint32_t *edges, int max)
{
    int n = 0;
    for (int j = 0; j < 21 && n < max; j++) {
        if (stream21 & (1u << (20 - j))) edges[n++] = t0 + (uint32_t)j * bit_ticks;
    }
    return n;
}

uint32_t fb_dshot_decode_edges_fw(const uint32_t *buffer, uint32_t count)
{
    uint32_t value = 0;
    uint32_t oldValue = buffer[0];
    int bits = 0;
    int len;
    for (uint32_t i = 1; i <= count; i++) {
        if (i < count) {
            int diff = (int)(buffer[i] - oldValue);
            if (bits >= 21) break;
            len = (diff + 8) / 16;
        } else {
            len = 21 - bits;
        }
        if (len <= 0) return 0xffff;                       /* guard added: firmware would shift by -1 */
        value <<= len;
        value |= 1u << (len - 1);
        if (i < count) oldValue = buffer[i];
        bits += len;
    }
    if (bits != 21) return 0xffff;
    static const uint32_t decode[32] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 10, 11, 0, 13, 14, 15,
        0, 0, 2, 3, 0, 5, 6, 7, 0, 0, 8, 1, 0, 4, 12, 0 };
    uint32_t decodedValue = decode[value & 0x1f];
    decodedValue |= decode[(value >> 5) & 0x1f] << 4;
    decodedValue |= decode[(value >> 10) & 0x1f] << 8;
    decodedValue |= decode[(value >> 15) & 0x1f] << 12;
    uint32_t csum = decodedValue;
    csum = csum ^ (csum >> 8);
    csum = csum ^ (csum >> 4);
    if ((csum & 0xf) != 0xf) return 0xffff;
    decodedValue >>= 4;
    if (decodedValue == 0x0fff) return 0;
    decodedValue = (decodedValue & 0x000001ff) << ((decodedValue & 0xfffffe00) >> 9);
    if (!decodedValue) return 0xffff;
    return (1000000 * 60 / 100 + decodedValue / 2) / decodedValue;
}
