/* Host-side test of the MSP DisplayPort OSD device: the byte stream RotorFlight 4.6's
 * io/displayport_msp.c sends (MSP v1 "$M>" pushes of MSP_DISPLAYPORT 182: heartbeat, clear, write
 * string [row][col][attr][chars], draw), the double buffer, deltas, the canvas announcement
 * (MSP_SET_OSD_CANVAS 188 as "$M<" request), silence -> new contact, release, link_lost. */
#include "fb/device.h"
#include "fb/msp_osd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void feed(fb_msp_osd_t *d, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) fb_msp_osd_rx_byte(d, b[i]);
}

/* one MSP_DISPLAYPORT push as the firmware's output(): mspSerialPush(..., MSP_DIRECTION_REPLY) */
static void dp(fb_msp_osd_t *d, const uint8_t *p, int n)
{
    uint8_t f[300];
    feed(d, f, fb_msp_v1_frame(f, '>', FB_MSP_DISPLAYPORT, p, n));
}

static void sub(fb_msp_osd_t *d, uint8_t s) { dp(d, &s, 1); }

/* writeString(): buf[0] = 3, row, col, attr, chars (at most 30) */
static void wr(fb_msp_osd_t *d, int row, int col, uint8_t attr, const char *s)
{
    uint8_t p[40] = { FB_MSP_DP_WRITE_STRING, (uint8_t)row, (uint8_t)col, attr };
    int n = (int)strlen(s);
    memcpy(p + 4, s, n);
    dp(d, p, n + 4);
}

static int nchanged(fb_msp_osd_t *d)
{
    int k = 0;
    for (int r = 0; r < fb_msp_osd_rows(d); r++)
        for (int c = 0; c < fb_msp_osd_cols(d); c++) k += fb_msp_osd_changed(d, r, c);
    return k;
}

static void row_text(fb_msp_osd_t *d, int r, int c0, int n, char *out)
{
    for (int i = 0; i < n; i++) out[i] = (char)fb_msp_osd_char(d, r, c0 + i);
    out[n] = 0;
}

/* one firmware OSD frame (heartbeat, clear, a write, draw) at the current time, then dt of silence */
static void osd_frame(fb_msp_osd_t *d, const char *s, uint64_t dt_ns)
{
    sub(d, FB_MSP_DP_HEARTBEAT);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 1, 1, 0, s);
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_tick(d, dt_ns);
}

/* contact loss adapts to the frame rate; an MCU reset is a clean new contact */
static void test_silence(void)
{
    const uint64_t MS = 1000000ull;
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    bool full;
    uint8_t tx[64];

    /* 12 Hz: 83 ms apart -> 3 x 83 < 250 -> 250 ms */
    for (int i = 0; i < 5; i++) osd_frame(d, "ACRO", 83 * MS);
    CHECK(fb_msp_osd_draw_interval_ns(d) == 83 * MS, "interval %llu", (unsigned long long)fb_msp_osd_draw_interval_ns(d));
    CHECK(fb_msp_osd_silence_ns(d) == FB_MSP_OSD_SILENCE_NS, "12 Hz threshold 250 ms");
    fb_msp_osd_tick(d, 170 * MS);                       /* 253 ms since the last frame */
    CHECK(!fb_msp_osd_link_up(d), "12 Hz: 253 ms of silence ends the contact");
    fb_msp_osd_free(d);

    /* 2 Hz (osd_framerate_hz = 2): 500 ms apart never ends the contact; the threshold is 1.5 s */
    d = fb_msp_osd_new(18, 50);
    CHECK(fb_msp_osd_silence_ns(d) == 3000 * MS, "before the first interval: 3 s (1 Hz, the slowest rate)");
    for (int i = 0; i < 20; i++) {
        osd_frame(d, i & 1 ? "ACRO" : "ANGL", 500 * MS);
        CHECK(fb_msp_osd_link_up(d), "2 Hz frame %d: link up", i);
    }
    /* an extra DRAW 1 ms after a frame (the firmware redraws when it gets the canvas) */
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_tick(d, 1 * MS);
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_tick(d, 499 * MS);
    osd_frame(d, "ACRO", 500 * MS);
    CHECK(fb_msp_osd_link_up(d) && fb_msp_osd_silence_ns(d) == 1500 * MS,
          "2 Hz: an extra DRAW 1 ms after a frame is not a frame interval");
    CHECK(fb_msp_osd_stats(d)->contacts == 1, "2 Hz: contacts %u (no flapping)", fb_msp_osd_stats(d)->contacts);
    CHECK(fb_msp_osd_silence_ns(d) == 1500 * MS, "2 Hz threshold %llu", (unsigned long long)fb_msp_osd_silence_ns(d));
    fb_msp_osd_tick(d, 1001 * MS);                      /* 1.501 s since the last frame */
    CHECK(!fb_msp_osd_link_up(d), "2 Hz: 1.5 s of silence ends the contact");
    fb_msp_osd_free(d);

    /* an MCU reset reported by the emulator: the next frame is a new contact even after 83 ms */
    d = fb_msp_osd_new(18, 50);
    for (int i = 0; i < 3; i++) osd_frame(d, "ACRO", 83 * MS);
    while (fb_msp_osd_take_frame(d, &full)) {}
    fb_msp_osd_tx_pop(d, tx, sizeof tx);
    fb_msp_osd_contact_lost(d);
    CHECK(!fb_msp_osd_link_up(d), "contact_lost -> link down at once");
    osd_frame(d, "ACRO", 83 * MS);
    CHECK(fb_msp_osd_stats(d)->contacts == 2 && fb_msp_osd_tx_pending(d) == 8, "reset: new contact, canvas announced");
    CHECK(fb_msp_osd_take_frame(d, &full) && full && nchanged(d) == 4, "reset: the next frame is full");
    CHECK(fb_msp_osd_draw_interval_ns(d) == 0 && fb_msp_osd_silence_ns(d) == 3000 * MS,
          "a reset forgets the interval (the new boot may run another rate)");
    fb_msp_osd_free(d);

    /* boot: 3 DRAWs within 2 ms, then 2 Hz: the short gaps are not frame intervals */
    d = fb_msp_osd_new(18, 50);
    for (int i = 0; i < 3; i++) osd_frame(d, "BOOT", 1 * MS);
    CHECK(fb_msp_osd_draw_interval_ns(d) == 0, "gaps < 16 ms ignored");
    fb_msp_osd_tick(d, 497 * MS);
    for (int i = 0; i < 5; i++) osd_frame(d, "ACRO", 500 * MS);
    CHECK(fb_msp_osd_link_up(d) && fb_msp_osd_stats(d)->contacts == 1 && fb_msp_osd_silence_ns(d) == 1500 * MS,
          "boot burst then 2 Hz: one contact, threshold 1.5 s");
    fb_msp_osd_free(d);

    /* a firmware that became slower than the threshold (12 Hz measured, then 2 Hz): one lost
     * contact, the gap across it is measured, then stable */
    d = fb_msp_osd_new(18, 50);
    for (int i = 0; i < 8; i++) osd_frame(d, "ACRO", 83 * MS);
    fb_msp_osd_tick(d, 417 * MS);
    CHECK(!fb_msp_osd_link_up(d), "slower firmware: the first 500 ms gap ends the contact");
    for (int i = 0; i < 10; i++) osd_frame(d, "ACRO", 500 * MS);
    CHECK(fb_msp_osd_link_up(d) && fb_msp_osd_stats(d)->contacts == 2 && fb_msp_osd_silence_ns(d) == 1500 * MS,
          "slower firmware: contacts %u, then stable", fb_msp_osd_stats(d)->contacts);
    fb_msp_osd_free(d);
}

static uint8_t xor_ck(const uint8_t *b, int n)
{
    uint8_t ck = 0;
    for (int i = 0; i < n; i++) ck ^= b[i];
    return ck;
}

static void test_parser(void)
{
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    bool full;
    uint8_t f[600];
    int n;

    /* a frame cut short (the FC reset mid-frame: size 10, 4 payload bytes), then valid frames: the
     * valid CLEAR is read as the cut frame's payload, the checksum fails, the bytes are rescanned
     * from the next '$' -> nothing lost */
    const uint8_t cut[] = { '$', 'M', '>', 10, FB_MSP_DISPLAYPORT, FB_MSP_DP_WRITE_STRING, 5, 5, 0 };
    feed(d, cut, sizeof cut);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 6, 6, 0, "OK");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_stats(d)->bad_checksum == 1, "cut frame: one bad checksum (%u)", fb_msp_osd_stats(d)->bad_checksum);
    CHECK(fb_msp_osd_stats(d)->clears == 1 && fb_msp_osd_stats(d)->writes == 1 && fb_msp_osd_stats(d)->frames == 1,
          "cut frame: the next frames are all parsed (clears %u writes %u frames %u)",
          fb_msp_osd_stats(d)->clears, fb_msp_osd_stats(d)->writes, fb_msp_osd_stats(d)->frames);
    CHECK(fb_msp_osd_take_frame(d, &full) && fb_msp_osd_char(d, 6, 6) == 'O' && fb_msp_osd_char(d, 5, 5) == ' ',
          "cut frame: the valid frame is shown, the cut one is not");

    /* jumbo form: size byte 255, command, 16-bit size (little endian), payload, xor over all */
    uint8_t p[300] = { FB_MSP_DP_WRITE_STRING, 7, 2, 0, 'J', 'U', 'M', 'B', 'O' };
    int pn = 9;
    n = 0;
    f[n++] = '$'; f[n++] = 'M'; f[n++] = '>'; f[n++] = 255; f[n++] = FB_MSP_DISPLAYPORT;
    f[n++] = (uint8_t)pn; f[n++] = 0;
    memcpy(f + n, p, pn); n += pn;
    f[n] = xor_ck(f + 3, n - 3); n++;
    feed(d, f, n);
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    char t[16];
    fb_msp_osd_take_frame(d, &full);
    row_text(d, 7, 2, 5, t);
    CHECK(fb_msp_osd_stats(d)->jumbo == 1 && !strcmp(t, "JUMBO"), "jumbo frame parsed ('%s')", t);
    /* a jumbo frame longer than 256 bytes: read to its end and dropped, the next frame is fine */
    memset(p + 4, 'X', sizeof p - 4);
    n = 0;
    f[n++] = '$'; f[n++] = 'M'; f[n++] = '>'; f[n++] = 255; f[n++] = FB_MSP_DISPLAYPORT;
    f[n++] = (uint8_t)(300 & 0xff); f[n++] = 300 >> 8;
    memcpy(f + n, p, 300); n += 300;
    f[n] = xor_ck(f + 3, n - 3); n++;
    uint32_t writes = fb_msp_osd_stats(d)->writes;
    feed(d, f, n);
    wr(d, 8, 2, 0, "NEXT");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_take_frame(d, &full);
    row_text(d, 8, 2, 4, t);
    CHECK(fb_msp_osd_stats(d)->oversize == 1 && fb_msp_osd_stats(d)->writes == writes + 1 && !strcmp(t, "NEXT") &&
          fb_msp_osd_char(d, 7, 10) != 'X', "oversize jumbo dropped, next frame parsed ('%s')", t);

    /* $M< MSP_DISPLAYPORT (request direction) is not the firmware's push: counted, not drawn */
    uint32_t frames = fb_msp_osd_stats(d)->frames, other = fb_msp_osd_stats(d)->other_frames;
    uint8_t w[] = { FB_MSP_DP_WRITE_STRING, 9, 9, 0, 'R', 'Q' }, dr = FB_MSP_DP_DRAW_SCREEN;
    feed(d, f, fb_msp_v1_frame(f, '<', FB_MSP_DISPLAYPORT, w, sizeof w));
    feed(d, f, fb_msp_v1_frame(f, '<', FB_MSP_DISPLAYPORT, &dr, 1));
    CHECK(fb_msp_osd_stats(d)->frames == frames && fb_msp_osd_stats(d)->other_frames == other + 2 &&
          !fb_msp_osd_take_frame(d, &full), "$M< DISPLAYPORT ignored");

    /* start column 250: every character outside the canvas (no wrap to column 0) */
    uint32_t clipped = fb_msp_osd_stats(d)->clipped;
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 3, 250, 0, "ABCDEFGH");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_take_frame(d, &full);
    CHECK(fb_msp_osd_stats(d)->clipped == clipped + 8 && fb_msp_osd_char(d, 3, 0) == ' ' && fb_msp_osd_char(d, 3, 4) == ' ',
          "column 250: 8 clipped, no wrap");

    /* NUL and 0xFF are glyph codes like any other */
    uint8_t z[] = { FB_MSP_DP_WRITE_STRING, 4, 4, 0, 0x00, 0xFF, 'A' };
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    dp(d, z, sizeof z);
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && fb_msp_osd_char(d, 4, 4) == 0x00 && fb_msp_osd_char(d, 4, 5) == 0xFF &&
          fb_msp_osd_char(d, 4, 6) == 'A' && fb_msp_osd_changed(d, 4, 4), "NUL and 0xFF kept");
    fb_msp_osd_free(d);
}

/* the `osd` message's cells: escaping of every byte class, runs split on attr changes */
static void test_json_cells(void)
{
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    bool full;
    char js[4096];
    /* row 2: 0x00 0x7F 0x80 0xFF '"' '\' 'a' with attr 0, then "b" with attr 0x40, then "c" attr 0 */
    uint8_t a[] = { FB_MSP_DP_WRITE_STRING, 2, 3, 0, 0x00, 0x7F, 0x80, 0xFF, '"', '\\', 'a' };
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    dp(d, a, sizeof a);
    wr(d, 2, 10, 0x40, "b");
    wr(d, 2, 11, 0, "c");
    wr(d, 17, 49, 1, "~");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && full, "json: full frame");
    size_t n = fb_msp_osd_cells_json(d, true, js, sizeof js);
    const char *want =
        "[{\"r\":2,\"c\":3,\"s\":\"\\u0000\\u007f\\u0080\\u00ff\\\"\\\\a\",\"attr\":0},"
        "{\"r\":2,\"c\":10,\"s\":\"b\",\"attr\":64},"
        "{\"r\":2,\"c\":11,\"s\":\"c\",\"attr\":0},"
        "{\"r\":17,\"c\":49,\"s\":\"~\",\"attr\":1}]";
    CHECK(n == strlen(want) && !strcmp(js, want), "json full:\n  got  %s\n  want %s", js, want);
    /* snprintf-like: a short buffer is cut and terminated, the length is still the full one */
    char small[8];
    CHECK(fb_msp_osd_cells_json(d, true, small, sizeof small) == n && strlen(small) == 7 && !memcmp(small, want, 7),
          "json: short buffer");
    CHECK(fb_msp_osd_cells_json(d, true, NULL, 0) == n, "json: length only");
    /* delta: 'a' -> 'A', "b" gone (a blank with attr 0, next to 'c' with attr 0: one run) */
    uint8_t a2[] = { FB_MSP_DP_WRITE_STRING, 2, 3, 0, 0x00, 0x7F, 0x80, 0xFF, '"', '\\', 'A' };
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    dp(d, a2, sizeof a2);
    wr(d, 2, 11, 0, "c");
    wr(d, 17, 49, 1, "~");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && !full, "json: delta");
    fb_msp_osd_cells_json(d, false, js, sizeof js);
    want = "[{\"r\":2,\"c\":9,\"s\":\"A \",\"attr\":0}]";
    CHECK(!strcmp(js, want), "json delta:\n  got  %s\n  want %s", js, want);
    /* nothing changed: [] */
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_take_frame(d, &full);
    CHECK(fb_msp_osd_cells_json(d, false, js, sizeof js) == 2 && !strcmp(js, "[]"), "json: empty delta");
    fb_msp_osd_free(d);
}

/* the VTX talks: MSP_FC_VARIANT every 125 ms from power-on (iNav sends nothing until it hears the
 * VTX), one request queued at a time; the reply names the FC and the font; OPTIONS kept */
static void test_poll(void)
{
    const uint64_t MS = 1000000ull;
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    uint8_t tx[64], req[8];
    int rn = fb_msp_v1_frame(req, '<', FB_MSP_FC_VARIANT, NULL, 0);
    CHECK(rn == 6 && !memcmp(req, "$M<\x00\x02\x02", 6), "FC_VARIANT request bytes");
    CHECK(fb_msp_osd_poll_in_ns(d) == FB_MSP_OSD_POLL_NS, "first request 125 ms after power-on");
    fb_msp_osd_tick(d, 124 * MS);
    CHECK(fb_msp_osd_tx_pending(d) == 0, "nothing before 125 ms");
    fb_msp_osd_tick(d, 1 * MS);
    CHECK(fb_msp_osd_tx_pop(d, tx, sizeof tx) == 6 && !memcmp(tx, req, 6), "request at 125 ms");
    CHECK(fb_msp_osd_poll_in_ns(d) == FB_MSP_OSD_POLL_NS, "next in 125 ms");
    /* the FC's USART does not take bytes (not open yet): no pile-up; a late tick does not catch up */
    fb_msp_osd_tick(d, 1000 * MS);
    CHECK(fb_msp_osd_tx_pending(d) == 6 && fb_msp_osd_stats(d)->polls == 2, "one queued (%d)", fb_msp_osd_tx_pending(d));
    CHECK(fb_msp_osd_poll_in_ns(d) == FB_MSP_OSD_POLL_NS, "on the 125 ms grid (%llu)",
          (unsigned long long)fb_msp_osd_poll_in_ns(d));
    fb_msp_osd_tick(d, FB_MSP_OSD_POLL_NS);
    CHECK(fb_msp_osd_stats(d)->polls == 2, "not while the last one waits");
    fb_msp_osd_tx_pop(d, tx, sizeof tx);
    CHECK(!*fb_msp_osd_fc_variant(d) && !fb_msp_osd_font(d), "no variant yet");
    /* the reply: a new contact (the canvas is announced) and the variant */
    uint8_t f[16];
    feed(d, f, fb_msp_v1_frame(f, '>', FB_MSP_FC_VARIANT, (const uint8_t *)"INAV", 4));
    CHECK(!strcmp(fb_msp_osd_fc_variant(d), "INAV") && !strcmp(fb_msp_osd_font(d), "inav") &&
          fb_msp_osd_stats(d)->variants == 1 && fb_msp_osd_stats(d)->contacts == 1, "INAV");
    CHECK(fb_msp_osd_tx_pending(d) == 8, "canvas announced on the reply's contact");
    /* iNav has no MSP_SET_OSD_CANVAS: its error reply is counted */
    feed(d, f, fb_msp_v1_frame(f, '!', FB_MSP_SET_OSD_CANVAS, NULL, 0));
    CHECK(fb_msp_osd_stats(d)->errors == 1 && fb_msp_osd_stats(d)->canvas_acks == 0, "error reply");
    /* OPTIONS [font][mode] as iNav's setDisplayMode sends it */
    int font = -1, mode = -1;
    CHECK(!fb_msp_osd_options(d, &font, &mode), "no OPTIONS yet");
    uint8_t opt[3] = { FB_MSP_DP_OPTIONS, 0, 1 };
    dp(d, opt, 3);
    CHECK(fb_msp_osd_options(d, &font, &mode) && font == 0 && mode == 1, "OPTIONS font 0 mode 1 (HD 50x18)");
    /* an MCU reset may bring other firmware: the variant is asked again */
    fb_msp_osd_contact_lost(d);
    CHECK(!*fb_msp_osd_fc_variant(d) && !fb_msp_osd_options(d, NULL, NULL), "forgotten on contact loss");
    feed(d, f, fb_msp_v1_frame(f, '>', FB_MSP_FC_VARIANT, (const uint8_t *)"RTFL", 4));
    CHECK(!strcmp(fb_msp_osd_font(d), "rotorflight"), "RTFL");
    /* link_lost: no requests */
    fb_msp_osd_tx_pop(d, tx, sizeof tx);
    fb_msp_osd_set_fault(d, "link_lost", true, NULL);
    fb_msp_osd_tick(d, 1000 * MS);
    CHECK(fb_msp_osd_tx_pending(d) == 0, "silent device polls nothing");
    fb_msp_osd_free(d);
    /* poll off (a RotorFlight board's machine): no requests, nothing due; kept across reset */
    d = fb_msp_osd_new(18, 50);
    fb_msp_osd_set_poll(d, false);
    fb_msp_osd_reset(d);
    fb_msp_osd_tick(d, 1000 * MS);
    CHECK(fb_msp_osd_tx_pending(d) == 0 && fb_msp_osd_stats(d)->polls == 0 &&
          fb_msp_osd_poll_in_ns(d) == UINT64_MAX, "poll off: nothing sent, nothing due");
    fb_msp_osd_set_poll(d, true);
    fb_msp_osd_tick(d, 1 * MS);
    CHECK(fb_msp_osd_tx_pending(d) == 6 && fb_msp_osd_stats(d)->polls == 1, "poll on again: the missed request at once");
    fb_msp_osd_free(d);
}

/* ---- iNav 9.1.0's real stream (core/qemu/evidence/inav-osd/, inav_osd_evidence.py) ------------ */

/* the evidence directory: FB_OSD_STREAMS or the repo's, relative to devices/ (make test runs there) */
static const char *streams_dir(void)
{
    const char *e = getenv("FB_OSD_STREAMS");
    return e && *e ? e : "../core/qemu/evidence/inav-osd";
}

static unsigned char *slurp(const char *name, long *n)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", streams_dir(), name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*n + 1);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
        free(b);
        b = NULL;
    }
    if (b) b[*n] = 0;
    fclose(f);
    return b;
}

/* feed the stream until DRAW number upto (0: all of it); returns the bytes fed */
static long replay(fb_msp_osd_t *d, const unsigned char *b, long n, uint32_t upto)
{
    long i = 0;
    while (i < n && (!upto || fb_msp_osd_stats(d)->frames < upto)) {
        fb_msp_osd_rx_byte(d, b[i++]);
    }
    return i;
}

/* the glyph iNav drew: its page is in the attr's low 2 bits (displayport_msp_osd.c drawScreen) */
static int glyph(const fb_msp_osd_t *d, int r, int c)
{
    return (fb_msp_osd_attr(d, r, c) & 3) << 8 | fb_msp_osd_char(d, r, c);
}

static bool text_at(const fb_msp_osd_t *d, int r, int c, const char *s)
{
    for (int i = 0; s[i]; i++) {
        if (fb_msp_osd_char(d, r, c + i) != (uint8_t)s[i] || fb_msp_osd_attr(d, r, c + i)) return false;
    }
    return true;
}

static void test_inav_boot_stream(void)
{
    long n = 0, cn = 0;
    unsigned char *b = slurp("stream-boot.bin", &n);
    char *cv = (char *)slurp("stream-boot.canvas.txt", &cn);
    CHECK(b && cv, "the captured iNav stream and its canvas under %s", streams_dir());
    if (!b || !cv) {
        free(b);
        free(cv);
        return;
    }
    /* the DRAW the core's last osd message came from, then 18 rows x 50 cells "AACC" */
    const char *p = strstr(cv, "\nframe ");
    unsigned frame = p ? (unsigned)strtoul(p + 7, NULL, 10) : 0;
    CHECK(frame > 10, "canvas file: frame %u", frame);
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    long fed = replay(d, b, n, frame);
    bool full = false;
    CHECK(fb_msp_osd_take_frame(d, &full) && full, "a frame, full (the first take of the contact)");
    const fb_msp_osd_stats_t *st = fb_msp_osd_stats(d);
    CHECK(st->frames == frame && fed <= n, "replayed to DRAW %u (%ld of %ld bytes)", st->frames, fed, n);
    /* the canvas the app got through the core's osd messages = the replay's */
    p = strchr(p + 1, '\n');
    int bad = 0, cells = 0, page1 = 0, attr_other = 0;
    for (int r = 0; p && r < 18; r++) {
        for (int c = 0; c < 50; c++) {
            unsigned v = (unsigned)strtoul(p + 1, (char **)&p, 16);
            unsigned got = (unsigned)fb_msp_osd_attr(d, r, c) << 8 | fb_msp_osd_char(d, r, c);
            cells++;
            if (got != v && bad++ < 5) printf("  cell (%d,%d): replay %04x, osd messages %04x\n", r, c, got, v);
            page1 += (fb_msp_osd_attr(d, r, c) & 3) == 1;
            attr_other += fb_msp_osd_attr(d, r, c) > 1;
        }
    }
    CHECK(cells == 18 * 50 && !bad, "replay == the core's canvas (%d cells, %d differ)", cells, bad);
    /* what iNav sends (displayport_msp_osd.c): MSP v1 only, attr = the font page (0 or 1 on this
     * font), never blink or the version bit, no RELEASE / SYS, OPTIONS [font 0][mode 1 = HD 50x18],
     * nothing outside the 50x18 canvas */
    int font = -1, mode = -1;
    CHECK(st->bad_checksum == 0 && st->jumbo == 0 && st->unknown_sub == 0 && st->clipped == 0 &&
          st->releases == 0 && st->sys == 0, "clean v1 stream (bad %u jumbo %u unknown %u clipped %u)",
          st->bad_checksum, st->jumbo, st->unknown_sub, st->clipped);
    CHECK(fb_msp_osd_options(d, &font, &mode) && font == 0 && mode == 1, "OPTIONS font %d mode %d", font, mode);
    CHECK(!attr_other && page1 > 0, "attrs 0/1 only, page-1 glyphs on the canvas (%d)", page1);
    CHECK(!strcmp(fb_msp_osd_fc_variant(d), "INAV") && !strcmp(fb_msp_osd_font(d), "inav"),
          "the FC's MSP_FC_VARIANT replies in the stream: INAV");
    CHECK(st->heartbeats > st->frames && st->clears >= 1, "heartbeats %u, clears %u", st->heartbeats, st->clears);
    /* the elements where the device's iNav lines put them (msp_osd.c msp_osd_config_inav), with iNav's
     * symbol codes (drivers/osd_symbols.h); the trainer disarmed on the ground, 16 satellites */
    CHECK(text_at(d, 1, 2, "ACRO"), "FLYMODE at (2,1)");
    CHECK(glyph(d, 2, 2) == 0x08 && glyph(d, 2, 3) == 0x09 && text_at(d, 2, 4, "16"),
          "GPS_SATS at (2,2): SYM_SAT_L SYM_SAT_R 16");
    CHECK(glyph(d, 1, 23) == 0x0C && glyph(d, 1, 27) == 0x0B, "HEADING at (23,1): SYM_HEADING ... SYM_DEGREES");
    CHECK(glyph(d, 1, 44) == 0x01, "RSSI_VALUE at (44,1): SYM_RSSI");
    CHECK(glyph(d, 9, 11) == 0x90 && glyph(d, 9, 40) == 0x76, "GPS_SPEED ... SYM_KMH, ALTITUDE ... SYM_ALT_M");
    CHECK(glyph(d, 16, 2) == 0x63 && glyph(d, 16, 6) == 0x1F, "MAIN_BATT_VOLTAGE at (2,16): SYM_BATT_FULL ... SYM_VOLT");
    bool msg = false;
    for (int c = 0; c < 30; c++) msg |= text_at(d, 15, c, "THROTTLE IS NOT LOW");
    CHECK(msg, "MESSAGES on row 15");
    /* the HUD at the canvas centre (osdCrosshairPosition: 50 / 2, 18 / 2), page-1 glyphs: the
     * crosshairs SYM_AH_CH_LEFT / CENTER / RIGHT, the sidebars' SYM_AH_DECORATION 7 columns out and
     * 3 rows up and down, the horizon line from SYM_AH_H_START..+8 */
    CHECK(glyph(d, 9, 24) == 0x13A && glyph(d, 9, 25) == 0x166 && glyph(d, 9, 26) == 0x13B, "crosshairs at (25,9)");
    int deco = 0, ahi = 0;
    for (int r = 6; r <= 12; r++) deco += (glyph(d, r, 18) == 0x131) + (glyph(d, r, 32) == 0x131);
    for (int r = 5; r <= 13; r++)
        for (int c = 19; c <= 31; c++) ahi += glyph(d, r, c) >= 0x14C && glyph(d, r, c) <= 0x154;
    CHECK(deco == 14 && ahi >= 9, "sidebars %d cells, horizon %d cells", deco, ahi);
    fb_msp_osd_free(d);
    free(b);
    free(cv);
}

/* the plane-launch flight's stream: every DRAW taken as the core does; the horizon moves with the
 * attitude, the mode is ANGLE, nothing leaves the canvas or the 2 pages */
static void test_inav_flight_stream(void)
{
    long n = 0;
    unsigned char *b = slurp("stream-flight.bin", &n);
    CHECK(b, "the captured iNav flight stream under %s", streams_dir());
    if (!b) {
        return;
    }
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    bool full;
    int takes = 0, angl = 0, bad_attr = 0, rows_seen = 0;
    unsigned horizon_rows = 0;      /* bit r: the horizon crossed column 25 on row r in some frame */
    for (long i = 0; i < n; i++) {
        fb_msp_osd_rx_byte(d, b[i]);
        if (!fb_msp_osd_take_frame(d, &full)) continue;
        takes++;
        angl += text_at(d, 1, 2, "ANGL");
        for (int r = 0; r < 18; r++) {
            for (int c = 0; c < 50; c++) bad_attr += fb_msp_osd_attr(d, r, c) > 1;
            int g = glyph(d, r, 22);
            if (g >= 0x14C && g <= 0x154) horizon_rows |= 1u << r;
        }
    }
    for (int r = 0; r < 18; r++) rows_seen += (horizon_rows >> r) & 1;
    const fb_msp_osd_stats_t *st = fb_msp_osd_stats(d);
    CHECK(takes == (int)st->frames && takes > 1000, "every DRAW taken (%d)", takes);
    CHECK(st->bad_checksum == 0 && st->clipped == 0 && st->unknown_sub == 0 && !bad_attr,
          "clean (bad %u clipped %u attr>1 %d)", st->bad_checksum, st->clipped, bad_attr);
    CHECK(angl > 100 && rows_seen >= 3, "ANGL in %d frames; the horizon on %d rows", angl, rows_seen);
    CHECK(!strcmp(fb_msp_osd_font(d), "inav"), "INAV");
    fb_msp_osd_free(d);
    free(b);
}

int main(void)
{
    fb_msp_osd_t *d = fb_msp_osd_new(18, 50);
    CHECK(fb_msp_osd_rows(d) == 18 && fb_msp_osd_cols(d) == 50, "canvas %dx%d", fb_msp_osd_rows(d), fb_msp_osd_cols(d));
    bool full = false;
    CHECK(!fb_msp_osd_take_frame(d, &full), "no frame before any DRAW");

    /* frame builder: the XOR checksum over len, cmd, payload */
    uint8_t f[16];
    uint8_t cv[2] = { 50, 18 };
    int n = fb_msp_v1_frame(f, '<', FB_MSP_SET_OSD_CANVAS, cv, 2);
    CHECK(n == 8 && !memcmp(f, "$M<\x02\xbc\x32\x12", 7) && f[7] == (0x02 ^ 0xbc ^ 0x32 ^ 0x12), "canvas request bytes");

    /* first contact: heartbeat -> the device announces its canvas once */
    sub(d, FB_MSP_DP_HEARTBEAT);
    uint8_t tx[64];
    n = fb_msp_osd_tx_pop(d, tx, sizeof tx);
    CHECK(n == 8 && !memcmp(tx, f, 8), "canvas announced on contact (%d bytes)", n);
    CHECK(fb_msp_osd_link_up(d) && fb_msp_osd_stats(d)->contacts == 1, "contact");
    sub(d, FB_MSP_DP_HEARTBEAT);
    CHECK(fb_msp_osd_tx_pending(d) == 0, "announced once per contact");
    /* the FC's empty reply to MSP_SET_OSD_CANVAS */
    feed(d, f, fb_msp_v1_frame(f, '>', FB_MSP_SET_OSD_CANVAS, NULL, 0));
    CHECK(fb_msp_osd_stats(d)->canvas_acks == 1, "canvas ack");

    /* one OSD frame: clear, writes, draw -> full (first frame of the contact) */
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 1, 1, 0, "ANGL");
    wr(d, 8, 24, 0, "\x72\x73\x74");          /* crosshair symbols (non-ASCII font codes) */
    wr(d, 16, 1, 0x01, "22.2V");               /* font page 1 */
    CHECK(!fb_msp_osd_take_frame(d, &full), "nothing shown before DRAW");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && full, "first frame is full");
    CHECK(nchanged(d) == 4 + 3 + 5, "full frame marks the non-blank cells (%d)", nchanged(d));
    char t[64];
    row_text(d, 1, 1, 4, t);
    CHECK(!strcmp(t, "ANGL"), "row 1 '%s'", t);
    CHECK(fb_msp_osd_char(d, 8, 25) == 0x73 && fb_msp_osd_attr(d, 16, 1) == 0x01, "chars/attr kept as bytes");
    CHECK(!fb_msp_osd_take_frame(d, &full), "taken once");

    /* next frame: only the timer changes -> delta */
    sub(d, FB_MSP_DP_HEARTBEAT);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 1, 1, 0, "ANGL");
    wr(d, 8, 24, 0, "\x72\x73\x74");
    wr(d, 16, 1, 0x01, "22.1V");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && !full, "second frame is a delta");
    CHECK(nchanged(d) == 1 && fb_msp_osd_changed(d, 16, 4), "delta = the one changed cell (%d)", nchanged(d));

    /* an element that disappears: its cells come back blank in the delta */
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 8, 24, 0, "\x72\x73\x74");
    wr(d, 16, 1, 0x01, "22.1V");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && !full && nchanged(d) == 4 && fb_msp_osd_char(d, 1, 1) == ' ',
          "removed element = 4 blank cells (%d)", nchanged(d));

    /* blink bit and the version bit are kept as sent; clipping at the canvas edge */
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 0, 46, FB_MSP_OSD_ATTR_BLINK | 3, "ABCDEF");      /* cols 46..51: 2 clipped */
    wr(d, 18, 0, 0, "X");                                   /* row 18: outside 18 rows */
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_take_frame(d, &full);
    CHECK(fb_msp_osd_attr(d, 0, 49) == 0x43 && fb_msp_osd_char(d, 0, 49) == 'D', "attr 0x43 at the last column");
    CHECK(fb_msp_osd_stats(d)->clipped == 3, "clipped %u", fb_msp_osd_stats(d)->clipped);

    /* several DRAWs between takes count once, the delta is against the last take */
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 2, 2, 0, "A");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 2, 2, 0, "B");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && fb_msp_osd_char(d, 2, 2) == 'B' && !fb_msp_osd_take_frame(d, &full), "coalesced");

    /* writes without a CLEAR accumulate in the drawing buffer */
    wr(d, 3, 0, 0, "Z");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    fb_msp_osd_take_frame(d, &full);
    CHECK(fb_msp_osd_char(d, 2, 2) == 'B' && fb_msp_osd_char(d, 3, 0) == 'Z', "drawing buffer kept across DRAW");

    /* a bad checksum is dropped; garbage between frames is skipped */
    uint8_t bad[16];
    uint8_t p1 = FB_MSP_DP_DRAW_SCREEN;
    n = fb_msp_v1_frame(bad, '>', FB_MSP_DISPLAYPORT, &p1, 1);
    bad[n - 1] ^= 0x55;
    feed(d, (const uint8_t *)"xx$$M", 5);
    feed(d, bad, n);
    CHECK(fb_msp_osd_stats(d)->bad_checksum == 1 && !fb_msp_osd_take_frame(d, &full), "bad checksum dropped");

    /* silence -> the FC rebooted: new contact, canvas announced again, next frame full */
    /* (no clock between the frames above: every gap 0 < 16 ms, not measured: the threshold is 3 s) */
    CHECK(fb_msp_osd_silence_ns(d) == 3000000000ull, "unmeasured threshold %llu", (unsigned long long)fb_msp_osd_silence_ns(d));
    fb_msp_osd_tick(d, fb_msp_osd_silence_ns(d) + 1);
    CHECK(!fb_msp_osd_link_up(d), "silent -> link down");
    CHECK(fb_msp_osd_tx_pending(d) == 6, "the poll went on during the silence");
    fb_msp_osd_tx_pop(d, tx, sizeof tx);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    CHECK(fb_msp_osd_tx_pending(d) == 8, "announced again after silence");
    fb_msp_osd_tx_pop(d, tx, sizeof tx);
    wr(d, 5, 5, 0, "RTFL");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && full && nchanged(d) == 4, "full after silence (%d)", nchanged(d));
    CHECK(fb_msp_osd_stats(d)->contacts == 2, "contacts %u", fb_msp_osd_stats(d)->contacts);

    /* release: canvas blanked, the next frame full */
    sub(d, FB_MSP_DP_RELEASE);
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && full && nchanged(d) == 0, "release -> empty full frame");

    /* SYS and OPTIONS are counted, not drawn */
    uint8_t sys[4] = { FB_MSP_DP_SYS, 3, 3, 0 };
    dp(d, sys, 4);
    sub(d, FB_MSP_DP_OPTIONS);
    CHECK(fb_msp_osd_stats(d)->sys == 1 && fb_msp_osd_stats(d)->options == 1, "sys/options counted");

    /* link_lost: nothing is taken, nothing sent; off -> full frame again */
    CHECK(fb_msp_osd_set_fault(d, "link_lost", true, NULL) && fb_msp_osd_fault_active(d, "link_lost"), "fault on");
    CHECK(!fb_msp_osd_set_fault(d, "nope", true, NULL), "unknown fault");
    uint32_t frames = fb_msp_osd_stats(d)->frames;
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 1, 1, 0, "ACRO");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(!fb_msp_osd_take_frame(d, &full) && fb_msp_osd_stats(d)->frames == frames && !fb_msp_osd_link_up(d),
          "silent device");
    fb_msp_osd_set_fault(d, "link_lost", false, NULL);
    sub(d, FB_MSP_DP_CLEAR_SCREEN);
    wr(d, 1, 1, 0, "ACRO");
    sub(d, FB_MSP_DP_DRAW_SCREEN);
    CHECK(fb_msp_osd_take_frame(d, &full) && full && nchanged(d) == 4, "full after link_lost off");
    CHECK(fb_msp_osd_tx_pending(d) == 8, "canvas announced after link_lost off");

    /* power-on reset: blank, faults kept */
    fb_msp_osd_set_fault(d, "link_lost", true, NULL);
    fb_msp_osd_reset(d);
    CHECK(fb_msp_osd_fault_active(d, "link_lost") && fb_msp_osd_char(d, 1, 1) == ' ', "reset keeps faults");
    fb_msp_osd_free(d);

    test_silence();
    test_parser();
    test_json_cells();
    test_poll();
    test_inav_boot_stream();
    test_inav_flight_stream();

    /* descriptor: category osd, one uart port both ways, config lines with {serial} */
    const fb_device_desc_t *desc = fb_device_find("msp_osd");
    CHECK(desc && !strcmp(desc->category, "osd") && !strcmp(desc->kind, "uart"), "descriptor");
    CHECK(desc && desc->ports && !strcmp(desc->ports[0].dir, "both") && !desc->ports[1].id, "one uart port");
    /* the CLI lines per firmware family: RotorFlight's and iNav's, none unconditional */
    const char *const *rf = NULL, *const *inav = NULL;
    for (int i = 0; desc && desc->config_fw && desc->config_fw[i].firmware; i++) {
        if (!strcmp(desc->config_fw[i].firmware, "rotorflight")) rf = desc->config_fw[i].lines;
        if (!strcmp(desc->config_fw[i].firmware, "inav")) inav = desc->config_fw[i].lines;
    }
    CHECK(desc && !desc->config && rf && inav && strstr(rf[0], "{serial}"), "config lines per firmware");
    bool auto_dev = false, msp_osd_fn = false, hdzero = false;
    for (int i = 0; rf && rf[i]; i++) {
        auto_dev |= !strcmp(rf[i], "set osd_displayport_device = AUTO");
    }
    for (int i = 0; inav && inav[i]; i++) {
        msp_osd_fn |= !strcmp(inav[i], "serial {serial} 33554432 115200 115200 0 115200");
        hdzero |= !strcmp(inav[i], "set osd_video_system = HDZERO");
    }
    CHECK(auto_dev, "RotorFlight: osd_displayport_device = AUTO (the firmware's save + reboot to HD 50x18)");
    CHECK(msp_osd_fn && hdzero, "iNav: FUNCTION_MSP_OSD (bit 25) at 115200, the HDZERO canvas");
    CHECK(desc && fb_device_fault(desc, "link_lost"), "link_lost fault");

    if (fails) {
        printf("test_msp_osd: %d failures\n", fails);
        return 1;
    }
    printf("test_msp_osd: OK\n");
    return 0;
}
