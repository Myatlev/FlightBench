/* Fuzz harness for the mode-message decoders (hw/arm/fb_msp_modes.c.inc): every decoder is fed
 * truncations and overruns of a known-good payload, plus pure random garbage, at every length up
 * to a generous cap. No CHECK()s of decoded values here -- this only asks the sanitizers (build
 * with -fsanitize=address,undefined, see the fuzz Makefile target) whether a decoder ever reads
 * or writes out of bounds, or hits signed-overflow / UB, on attacker-controlled MSP bytes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hw/arm/fb_msp_modes.c.inc"

#define OVERRUN   64    /* bytes appended past a good payload's length */
#define GARBAGE   320   /* max length of the pure-random sweep */

static uint64_t rngstate = 0x9e3779b97f4a7c15ULL;

static uint64_t xrand(void)
{
    rngstate ^= rngstate << 13;
    rngstate ^= rngstate >> 7;
    rngstate ^= rngstate << 17;
    return rngstate;
}

static void fill_random(uint8_t *buf, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        buf[i] = (uint8_t)xrand();
    }
}

static long iters;

/* ---- one call per decoder, fresh state each time -------------------------------------------- */

static void try_names(const uint8_t *p, unsigned n)
{
    char names[FBM_MAX_BOXES][FBM_NAME];
    int count = 0;
    fbm_names(p, n, names, &count, FBM_MAX_BOXES);
    iters++;
}

static void try_ids(const uint8_t *p, unsigned n)
{
    uint8_t ids[FBM_MAX_BOXES];
    int count = 0;
    fbm_ids(p, n, ids, &count, FBM_MAX_BOXES);
    iters++;
}

static void try_rx_map(const uint8_t *p, unsigned n)
{
    uint8_t map[FBM_MAX_MAP];
    fbm_rx_map(p, n, map, FBM_MAX_MAP);
    iters++;
}

static void try_ranges(bool inav, const uint8_t *p, unsigned n)
{
    fbm_range_t range[FBM_MAX_SLOTS];
    fbm_ranges(inav, p, n, range, FBM_MAX_SLOTS);
    iters++;
}

/* ranges/nranges: the raw MSP_MODE_RANGES bytes MSP_MODE_RANGES_EXTRA is decoded against
 * (fbm_ranges_extra reads the aux channel of a linked-only slot straight out of it) */
static void try_ranges_extra(const uint8_t *ranges, unsigned nranges, const uint8_t *p, unsigned n)
{
    fbm_range_t range[FBM_MAX_SLOTS];
    memset(range, 0, sizeof(range));
    int k = fbm_ranges(false, ranges, nranges, range, FBM_MAX_SLOTS);
    fbm_ranges_extra(ranges, nranges, p, n, range, k, FBM_MAX_SLOTS);
    iters++;
}

static void try_active_ids(const fbm_modes_t *m, uint64_t bits, int nbits)
{
    uint8_t out[FBM_MAX_BOXES];
    fbm_active_ids(m, bits, nbits, out);
    iters++;
}

/* fbm_rc_channel/fbm_rx_channel take plain ints, not wire bytes -- but rc/aux both ultimately
 * come from a uint8_t on the wire (fbm_range_t.aux) through the one call site in
 * nexus_xr_ctl.c.inc (modes_json), so rc is never negative there in practice. Still probe
 * out-of-contract values directly: a negative rc used to index fbm_modes_t.map[] with no lower
 * bound (fixed below; this call is the regression test for that). */
static void try_channels(const fbm_modes_t *m, int aux, int rc)
{
    int a = fbm_rc_channel(true, aux);
    int b = fbm_rc_channel(false, aux);
    int c = fbm_rx_channel(m, rc);
    (void)a; (void)b; (void)c;
    iters++;
}

/* ---- length sweeps: truncated / overrun of a good payload, plus pure garbage ----------------- */

typedef void (*decode1_fn)(const uint8_t *, unsigned);

static void sweep_one(const char *what, const uint8_t *good, unsigned goodlen, decode1_fn f)
{
    uint8_t buf[4096];
    for (unsigned len = 0; len < goodlen; len++) {
        memcpy(buf, good, len);
        f(buf, len);
    }
    for (unsigned len = goodlen + 1; len <= goodlen + OVERRUN; len++) {
        memcpy(buf, good, goodlen);
        fill_random(buf + goodlen, len - goodlen);
        f(buf, len);
    }
    for (unsigned len = 0; len <= GARBAGE; len++) {
        fill_random(buf, len);
        f(buf, len);
    }
    printf("fuzz: %s: OK (%ld calls so far)\n", what, iters);
}

/* MSP_MODE_RANGES_EXTRA also varies the raw MODE_RANGES half; sweep both axes */
static void sweep_extra(const uint8_t *ranges, unsigned rangeslen, const uint8_t *extra,
                        unsigned extralen)
{
    uint8_t rbuf[4096], ebuf[4096];
    /* fixed valid ranges, extra swept */
    for (unsigned len = 0; len < extralen; len++) {
        memcpy(ebuf, extra, len);
        try_ranges_extra(ranges, rangeslen, ebuf, len);
    }
    for (unsigned len = extralen + 1; len <= extralen + OVERRUN; len++) {
        memcpy(ebuf, extra, extralen);
        fill_random(ebuf + extralen, len - extralen);
        try_ranges_extra(ranges, rangeslen, ebuf, len);
    }
    for (unsigned len = 0; len <= GARBAGE; len++) {
        fill_random(ebuf, len);
        try_ranges_extra(ranges, rangeslen, ebuf, len);
    }
    /* fixed valid extra, ranges swept (garbage ranges + garbage extra together too) */
    for (unsigned len = 0; len < rangeslen; len++) {
        memcpy(rbuf, ranges, len);
        try_ranges_extra(rbuf, len, extra, extralen);
    }
    for (unsigned len = rangeslen + 1; len <= rangeslen + OVERRUN; len++) {
        memcpy(rbuf, ranges, rangeslen);
        fill_random(rbuf + rangeslen, len - rangeslen);
        try_ranges_extra(rbuf, len, extra, extralen);
    }
    for (unsigned len = 0; len <= GARBAGE; len++) {
        fill_random(rbuf, len);
        unsigned elen = (unsigned)(xrand() % (GARBAGE + 1));
        fill_random(ebuf, elen);
        try_ranges_extra(rbuf, len, ebuf, elen);
    }
    printf("fuzz: mode_ranges_extra (both axes): OK (%ld calls so far)\n", iters);
}

int main(void)
{
    /* known-good payloads, lifted from the captured fixtures (fixtures/msp-*.json) */
    static const uint8_t inav_names[] =
        "ARM;PREARM;MULTI FUNCTION;ANGLE;HORIZON;TURN ASSIST;HEADING HOLD;CAMSTAB;MANUAL;"
        "NAV LAUNCH;SERVO AUTOTRIM;AUTO TUNE;ANGLE HOLD;BEEPER;BEEPER MUTE;OSD OFF;BLACKBOX;"
        "FAILSAFE;CAMERA CONTROL 1;CAMERA CONTROL 2;CAMERA CONTROL 3;USER1;USER2;USER3;USER4;"
        "OSD ALT 1;OSD ALT 2;OSD ALT 3;MSP RC OVERRIDE;MIXER PROFILE 2;MIXER TRANSITION;"
        "GIMBAL LEVEL TILT;GIMBAL LEVEL ROLL;GIMBAL CENTER;";
    static const uint8_t rtfl_names[] =
        "ARM;ANGLE;HORIZON;TRAINER;RESCUE;FAILSAFE;PREARM;PARALYZE;BEEPER;BEEPER MUTE;"
        "OSD DISABLE;TELEMETRY;BLACKBOX;BLACKBOX ERASE;STICK COMMANDS DISABLE;"
        "GOVERNOR FALLBACK;GOVERNOR SUSPEND;GOVERNOR BYPASS;";
    static const uint8_t rtfl_ids[] = {
        0x00, 0x01, 0x02, 0x2f, 0x35, 0x1b, 0x24, 0x2d, 0x0d, 0x34, 0x13, 0x14, 0x1a, 0x1f, 0x33,
        0x37, 0x38, 0x39,
    };
    static const uint8_t inav_ids[] = {
        0x00, 0x33, 0x3d, 0x01, 0x02, 0x23, 0x05, 0x08, 0x0c, 0x24, 0x25, 0x15, 0x40, 0x0d, 0x3c,
        0x13, 0x1a, 0x1b, 0x27, 0x28, 0x29, 0x2f, 0x30, 0x39, 0x3a, 0x2a, 0x2b, 0x2c, 0x32, 0x3e,
        0x3f, 0x41, 0x42, 0x43,
    };
    static const uint8_t rtfl_map[] = { 0, 1, 3, 2, 5, 4, 6, 7 };
    static const uint8_t inav_map[] = { 0, 1, 3, 2 };
    /* RS7: one usable slot (ARM on aux 0, 1700..2100), the rest zeroed (unused) */
    static const uint8_t rtfl_ranges[80] = { 0x00, 0x00, 0x28, 0x78 };
    /* trainer: three usable slots */
    static const uint8_t inav_ranges[160] = {
        0x00, 0x00, 0x20, 0x30, 0x01, 0x01, 0x20, 0x30, 0x24, 0x02, 0x20, 0x30,
    };
    /* RS7's MSP_MODE_RANGES_EXTRA: 20 slots, all OR / not linked */
    static uint8_t rtfl_extra[1 + 3 * 20];
    rtfl_extra[0] = 20;
    for (int i = 0; i < 20; i++) {
        rtfl_extra[1 + 3 * i] = i == 0 ? 0 : 0;    /* box id, whatever fbm_ranges saw at slot i */
        rtfl_extra[1 + 3 * i + 1] = 0;             /* OR */
        rtfl_extra[1 + 3 * i + 2] = 0;             /* not linked */
    }
    /* a synthetic extra with an AND'd slot and a linked-only slot (mirrors test_synthetic in
     * test_msp.c), to exercise the "add a slot for a link" path under truncation too */
    static const uint8_t link_ranges[12] = { 1, 2, 0x28, 0x78, 13, 0, 0, 0, 5, 1, 0x9c, 0x00 };
    static const uint8_t link_extra[10] = { 3, 1, 1, 0, 13, 0, 1, 5, 0, 0 };

    sweep_one("MSP_BOXNAMES (iNav)", inav_names, sizeof(inav_names) - 1, try_names);
    sweep_one("MSP_BOXNAMES (RotorFlight)", rtfl_names, sizeof(rtfl_names) - 1, try_names);
    sweep_one("MSP_BOXIDS (iNav)", inav_ids, sizeof(inav_ids), try_ids);
    sweep_one("MSP_BOXIDS (RotorFlight)", rtfl_ids, sizeof(rtfl_ids), try_ids);
    sweep_one("MSP_RX_MAP (iNav, 4)", inav_map, sizeof(inav_map), try_rx_map);
    sweep_one("MSP_RX_MAP (RotorFlight, 8)", rtfl_map, sizeof(rtfl_map), try_rx_map);
    for (unsigned len = 0; len < sizeof(inav_ranges); len++) {
        try_ranges(true, inav_ranges, len);
    }
    {
        uint8_t buf[4096];
        for (unsigned len = sizeof(inav_ranges) + 1; len <= sizeof(inav_ranges) + OVERRUN; len++) {
            memcpy(buf, inav_ranges, sizeof(inav_ranges));
            fill_random(buf + sizeof(inav_ranges), len - sizeof(inav_ranges));
            try_ranges(true, buf, len);
        }
        for (unsigned len = 0; len <= GARBAGE; len++) {
            fill_random(buf, len);
            try_ranges(true, buf, len);
        }
    }
    printf("fuzz: MSP_MODE_RANGES (iNav): OK (%ld calls so far)\n", iters);
    for (unsigned len = 0; len < sizeof(rtfl_ranges); len++) {
        try_ranges(false, rtfl_ranges, len);
    }
    {
        uint8_t buf[4096];
        for (unsigned len = sizeof(rtfl_ranges) + 1; len <= sizeof(rtfl_ranges) + OVERRUN; len++) {
            memcpy(buf, rtfl_ranges, sizeof(rtfl_ranges));
            fill_random(buf + sizeof(rtfl_ranges), len - sizeof(rtfl_ranges));
            try_ranges(false, buf, len);
        }
        for (unsigned len = 0; len <= GARBAGE; len++) {
            fill_random(buf, len);
            try_ranges(false, buf, len);
        }
    }
    printf("fuzz: MSP_MODE_RANGES (RotorFlight): OK (%ld calls so far)\n", iters);

    sweep_extra(rtfl_ranges, sizeof(rtfl_ranges), rtfl_extra, sizeof(rtfl_extra));
    sweep_extra(link_ranges, sizeof(link_ranges), link_extra, sizeof(link_extra));

    /* fbm_active_ids: garbage bit patterns and box counts against both fixture box lists */
    {
        fbm_modes_t m;
        memset(&m, 0, sizeof(m));
        m.nid = sizeof(rtfl_ids);
        memcpy(m.id, rtfl_ids, sizeof(rtfl_ids));
        for (int nbits = 0; nbits <= 70; nbits++) {
            for (int t = 0; t < 200; t++) {
                try_active_ids(&m, xrand(), nbits);
            }
        }
        m.nid = sizeof(inav_ids);
        memcpy(m.id, inav_ids, sizeof(inav_ids));
        for (int nbits = 0; nbits <= 70; nbits++) {
            for (int t = 0; t < 200; t++) {
                try_active_ids(&m, xrand(), nbits);
            }
        }
        /* garbage nid too (out-of-contract, but fbm_active_ids should still stay in bounds) */
        for (int t = 0; t < 2000; t++) {
            m.nid = (int)(xrand() % 200) - 50;
            try_active_ids(&m, xrand(), (int)(xrand() % 140) - 20);
        }
        printf("fuzz: fbm_active_ids: OK (%ld calls so far)\n", iters);
    }

    /* fbm_rc_channel / fbm_rx_channel: aux/rc within the real uint8_t contract, then well past
     * it (negative, huge) against every nmap 0..FBM_MAX_MAP and garbage map bytes */
    {
        fbm_modes_t m;
        for (int nmap = 0; nmap <= FBM_MAX_MAP; nmap++) {
            memset(&m, 0, sizeof(m));
            m.nmap = nmap;
            fill_random(m.map, sizeof(m.map));
            for (int aux = -20; aux <= 300; aux++) {
                try_channels(&m, aux, aux);
            }
            for (int t = 0; t < 500; t++) {
                int64_t v = (int64_t)(xrand() % (2ull * INT32_MAX)) - INT32_MAX;
                try_channels(&m, (int)v, (int)v);
            }
        }
        printf("fuzz: fbm_rc_channel/fbm_rx_channel: OK (%ld calls so far)\n", iters);
    }

    printf("test_msp_fuzz: %ld decoder calls, no crash\n", iters);
    return 0;
}
