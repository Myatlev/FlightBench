/* DShot codec + eRPM reply + ESC model tests, checked against the firmware's own algorithms
 * (prepareDshotPacket / loadDmaBufferDshot / decodeTelemetryPacket / blDecodeTelemetryFrame). */
#include "fb/dshot.h"
#include "fb/esc.h"
#include "fb/device.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* firmware: prepareDshotPacket */
static uint16_t fw_prepare(uint16_t value, bool telem, bool bidir)
{
    uint16_t packet = (uint16_t)((value << 1) | (telem ? 1 : 0));
    unsigned csum = 0, csum_data = packet;
    for (int i = 0; i < 3; i++) { csum ^= csum_data; csum_data >>= 4; }
    if (bidir) csum = ~csum;
    csum &= 0xf;
    return (uint16_t)((packet << 4) | csum);
}
/* firmware: loadDmaBufferDshot into 18 entries */
static void fw_load(uint32_t *buf, uint16_t packet)
{
    int i;
    for (i = 0; i < 16; i++) { buf[i] = (packet & 0x8000) ? 14 : 7; packet <<= 1; }
    buf[i++] = 0; buf[i++] = 0;
}

/* frames every ms for 1 s, then none: the throttle holds until the timeout, then 0; the motor
 * spins down; the next frame restores the throttle */
static bool esc_signal_timeout_ok(fb_esc_t *esc, float timeout_s)
{
    for (int i = 0; i < 1000; i++) {
        fb_esc_set_dshot(esc, 1200, false);
        fb_esc_step(esc, 0.001f);
    }
    float rpm0 = fb_esc_rpm(esc);
    int steps = (int)lrintf(timeout_s / 0.001f);
    bool ok = rpm0 > 1000.0f;
    for (int i = 0; i < steps - 2; i++) {       /* the last frame was 1 ms before: steps-1 ms */
        fb_esc_step(esc, 0.001f);
        ok = ok && fb_esc_throttle(esc) > 0.5f && !fb_esc_signal_lost(esc);
    }
    if (!ok) printf("  lost before %.3f s\n", timeout_s);
    for (int i = 0; i < 2; i++) fb_esc_step(esc, 0.001f);
    ok = ok && fb_esc_throttle(esc) == 0.0f && fb_esc_signal_lost(esc);
    for (int i = 0; i < 3000; i++) fb_esc_step(esc, 0.001f);
    ok = ok && fb_esc_rpm(esc) < rpm0 * 0.05f;
    fb_esc_set_dshot(esc, 1200, false);
    ok = ok && fb_esc_throttle(esc) > 0.5f && !fb_esc_signal_lost(esc);
    return ok;
}

int main(void)
{
    /* packet encode matches the firmware for all values, both modes */
    int mism = 0;
    for (uint16_t v = 0; v < 2048; v++)
        for (int t = 0; t < 2; t++)
            for (int b = 0; b < 2; b++)
                if (fb_dshot_encode(v, t, b) != fw_prepare(v, t, b)) mism++;
    CHECK(mism == 0, "encode mismatches: %d", mism);

    /* CCR buffer round trip (period 20 as MOTOR_BITLENGTH) */
    uint32_t ccr[18];
    fw_load(ccr, fw_prepare(1000, true, true));
    uint16_t val; bool tel;
    CHECK(fb_dshot_decode_ccr(ccr, 20, true, &val, &tel) && val == 1000 && tel, "ccr decode bidir: %u %d", val, tel);
    CHECK(!fb_dshot_decode_ccr(ccr, 20, false, &val, &tel), "bidir frame must fail the normal checksum");
    fw_load(ccr, fw_prepare(48, false, false));
    CHECK(fb_dshot_decode_ccr(ccr, 20, false, &val, &tel) && val == 48 && !tel, "ccr decode normal");
    /* scaled timer: DShot600 on 216 MHz timer -> period 360 ticks, bit1 = 252, bit0 = 126 */
    for (int i = 0; i < 16; i++) ccr[i] = ccr[i] == 14 ? 252 : 126;
    CHECK(fb_dshot_decode_ccr(ccr, 360, false, &val, &tel) && val == 48, "scaled ccr decode");
    CHECK(fb_dshot_value_to_throttle(47) == 0.0f && fb_dshot_value_to_throttle(2047) == 1.0f, "throttle scale");

    /* eRPM reply: value -> stream -> edges -> firmware decoder -> erpm/100 */
    /* 12-bit period code covers ~917 eRPM (period 65408 us) up to MHz-class eRPM */
    const uint32_t erpms[] = { 1000, 1234, 5000, 18000, 60000, 120000, 300000 };
    for (unsigned k = 0; k < sizeof erpms / sizeof erpms[0]; k++) {
        uint16_t v16 = fb_dshot_erpm_to_value16(erpms[k]);
        uint32_t s21 = fb_dshot_value16_to_stream21(v16);
        uint32_t edges[22];
        int n = fb_dshot_stream21_to_edges(s21, 1000, 16, edges, 22);
        uint32_t got = fb_dshot_decode_edges_fw(edges, (uint32_t)n);
        uint32_t want = erpms[k] / 100;
        /* period quantisation (9-bit mantissa) gives up to ~0.2 % error at high erpm */
        CHECK(got != 0xffff && got != 0 && fabs((double)got - (double)want) <= (want * 0.004 + 1),
              "erpm %u: decoded %u (want ~%u), edges=%d stream=%06x", erpms[k], got, want, n, s21);
    }
    /* idle code */
    {
        uint32_t edges[22];
        int n = fb_dshot_stream21_to_edges(fb_dshot_value16_to_stream21(fb_dshot_erpm_to_value16(0)), 0, 16, edges, 22);
        CHECK(fb_dshot_decode_edges_fw(edges, (uint32_t)n) == 0, "erpm 0 -> idle code decodes to 0");
    }
    /* jittered edges (+-3 ticks of 16) still decode */
    {
        uint32_t edges[22];
        int n = fb_dshot_stream21_to_edges(fb_dshot_value16_to_stream21(fb_dshot_erpm_to_value16(18000)), 500, 16, edges, 22);
        for (int i = 1; i < n; i++) edges[i] += (i & 1) ? 3 : (uint32_t)-3;
        uint32_t got = fb_dshot_decode_edges_fw(edges, (uint32_t)n);
        CHECK(got == 180, "jittered decode = %u", got);
    }

    /* ESC model: spin up to governor speed and answer telemetry like a BLHeli32/AM32 */
    fb_esc_t *esc = fb_esc_new(NULL);
    fb_esc_set_voltage(esc, 50.0f);
    /* frames keep coming, as from the firmware (one per step here; the signal timeout would
     * otherwise stop the motor after 100 ms, see below) */
    for (int i = 0; i < 4000; i++) {                            /* 4 s */
        fb_esc_set_dshot(esc, 1200, false);                     /* ~57.6 % */
        fb_esc_step(esc, 0.001f);
    }
    float rpm = fb_esc_rpm(esc);
    float expect = fb_dshot_value_to_throttle(1200) * 520.0f * 50.0f;
    CHECK(fabsf(rpm - expect) < expect * 0.01f, "rpm after spin-up %.0f, want ~%.0f", rpm, expect);
    CHECK(fb_esc_erpm(esc) == (uint32_t)lrintf(rpm * 5), "erpm = rpm * poles/2");
    CHECK(fb_esc_armed(esc), "armed after throttle");
    CHECK(!fb_esc_telemetry_due(esc), "no telemetry unless requested");
    fb_esc_set_dshot(esc, 1200, true);
    CHECK(fb_esc_telemetry_due(esc), "telemetry requested");
    uint8_t f[10];
    CHECK(fb_esc_telemetry_frame(esc, f) == 10, "frame built");
    CHECK(fb_esc_crc8_kiss(f, 9) == f[9], "kiss crc");
    uint16_t volt = (uint16_t)((f[1] << 8) | f[2]), erpm100 = (uint16_t)((f[7] << 8) | f[8]);
    CHECK(volt == 5000, "voltage field %u (10 mV units)", volt);
    CHECK(erpm100 == fb_esc_erpm(esc) / 100, "erpm field %u vs %u", erpm100, fb_esc_erpm(esc) / 100);
    CHECK(!fb_esc_telemetry_due(esc), "request cleared after frame");
    /* crc8 kiss known vector: "123456789" with poly 0x07 -> 0xF4 */
    CHECK(fb_esc_crc8_kiss((const uint8_t *)"123456789", 9) == 0xF4, "crc8 0x07 check value");
    fb_esc_set_dshot(esc, 0, false);
    for (int i = 0; i < 4000; i++) fb_esc_step(esc, 0.001f);
    CHECK(fb_esc_rpm(esc) < expect * 0.01f, "spins down to ~0: %.0f", fb_esc_rpm(esc));

    /* signal loss: frames stop -> throttle 0 after the timeout (default 100 ms), not before;
     * the motor spins down; the next frame brings the throttle back */
    CHECK(esc_signal_timeout_ok(esc, 0.100f), "default timeout 100 ms");
    fb_esc_set_signal_timeout(esc, 0.25f);
    CHECK(esc_signal_timeout_ok(esc, 0.25f), "timeout 250 ms");
    fb_esc_reset(esc);
    CHECK(esc_signal_timeout_ok(esc, 0.25f), "timeout kept across reset");
    fb_esc_set_signal_timeout(esc, 0);           /* never */
    fb_esc_set_dshot(esc, 1200, false);
    for (int i = 0; i < 5000; i++) fb_esc_step(esc, 0.001f);
    CHECK(fb_esc_throttle(esc) > 0.5f && !fb_esc_signal_lost(esc), "timeout 0: holds the last frame");
    {
        const fb_device_desc_t *dd = fb_esc_desc();
        int found = 0;
        for (int i = 0; dd->props[i].id; i++) {
            if (!strcmp(dd->props[i].id, "signal_timeout_ms")) {
                found = dd->props[i].has_default && dd->props[i].def == FB_ESC_SIGNAL_TIMEOUT_MS_DEFAULT;
            }
        }
        CHECK(found, "descriptor prop signal_timeout_ms with default 100");
    }
    fb_esc_free(esc);

    /* bidir eRPM reply edges across a 16-bit timer wrap (TIM1): the machine computes the edges in
     * 32 bits and masks them to the counter width, like the capture registers would hold them */
    {
        uint16_t v16 = fb_dshot_erpm_to_value16(8000);
        uint32_t st = fb_dshot_value16_to_stream21(v16), e[21], m[21];
        uint32_t ref[21];
        int nref = fb_dshot_stream21_to_edges(st, 1000, 16, ref, 21);
        uint32_t want = fb_dshot_decode_edges_fw(ref, (uint32_t)nref);
        CHECK(want != 0xffff && want > 0, "reference decode %u", want);
        for (uint32_t t0 = 0xFE00; t0 <= 0xFFFF; t0 += 5) {
            int n = fb_dshot_stream21_to_edges(st, t0, 16, e, 21);
            bool wraps = false;
            for (int k = 0; k < n; k++) {
                m[k] = e[k] & 0xFFFF;
                wraps |= k && m[k] < m[k - 1];
                /* the 16-bit differences are the bit spacing, whatever the wrap */
                if (k) CHECK((uint16_t)(m[k] - m[k - 1]) == ref[k] - ref[k - 1], "t0 %u edge %d spacing", t0, k);
            }
            CHECK(n == nref && e[0] == t0, "t0 %u: %d edges", t0, n);
            /* the firmware's decoder subtracts 32-bit captures: a buffer that wraps is an invalid
             * packet for it (as on the chip, where the counter restarts at 0 for every frame and
             * never gets near the wrap); without a wrap it decodes the same value */
            uint32_t got = fb_dshot_decode_edges_fw(m, (uint32_t)n);
            if (wraps) CHECK(got == 0xffff, "t0 %u: wrapped buffer decoded as %u", t0, got);
            else CHECK(got == want, "t0 %u: %u vs %u", t0, got, want);
        }
    }

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
