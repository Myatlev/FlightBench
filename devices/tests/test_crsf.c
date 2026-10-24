/* Host-side test of the CRSF receiver model: frames it emits parse back with the same bit layout
 * and CRC the firmware uses (rx/crsf.c), telemetry from the FC is parsed and decoded. */
#include "fb/crsf.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the firmware's unpacking: little-endian bitfield of 16 x 11 bits */
static uint16_t unpack11(const uint8_t *p, int ch)
{
    uint32_t bit = (uint32_t)ch * 11, v = 0;
    for (int b = 0; b < 11; b++, bit++) if (p[bit >> 3] & (1u << (bit & 7))) v |= 1u << b;
    return (uint16_t)v;
}

int main(void)
{
    /* CRC check value from the CRSF spec: crc8_dvb_s2 of "123456789" = 0xBC */
    CHECK(fb_crsf_crc8((const uint8_t *)"123456789", 9) == 0xBC, "crc8 dvb-s2 check value = 0x%02x", fb_crsf_crc8((const uint8_t *)"123456789", 9));

    /* scale round trips at the firmware's reference points */
    CHECK(fb_crsf_us_to_raw(1500) == 992, "1500us -> %u", fb_crsf_us_to_raw(1500));
    CHECK(fb_crsf_raw_to_us(172) == 988 && fb_crsf_raw_to_us(1811) == 2012, "172/1811 -> %u/%u us", fb_crsf_raw_to_us(172), fb_crsf_raw_to_us(1811));
    CHECK(fb_crsf_raw_to_us(fb_crsf_us_to_raw(1234)) == 1234, "round trip 1234");

    fb_crsf_t *rx = fb_crsf_new();
    uint16_t us[16];
    for (int i = 0; i < 16; i++) us[i] = (uint16_t)(1000 + i * 60);
    fb_crsf_set_channels_us(rx, us);
    fb_crsf_set_rates(rx, 250, 200);

    /* 4 ms -> exactly one RC frame at 250 Hz (first at t=0) plus one link-stats at t=0 */
    fb_crsf_tick(rx, 0);
    uint8_t buf[256];
    int n = fb_crsf_tx_pop(rx, buf, sizeof buf);
    CHECK(n == 26 + 14, "bytes after first tick = %d (want 26 RC + 14 linkstats)", n);
    CHECK(buf[0] == 0xC8 && buf[1] == 24 && buf[2] == 0x16, "RC frame header %02x %02x %02x", buf[0], buf[1], buf[2]);
    CHECK(fb_crsf_crc8(&buf[2], 23) == buf[25], "RC frame crc");
    for (int i = 0; i < 16; i++) {
        uint16_t raw = unpack11(&buf[3], i);
        CHECK(fb_crsf_raw_to_us(raw) == us[i], "ch%d: %u us, want %u", i, fb_crsf_raw_to_us(raw), us[i]);
    }
    CHECK(buf[26] == 0xC8 && buf[27] == 12 && buf[28] == 0x14, "linkstats header");
    CHECK(buf[29] == 60 && buf[31] == 100, "linkstats rssi=%u lq=%u", buf[29], buf[31]);

    /* one second in 1 ms steps, draining like a UART would: 250 RC frames + 5 link stats
     * (t=0 excluded, already popped) */
    int total = 0;
    for (int ms = 0; ms < 1000; ms++) {
        fb_crsf_tick(rx, 1000000ull);
        while ((n = fb_crsf_tx_pop(rx, buf, sizeof buf)) > 0) total += n;
    }
    CHECK(total == 250 * 26 + 5 * 14, "bytes in 1 s = %d (want %d)", total, 250 * 26 + 5 * 14);

    /* link lost: nothing is sent */
    fb_crsf_set_link(rx, -110, 0, -10, false);
    fb_crsf_tick(rx, 500000000ull);
    CHECK(fb_crsf_tx_pending(rx) == 0, "silence while link down");
    fb_crsf_set_link(rx, -70, 90, 5, true);
    fb_crsf_tick(rx, 4000000ull);
    CHECK(fb_crsf_tx_pending(rx) > 0, "frames resume after link up");

    /* telemetry from the FC: battery 25.2 V, 12.3 A, 1234 mAh, 77 % */
    uint8_t bat[8] = { 0x00, 0xFC, 0x00, 0x7B, 0x00, 0x04, 0xD2, 77 };
    uint8_t frame[64];
    int fl = fb_crsf_build_frame(frame, FB_CRSF_BATTERY, bat, 8);
    for (int i = 0; i < fl; i++) fb_crsf_rx_byte(rx, frame[i]);
    uint8_t out[64]; uint32_t cnt;
    int len = fb_crsf_telemetry_last(rx, FB_CRSF_BATTERY, out, sizeof out, &cnt);
    CHECK(len == 8 && cnt == 1, "battery payload len=%d count=%u", len, cnt);
    float v, a; uint32_t mah; uint8_t pct;
    CHECK(fb_crsf_decode_battery(out, len, &v, &a, &mah, &pct) && fabsf(v - 25.2f) < 0.01f && fabsf(a - 12.3f) < 0.01f && mah == 1234 && pct == 77,
          "battery decode v=%.1f a=%.1f mah=%u pct=%u", v, a, mah, pct);

    /* attitude: pitch 0.1 rad (1000), roll -0.2 rad, yaw 1.0 rad */
    uint8_t att[6] = { 0x03, 0xE8, 0xF8, 0x30, 0x27, 0x10 };
    fl = fb_crsf_build_frame(frame, FB_CRSF_ATTITUDE, att, 6);
    for (int i = 0; i < fl; i++) fb_crsf_rx_byte(rx, frame[i]);
    float p, r, y;
    len = fb_crsf_telemetry_last(rx, FB_CRSF_ATTITUDE, out, sizeof out, &cnt);
    CHECK(fb_crsf_decode_attitude(out, len, &p, &r, &y) && fabsf(p - 5.73f) < 0.01f && fabsf(r + 11.46f) < 0.01f && fabsf(y - 57.30f) < 0.01f,
          "attitude decode p=%.2f r=%.2f y=%.2f", p, r, y);

    /* corrupted frame counts as crc error, garbage between frames is skipped */
    frame[5] ^= 0xFF;
    fb_crsf_rx_byte(rx, 0x00); fb_crsf_rx_byte(rx, 0x55);
    for (int i = 0; i < fl; i++) fb_crsf_rx_byte(rx, frame[i]);
    CHECK(fb_crsf_rx_crc_errors(rx) == 1 && fb_crsf_rx_frames(rx) == 2, "crc errors=%u frames=%u", fb_crsf_rx_crc_errors(rx), fb_crsf_rx_frames(rx));
    CHECK(fb_crsf_telemetry_last(rx, FB_CRSF_GPS, out, sizeof out, &cnt) == -1 && cnt == 0, "unseen type");

    fb_crsf_free(rx);

    /* frame pacing (the emulator's one deadline per frame): tick exactly at next_due, the FC then
     * reads the whole burst. 1 s at 250 Hz: 250 RC frames on the 4 ms grid, 5 link statistics on
     * the 200 ms grid, every burst made of whole frames */
    rx = fb_crsf_new();
    fb_crsf_set_rates(rx, 250, 200);
    uint64_t t = 0;
    int rc = 0, ls = 0, deadlines = 0, off_grid = 0, torn = 0;
    fb_crsf_tick(rx, 0);
    fb_crsf_tx_discard(rx);                                   /* the t=0 frames: counted above */
    for (;;) {
        uint64_t due = fb_crsf_next_due_ns(rx);
        CHECK(due > 0, "next_due right after a tick = 0 at t=%llu", (unsigned long long)t);
        if (t + due > 1000000000ull) break;
        t += due;
        deadlines++;
        CHECK(fb_crsf_tx_discard(rx) == 0, "stale bytes at t=%llu", (unsigned long long)t);
        fb_crsf_tick(rx, due);
        n = fb_crsf_tx_pop(rx, buf, sizeof buf);
        for (int i = 0; i < n; ) {
            if (buf[i] != 0xC8 || i + buf[i + 1] + 2 > n) { torn++; break; }
            if (buf[i + 2] == FB_CRSF_RC_CHANNELS) { rc++; off_grid += t % 4000000ull != 0; }
            if (buf[i + 2] == FB_CRSF_LINK_STATISTICS) { ls++; off_grid += t % 200000000ull != 0; }
            i += buf[i + 1] + 2;
        }
    }
    CHECK(rc == 250 && ls == 5, "paced: %d RC frames, %d link statistics in 1 s (want 250, 5)", rc, ls);
    CHECK(deadlines == 250, "paced: %d deadlines in 1 s (want 250: one per frame time)", deadlines);
    CHECK(off_grid == 0 && torn == 0, "paced: %d frames off their grid, %d torn bursts", off_grid, torn);
    /* a frame the FC never read is dropped at the next frame time (overrun on the wire) */
    t = fb_crsf_next_due_ns(rx);
    fb_crsf_tick(rx, t);
    CHECK(fb_crsf_tx_pending(rx) == 26, "one RC frame queued: %d", fb_crsf_tx_pending(rx));
    CHECK(fb_crsf_tx_discard(rx) == 26 && fb_crsf_tx_pending(rx) == 0, "stale frame discarded");
    /* link lost: silent, the model is still looked at once per packet period; link back: resumes */
    fb_crsf_set_fault(rx, "link_lost", true, NULL);
    for (int k = 0; k < 10; k++) {
        uint64_t due = fb_crsf_next_due_ns(rx);
        CHECK(due == 4000000ull, "link lost: next_due %llu", (unsigned long long)due);
        fb_crsf_tick(rx, due);
    }
    CHECK(fb_crsf_tx_pending(rx) == 0, "link lost: %d bytes queued", fb_crsf_tx_pending(rx));
    fb_crsf_set_fault(rx, "link_lost", false, NULL);
    CHECK(fb_crsf_next_due_ns(rx) == 0, "link back: due now (%llu)", (unsigned long long)fb_crsf_next_due_ns(rx));
    fb_crsf_tick(rx, 0);
    CHECK(fb_crsf_tx_pending(rx) == 26 + 14, "link back: RC + link statistics queued (%d)", fb_crsf_tx_pending(rx));
    fb_crsf_free(rx);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
