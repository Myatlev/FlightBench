/* Host-side test of the u-blox model against a scripted master that does what iNav 9.1.0's
 * io/gps_ublox.c does: autobaud with $PUBX,41 at every baud, MON-VER / MON-GNSS polls decoded the
 * way gpsParseFrameUBLOX() does, then the gpsConfigure() message sequence of the M10 (CFG-VALSET)
 * or the M8 (CFG-NAV5/MSG/RATE/SBAS/GNSS) path, expecting the ACKs the driver waits for; then the
 * periodic messages, decoded as the driver decodes them. */
#include "fb/ublox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define MS 1000000ull

/* ---- the FC side: iNav's receive state machine (gpsNewFrameUBLOX) ------------------------- */

typedef struct { uint8_t cls, id; uint16_t len; uint8_t p[600]; uint64_t t_ms; } frame_t;
#define NF 16384
static frame_t frames[NF];
static int nframes;
static int bad_ck;
static uint64_t now_ms;

static struct { int step; uint8_t cls, id, a, b; uint16_t len, n; uint8_t p[600]; } rx;

static void fc_byte(uint8_t c)
{
    switch (rx.step) {
    case 0: if (c == 0xB5) rx.step = 1; break;
    case 1: rx.step = c == 0x62 ? 2 : 0; break;
    case 2: rx.cls = c; rx.a = rx.b = c; rx.step = 3; break;
    case 3: rx.id = c; rx.b += (rx.a += c); rx.step = 4; break;
    case 4: rx.len = c; rx.b += (rx.a += c); rx.step = 5; break;
    case 5: rx.len |= (uint16_t)(c << 8); rx.b += (rx.a += c); rx.n = 0; rx.step = rx.len > 600 ? 0 : rx.len ? 6 : 7; break;
    case 6: rx.b += (rx.a += c); rx.p[rx.n++] = c; if (rx.n == rx.len) rx.step = 7; break;
    case 7: if (c != rx.a) { bad_ck++; rx.step = 0; } else rx.step = 8; break;
    case 8:
        rx.step = 0;
        if (c != rx.b) { bad_ck++; break; }
        if (nframes < NF) {
            frame_t *f = &frames[nframes++];
            f->cls = rx.cls; f->id = rx.id; f->len = rx.len; f->t_ms = now_ms;
            memcpy(f->p, rx.p, rx.len);
        }
        break;
    }
}

/* run the receiver for ms milliseconds in 1 ms steps, the FC reading everything */
static void pump(fb_ublox_t *d, int ms)
{
    for (int i = 0; i < ms; i++) {
        fb_ublox_tick(d, MS);
        now_ms++;
        uint8_t b[4096];
        int n = fb_ublox_tx_pop(d, b, sizeof(b));
        for (int k = 0; k < n; k++) fc_byte(b[k]);
    }
}

static void fc_send(fb_ublox_t *d, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) fb_ublox_rx_byte(d, b[i]);
}

/* sendConfigMessageUBLOX(): header + payload + checksum over class..payload */
static void fc_ubx(fb_ublox_t *d, uint8_t cls, uint8_t id, const uint8_t *p, int len)
{
    uint8_t f[128];
    f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = (uint8_t)len; f[5] = (uint8_t)(len >> 8);
    if (len) memcpy(f + 6, p, (size_t)len);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < len + 6; i++) { a += f[i]; b += a; }     /* ublox_update_checksum() */
    f[len + 6] = a; f[len + 7] = b;
    fc_send(d, f, len + 8);
}

/* ubloxCfgFillBytes()/ubloxCfgFillU2(): VALSET version 1, layers 1 (RAM), transaction 0 */
static void fc_valset(fb_ublox_t *d, const uint32_t *keys, const uint32_t *vals, int n, int vsize)
{
    uint8_t p[4 + 32 * 6] = { 1, 1, 0, 0 };
    int len = 4;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) p[len++] = (uint8_t)(keys[i] >> (8 * k));
        for (int k = 0; k < vsize; k++) p[len++] = (uint8_t)(vals[i] >> (8 * k));
    }
    fc_ubx(d, 0x06, 0x8A, p, len);
}

/* ptWait(_ack_state ...): ACK-ACK / ACK-NAK for message id within timeout; 1 ack, 2 nak, 0 none */
static int wait_ack(fb_ublox_t *d, uint8_t id, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t++) {
        int from = nframes;
        pump(d, 1);
        for (int i = from; i < nframes; i++) {
            if (frames[i].cls == 0x05 && frames[i].len == 2 && frames[i].p[1] == id) return frames[i].id == 0x01 ? 1 : 2;
        }
    }
    return 0;
}

static const frame_t *wait_frame(fb_ublox_t *d, uint8_t cls, uint8_t id, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t++) {
        int from = nframes;
        pump(d, 1);
        for (int i = from; i < nframes; i++) {
            if (frames[i].cls == cls && frames[i].id == id) return &frames[i];
        }
    }
    return NULL;
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int32_t i32(const uint8_t *p) { return (int32_t)u32(p); }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const char *find30(const uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= 30; i++) if (!memcmp(p + i, s, n)) return (const char *)p + i;
    return NULL;
}

/* ---- the handshake ---------------------------------------------------------------------- */

typedef struct { int hw; int major, minor; uint8_t supported; int max_gnss; } caps_t;

static const uint32_t bauds[8] = { 115200, 57600, 38400, 19200, 9600, 230400, 460800, 921600 };

/* gpsProtocolStateThread() autobaud: the $PUBX for the port's baud (115200) at every rate up to
 * gps_auto_baud_max_supported (230400), 100 ms apart; the port ends at 115200 */
static void autobaud(fb_ublox_t *d)
{
    const char *pubx = "$PUBX,41,1,0003,0001,115200,0*1E\r\n";   /* baudInitDataNMEA[GPS_BAUDRATE_115200] */
    for (int i = 0; i < 8; i++) {
        if (bauds[i] > 230400) continue;
        fb_ublox_set_fc_baud(d, bauds[i]);
        fc_send(d, (const uint8_t *)pubx, (int)strlen(pubx));
        pump(d, 100);                                            /* GPS_BAUD_CHANGE_DELAY */
    }
    fb_ublox_set_fc_baud(d, 115200);
}

static caps_t detect(fb_ublox_t *d)
{
    caps_t c = { 0 };
    fc_ubx(d, 0x0A, 0x04, NULL, 0);                              /* pollVersion() */
    const frame_t *f = wait_frame(d, 0x0A, 0x04, 500);
    CHECK(f != NULL, "MON-VER answered");
    if (f) {
        if (!strncmp((const char *)f->p + 30, "000A0000", 10)) c.hw = 10;
        if (!strncmp((const char *)f->p + 30, "00080000", 10)) c.hw = 8;
        for (int j = 40; j < f->len; j += 30) {
            const char *s = find30(f->p + j, "PROTVER");
            if (s) {
                float ver = strtof(s + 8, NULL);                 /* fastA2F */
                c.major = (int)ver;
                c.minor = (int)((ver - c.major) * 100.0f);
                break;
            }
        }
        for (int j = 40; j < f->len; j += 30) {
            if (find30(f->p + j, "GAL")) c.supported |= 8;
            if (find30(f->p + j, "BDS")) c.supported |= 4;
            if (find30(f->p + j, "GLO")) c.supported |= 2;
            if (c.supported) break;
        }
    }
    fc_ubx(d, 0x0A, 0x28, NULL, 0);                              /* pollGnssCapabilities() */
    f = wait_frame(d, 0x0A, 0x28, 500);
    CHECK(f && f->len == 8 && f->p[0] == 0, "MON-GNSS v0");
    if (f) { c.supported = f->p[1]; c.max_gnss = f->p[4]; }
    return c;
}

static bool gt(const caps_t *c, int mj, int mn) { return c->major > mj || (c->major == mj && c->minor > mn); }

/* gpsConfigure(), M9+/VALSET path (iNav defaults: AIR_2G, SBAS NONE, GAL on, BDS on, GLO off, 10 Hz) */
static void configure_m10(fb_ublox_t *d)
{
    uint32_t k1[] = { 0x20110021, 0x20110011 }, v1[] = { 7, 3 };
    fc_valset(d, k1, v1, 2, 1);
    CHECK(wait_ack(d, 0x8A, 500) == 1, "VALSET dynmodel ACK");
    uint32_t k2[] = { 0x209100bb, 0x209100ca, 0x209100c0, 0x209100ac, 0x209100b1 }, v2[5] = { 0 };
    fc_valset(d, k2, v2, 5, 1);
    CHECK(wait_ack(d, 0x8A, 500) == 1, "VALSET NMEA off ACK");
    uint32_t k3[] = { 0x2091002a, 0x2091001b, 0x20910043, 0x2091005c, 0x20910007, 0x20910346, 0x20910016 };
    uint32_t v3[] = { 0, 0, 0, 0, 1, 1, 0 };
    fc_valset(d, k3, v3, 7, 1);
    CHECK(wait_ack(d, 0x8A, 500) == 1, "VALSET MSGOUT ACK");
    uint32_t k4[] = { 0x30210001, 0x30210002 }, v4[] = { 100, 1 };
    fc_valset(d, k4, v4, 2, 2);                                  /* configureRATE(hz2rate(10)) */
    CHECK(wait_ack(d, 0x8A, 500) == 1, "VALSET RATE ACK");
    uint8_t sbas[8] = { 2, 3, 3, 0, 0, 0, 0, 0 };                /* configureSBAS(), SBAS_NONE */
    fc_ubx(d, 0x06, 0x16, sbas, 8);
    CHECK(wait_ack(d, 0x16, 500) == 2, "CFG-SBAS on an M10: NAK");
    uint32_t k5[] = { 0x10310020, 0x10310005, 0x10310021, 0x10310007, 0x10310022, 0x1031000d,
                      0x1031000f, 0x10310024, 0x10310012, 0x10310014, 0x10310025, 0x10310018 };
    uint32_t v5[] = { 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0 };
    fc_valset(d, k5, v5, 12, 1);                                 /* configureGNSS10() */
    CHECK(wait_ack(d, 0x8A, 500) == 1, "VALSET GNSS ACK");
}

/* gpsConfigure(), M8 path (protocol 15..23.01) */
static void configure_m8(fb_ublox_t *d, const caps_t *c)
{
    static const uint8_t nav5[36] = { 0xFF, 0xFF, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x05,
        0x00, 0xFA, 0x00, 0xFA, 0x00, 0x64, 0x00, 0x2C, 0x01, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00 };
    uint8_t p[36];
    memcpy(p, nav5, 36);
    p[2] = 7; p[3] = 3;                                          /* AIR_2G, auto */
    fc_ubx(d, 0x06, 0x24, p, 36);
    CHECK(wait_ack(d, 0x24, 500) == 1, "CFG-NAV5 ACK");
    for (uint8_t id = 0; id <= 5; id++) {                        /* GGA GLL GSA GSV RMC VTG off */
        uint8_t m[3] = { 0xF0, id, 0 };
        fc_ubx(d, 0x06, 0x01, m, 3);
        CHECK(wait_ack(d, 0x01, 500) == 1, "CFG-MSG NMEA %d ACK", id);
    }
    static const uint8_t ids[8][2] = { { 0x02, 0 }, { 0x03, 0 }, { 0x06, 0 }, { 0x12, 0 }, { 0x21, 0 }, { 0x30, 0 }, { 0x07, 1 }, { 0x35, 1 } };
    for (int i = 0; i < 8; i++) {
        uint8_t m[3] = { 0x01, ids[i][0], ids[i][1] };
        fc_ubx(d, 0x06, 0x01, m, 3);
        CHECK(wait_ack(d, 0x01, 500) == 1, "CFG-MSG NAV 0x%02x ACK", ids[i][0]);
    }
    uint8_t rate[6] = { 100, 0, 1, 0, 1, 0 };
    fc_ubx(d, 0x06, 0x08, rate, 6);
    CHECK(wait_ack(d, 0x08, 500) == 1, "CFG-RATE ACK");
    uint8_t sbas[8] = { 2, 3, 3, 0, 0, 0, 0, 0 };
    fc_ubx(d, 0x06, 0x16, sbas, 8);
    CHECK(wait_ack(d, 0x16, 500) == 1, "CFG-SBAS ACK");
    /* configureGNSS(): SBAS always, then GAL/BDS/GLO blocks when supported */
    uint8_t g[4 + 8 * 4] = { 0, 0, 0xFF, 0 };
    int nb = 0;
    uint8_t blocks[4][8] = { { 1, 0, 3, 0, 0, 0, 1, 0 } };
    if (c->supported & 8) { uint8_t b[8] = { 2, 4, 8, 0, 1, 0, 1, 0 }; memcpy(blocks[++nb], b, 8); }
    if (c->supported & 4) { uint8_t b[8] = { 3, 4, 8, 0, 1, 0, 1, 0 }; memcpy(blocks[++nb], b, 8); }
    if (c->supported & 2) { uint8_t b[8] = { 6, 0, 8, 0, 0, 0, 1, 0 }; memcpy(blocks[++nb], b, 8); }
    nb++;
    g[3] = (uint8_t)nb;
    for (int i = 0; i < nb; i++) memcpy(g + 4 + 8 * i, blocks[i], 8);
    fc_ubx(d, 0x06, 0x3E, g, 4 + 8 * nb);
    CHECK(wait_ack(d, 0x3E, 500) == 1, "CFG-GNSS ACK");
}

/* NAV-PVT as gpsParseFrameUBLOX() takes it */
typedef struct { int fix; int32_t lat, lon, alt_cm; int16_t vel[3]; uint16_t gspeed, course; int sats; uint16_t hdop; bool tvalid; int year, mon, day, h, m, s; uint32_t itow; } pvt_t;
static pvt_t decode_pvt(const frame_t *f)
{
    const uint8_t *p = f->p;
    pvt_t v;
    v.itow = u32(p);
    v.fix = (p[21] & 1) && p[20] == 3 ? 3 : (p[21] & 1) && p[20] == 2 ? 2 : 0;
    v.lon = i32(p + 24); v.lat = i32(p + 28); v.alt_cm = i32(p + 36) / 10;
    for (int i = 0; i < 3; i++) v.vel[i] = (int16_t)(i32(p + 48 + 4 * i) / 10);
    v.gspeed = (uint16_t)(i32(p + 60) / 10);
    v.course = (uint16_t)(i32(p + 64) / 10000);
    v.sats = p[23];
    v.hdop = u16(p + 76);
    v.tvalid = (p[11] & 1) && (p[11] & 2);
    v.year = u16(p + 4); v.mon = p[6]; v.day = p[7]; v.h = p[8]; v.m = p[9]; v.s = p[10];
    return v;
}

static const frame_t *last(uint8_t cls, uint8_t id)
{
    for (int i = nframes - 1; i >= 0; i--) if (frames[i].cls == cls && frames[i].id == id) return &frames[i];
    return NULL;
}
static int count(uint8_t cls, uint8_t id, int from)
{
    int n = 0;
    for (int i = from; i < nframes; i++) n += frames[i].cls == cls && frames[i].id == id;
    return n;
}

static void clear(void) { nframes = 0; }

/* power on, boot, handshake: returns caps */
static caps_t bring_up(fb_ublox_t *d)
{
    now_ms = 0;
    clear();
    pump(d, 3500);                                               /* GPS_BOOT_DELAY + GPS_INIT_DELAY */
    autobaud(d);
    CHECK(fb_ublox_baud(d) == 115200, "autobaud: receiver at %u", fb_ublox_baud(d));
    return detect(d);
}

int main(void)
{
    /* framing: the MON-VER poll is B5 62 0A 04 00 00 0E 34 */
    uint8_t fr[16];
    CHECK(fb_ubx_frame(fr, 0x0A, 0x04, NULL, 0) == 8 && fr[6] == 0x0E && fr[7] == 0x34, "MON-VER poll checksum %02x %02x", fr[6], fr[7]);

    fb_ublox_config_t cfg;
    fb_ublox_default_config(&cfg);
    cfg.noise_m = 0;
    fb_ublox_t *d = fb_ublox_new(&cfg);
    const double home[3] = { 40.1792, 44.4991, 1000 };
    fb_ublox_set_home(d, home[0], home[1], home[2]);

    /* ---- M10 path ---- */
    caps_t c = bring_up(d);
    fb_ublox_stats_t st;
    fb_ublox_stats(d, &st);
    CHECK(st.rx_pubx == 1 && st.rx_lost > 0, "one PUBX heard (at 9600), the others lost: %u / %u", st.rx_pubx, st.rx_lost);
    CHECK(c.hw == 10 && c.major == 34 && gt(&c, 23, 1), "M10: hw %d protver %d.%d", c.hw, c.major, c.minor);
    CHECK(c.supported == 0x0F && c.max_gnss == 4, "M10 caps 0x%x %d", c.supported, c.max_gnss);
    configure_m10(d);
    CHECK(fb_ublox_nav_period_ms(d) == 100 && fb_ublox_dyn_model(d) == 7, "rate %u dyn %d", fb_ublox_nav_period_ms(d), fb_ublox_dyn_model(d));
    CHECK(fb_ublox_msg_rate(d, 0x01, 0x07) == 1 && fb_ublox_msg_rate(d, 0x01, 0x43) == 1 && fb_ublox_msg_rate(d, 0x01, 0x35) == 0, "M10 msgout");
    CHECK(fb_ublox_gnss_enabled(d) == 0x0D, "GPS+GAL+BDS, gnss 0x%x", fb_ublox_gnss_enabled(d));

    /* fix acquisition: no fix before 20 s */
    double pos[3] = { 0, 0, 0 }, vel[3] = { 0, 0, 0 };
    fb_ublox_set_motion(d, pos, vel);
    int from = nframes;
    pump(d, 1000);
    CHECK(count(0x01, 0x07, from) == 10 && count(0x01, 0x43, from) == 10 && count(0x01, 0x35, from) == 0,
          "10 Hz: PVT %d SIG %d SAT %d", count(0x01, 0x07, from), count(0x01, 0x43, from), count(0x01, 0x35, from));
    pvt_t v = decode_pvt(last(0x01, 0x07));
    CHECK(v.fix == 0 && !v.tvalid && v.sats <= 3 && v.hdop == 9999, "before the fix: fix %d sats %d", v.fix, v.sats);
    while (now_ms < 20000) pump(d, 100);
    pump(d, 100);
    v = decode_pvt(last(0x01, 0x07));
    CHECK(v.fix == 3 && v.sats == 16 && v.hdop == 120 && v.tvalid, "fix at 20 s: fix %d sats %d hdop %u", v.fix, v.sats, v.hdop);
    const frame_t *sig = last(0x01, 0x43);
    CHECK(sig && sig->p[5] == 16 && sig->len == 8 + 16 * 16, "NAV-SIG with 16 signals");

    /* UTC and GPS time: 2026-09-29 (a Tuesday) + virtual time */
    CHECK(v.year == 2026 && v.mon == 9 && v.day == 29 && v.h == 0 && v.m == 0 && v.s == (int)(now_ms / 1000),
          "UTC %04d-%02d-%02d %02d:%02d:%02d", v.year, v.mon, v.day, v.h, v.m, v.s);
    CHECK(v.itow == 172818000u + (uint32_t)(now_ms / 100 * 100), "iTOW %u", v.itow);

    /* NED -> lat/lon -> NED within 1 cm, velocity, course */
    pos[0] = 1234.567; pos[1] = -987.654; pos[2] = -150.25;
    vel[0] = 12.0; vel[1] = -9.0; vel[2] = -1.5;
    fb_ublox_set_motion(d, pos, vel);
    pump(d, 100);
    v = decode_pvt(last(0x01, 0x07));
    double ned[3];
    const frame_t *pf = last(0x01, 0x07);
    fb_ubx_llh_to_ned(home[0], home[1], home[2], v.lat * 1e-7, v.lon * 1e-7, i32(pf->p + 36) * 1e-3, ned);
    CHECK(fabs(ned[0] - pos[0]) < 0.01 && fabs(ned[1] - pos[1]) < 0.01 && fabs(ned[2] - pos[2]) < 0.001,
          "round trip %.4f %.4f %.4f", ned[0] - pos[0], ned[1] - pos[1], ned[2] - pos[2]);
    CHECK(v.alt_cm == 115025, "alt %d cm", v.alt_cm);
    CHECK(v.vel[0] == 1200 && v.vel[1] == -900 && v.vel[2] == -150 && v.gspeed == 1500, "vel %d %d %d gs %u", v.vel[0], v.vel[1], v.vel[2], v.gspeed);
    CHECK(v.course == 3231, "course %u (323.13 deg)", v.course);
    /* WGS-84, not a sphere: 1234.567 m north at 40.18 deg, 1000 m up (M + h = 6363013 m) is 0.0111167 deg */
    CHECK(fabs(v.lat * 1e-7 - 40.1792 - 0.0111167) < 1e-7, "lat %.7f", v.lat * 1e-7);

    /* rate: RATE_MEAS 200 ms -> 5 Hz on the new grid */
    uint32_t kr[] = { 0x30210001, 0x30210002 }, vr[] = { 200, 1 };
    fc_valset(d, kr, vr, 2, 2);
    CHECK(wait_ack(d, 0x8A, 100) == 1, "rate change ACK");
    from = nframes;
    pump(d, 1000);
    CHECK(count(0x01, 0x07, from) == 5, "5 Hz: %d", count(0x01, 0x07, from));
    uint32_t kbad[] = { 0x30210001 }, vbad[] = { 10 };
    fc_valset(d, kbad, vbad, 1, 2);
    CHECK(wait_ack(d, 0x8A, 100) == 2, "RATE_MEAS 10 ms: NAK");
    uint32_t kunk[] = { 0x20990001 }, vunk[] = { 1 };
    fc_valset(d, kunk, vunk, 1, 1);
    CHECK(wait_ack(d, 0x8A, 100) == 2 && fb_ublox_nav_period_ms(d) == 200, "unknown key: NAK, nothing applied");
    fc_valset(d, kr, (uint32_t[]){ 100, 1 }, 2, 2);
    wait_ack(d, 0x8A, 100);

    /* bad checksum: ignored, no answer */
    uint8_t poll[8];
    fb_ubx_frame(poll, 0x0A, 0x04, NULL, 0);
    poll[7] ^= 1;
    fb_ublox_stats(d, &st);
    uint32_t bad0 = st.rx_bad_checksum;
    fc_send(d, poll, 8);
    CHECK(wait_frame(d, 0x0A, 0x04, 200) == NULL, "no MON-VER for a bad checksum");
    fb_ublox_stats(d, &st);
    CHECK(st.rx_bad_checksum == bad0 + 1, "bad checksum counted");

    /* periodic MON-GNSS re-poll (every 5 s by iNav) still answered while running */
    fc_ubx(d, 0x0A, 0x28, NULL, 0);
    CHECK(wait_frame(d, 0x0A, 0x28, 100) != NULL, "MON-GNSS while running");

    /* ---- faults ---- */
    pos[0] = pos[1] = pos[2] = 0;
    vel[0] = vel[1] = vel[2] = 0;
    fb_ublox_set_motion(d, pos, vel);
    CHECK(fb_ublox_set_fault(d, "no_fix", true, NULL), "no_fix on");
    pump(d, 200);
    v = decode_pvt(last(0x01, 0x07));
    CHECK(v.fix == 0 && v.sats == 2, "no_fix: fix %d sats %d", v.fix, v.sats);
    fb_ublox_set_fault(d, "no_fix", false, NULL);
    pump(d, 200);
    CHECK(decode_pvt(last(0x01, 0x07)).fix == 3, "fix back");

    double sp[3] = { 100, -50, 20 };
    fb_ublox_set_fault(d, "spoof", true, sp);
    pump(d, 200);
    pf = last(0x01, 0x07);
    v = decode_pvt(pf);
    fb_ubx_llh_to_ned(home[0], home[1], home[2], v.lat * 1e-7, v.lon * 1e-7, i32(pf->p + 36) * 1e-3, ned);
    CHECK(fabs(ned[0] - 100) < 0.01 && fabs(ned[1] + 50) < 0.01 && fabs(ned[2] + 20) < 0.001, "spoof %.2f %.2f %.2f", ned[0], ned[1], ned[2]);
    fb_ublox_set_fault(d, "spoof", false, NULL);

    double dr = 0.5;
    fb_ublox_set_fault(d, "drift", true, &dr);
    pump(d, 100000);
    pf = last(0x01, 0x07);
    v = decode_pvt(pf);
    fb_ubx_llh_to_ned(home[0], home[1], home[2], v.lat * 1e-7, v.lon * 1e-7, i32(pf->p + 36) * 1e-3, ned);
    double dist = hypot(ned[0], ned[1]);
    CHECK(dist > 0.2 && dist < 30, "drift over 100 s: %.2f m", dist);
    fb_ublox_set_fault(d, "drift", false, NULL);
    pump(d, 100);
    v = decode_pvt(last(0x01, 0x07));
    CHECK(abs(v.lat - 401792000) <= 1, "drift off: back");

    fb_ublox_set_fault(d, "silence", true, NULL);
    from = nframes;
    fc_ubx(d, 0x0A, 0x04, NULL, 0);
    pump(d, 2000);
    CHECK(nframes == from, "silence: nothing for 2 s (iNav times out after 1 s)");
    fb_ublox_set_fault(d, "silence", false, NULL);
    pump(d, 200);
    CHECK(nframes > from, "talking again");

    fb_ublox_set_fault(d, "glitch", true, NULL);
    from = nframes;
    pump(d, 300);
    int jumps = 0, normal = 0;
    for (int i = from; i < nframes; i++) {
        if (frames[i].cls != 1 || frames[i].id != 7) continue;
        int32_t lat = i32(frames[i].p + 28);
        if (abs(lat - 401792000) > 8000) jumps++;
        else normal++;
    }
    CHECK(jumps == 1 && normal == 2 && !fb_ublox_fault_active(d, "glitch"), "glitch: %d jump(s), %d normal", jumps, normal);
    double zero = 0;
    int bad1 = bad_ck;
    fb_ublox_set_fault(d, "glitch", true, &zero);
    from = nframes;
    pump(d, 300);
    CHECK(bad_ck == bad1 + 1 && count(0x01, 0x07, from) == 2, "glitch 0: one broken frame (%d), %d good", bad_ck - bad1, count(0x01, 0x07, from));
    CHECK(!fb_ublox_set_fault(d, "nope", true, NULL), "unknown fault");

    /* power cycle: factory baud, no UBX output; faults survive */
    fb_ublox_set_fault(d, "no_fix", true, NULL);
    fb_ublox_reset(d);
    CHECK(fb_ublox_baud(d) == 9600 && fb_ublox_msg_rate(d, 1, 7) == 0 && fb_ublox_fault_active(d, "no_fix"), "reset");
    fb_ublox_set_fault(d, "no_fix", false, NULL);
    /* the FC at 115200 hears nothing from a 9600 receiver and vice versa */
    fb_ublox_stats(d, &st);
    uint32_t lost0 = st.rx_lost;
    fc_ubx(d, 0x0A, 0x04, NULL, 0);
    CHECK(wait_frame(d, 0x0A, 0x04, 200) == NULL, "baud mismatch: no answer");
    fb_ublox_stats(d, &st);
    CHECK(st.rx_lost == lost0 + 8, "8 bytes lost");
    fb_ublox_free(d);

    /* ---- M8 path ---- */
    cfg.generation = FB_UBLOX_M8;
    d = fb_ublox_new(&cfg);
    fb_ublox_set_home(d, home[0], home[1], home[2]);
    c = bring_up(d);
    CHECK(c.hw == 8 && c.major == 18 && c.minor == 0 && !gt(&c, 23, 1) && c.max_gnss == 3, "M8: hw %d protver %d.%d", c.hw, c.major, c.minor);
    configure_m8(d, &c);
    CHECK(fb_ublox_msg_rate(d, 1, 0x07) == 1 && fb_ublox_msg_rate(d, 1, 0x35) == 1 && fb_ublox_msg_rate(d, 1, 0x06) == 0
          && fb_ublox_msg_rate(d, 1, 0x43) == -1 && fb_ublox_nav_period_ms(d) == 100, "M8 config");
    CHECK(fb_ublox_gnss_enabled(d) == (1 | 8 | 4), "CFG-GNSS: GPS GAL BDS, GLO off: 0x%x", fb_ublox_gnss_enabled(d));
    while (now_ms < 21000) pump(d, 100);
    from = nframes;
    pump(d, 1000);
    CHECK(count(1, 0x07, from) == 10 && count(1, 0x35, from) == 10 && count(1, 0x43, from) == 0, "M8 output PVT %d SAT %d",
          count(1, 0x07, from), count(1, 0x35, from));
    const frame_t *sat = last(1, 0x35);
    CHECK(sat && sat->p[5] == 16 && sat->len == 8 + 12 * 16 && (u32(sat->p + 8 + 8) & 8), "NAV-SAT 16 used");
    /* legacy messages on request: NAV-SOL/POSLLH/VELNED/TIMEUTC produced when enabled */
    uint8_t m[3] = { 1, 0x06, 1 };
    fc_ubx(d, 6, 1, m, 3); wait_ack(d, 1, 100);
    m[1] = 0x12; fc_ubx(d, 6, 1, m, 3); wait_ack(d, 1, 100);
    m[1] = 0x21; m[2] = 10; fc_ubx(d, 6, 1, m, 3); wait_ack(d, 1, 100);
    from = nframes;
    pump(d, 1000);
    CHECK(count(1, 0x06, from) == 10 && count(1, 0x12, from) == 10 && count(1, 0x21, from) == 1, "SOL %d VELNED %d TIMEUTC %d",
          count(1, 0x06, from), count(1, 0x12, from), count(1, 0x21, from));
    const frame_t *sol = last(1, 0x06);
    CHECK(sol && sol->len == 52 && sol->p[10] == 3 && sol->p[47] == 16, "NAV-SOL fix/sats");
    fb_ublox_free(d);

    /* UART capacity: auto-baud off, 9600: a 100-byte NAV-PVT every 100 ms does not fit (96 bytes) */
    cfg.generation = FB_UBLOX_M10;
    d = fb_ublox_new(&cfg);
    fb_ublox_set_fc_baud(d, 9600);
    uint32_t kp[] = { 0x20910007 }, vp[] = { 1 };
    fc_valset(d, kp, vp, 1, 1);
    CHECK(wait_ack(d, 0x8A, 100) == 1, "9600: VALSET ACK");
    from = nframes;
    pump(d, 1000);
    fb_ublox_stats(d, &st);
    CHECK(count(1, 7, from) == 0 && st.tx_lost >= 9, "9600 at 10 Hz: PVT lost (%u)", st.tx_lost);
    /* the FC switches baud with an answer still queued: those bytes never arrive intact */
    fc_ubx(d, 0x0A, 0x04, NULL, 0);
    CHECK(fb_ublox_tx_pending(d) > 0, "MON-VER queued");
    fb_ublox_set_fc_baud(d, 57600);
    uint8_t junk[64];
    uint32_t lost_before = st.tx_lost;
    CHECK(fb_ublox_tx_pop(d, junk, sizeof(junk)) == 0 && fb_ublox_tx_pending(d) == 0, "queue dropped on baud change");
    fb_ublox_stats(d, &st);
    CHECK(st.tx_lost == lost_before + 1, "counted (%u)", st.tx_lost);
    fb_ublox_free(d);

    /* determinism: same seed -> same bytes; another seed -> different noise */
    uint8_t out[3][4096];
    int nout[3];
    for (int r = 0; r < 3; r++) {
        cfg.noise_m = 1.0;
        cfg.seed = r < 2 ? 7 : 8;
        cfg.fix_delay_s = 0;
        cfg.baud = 115200;
        d = fb_ublox_new(&cfg);
        fc_valset(d, kp, vp, 1, 1);
        uint8_t junk[64];
        fb_ublox_tx_pop(d, junk, sizeof(junk));
        nout[r] = 0;
        for (int i = 0; i < 1000; i++) {
            fb_ublox_tick(d, MS);
            nout[r] += fb_ublox_tx_pop(d, out[r] + nout[r], 4096 - nout[r]);
        }
        fb_ublox_free(d);
    }
    CHECK(nout[0] == 1000 && nout[0] == nout[1] && !memcmp(out[0], out[1], (size_t)nout[0]), "reproducible (%d bytes)", nout[0]);
    CHECK(memcmp(out[0], out[2], (size_t)nout[0]) != 0, "another seed differs");

    /* numSV never above what NAV-SAT/SIG can list (M10 default: GPS+GAL+BDS = 24 synthetic sats) */
    {
        fb_ublox_config_t c;
        fb_ublox_default_config(&c);
        c.sats = 32;
        c.fix_delay_s = 0;
        fb_ublox_t *g = fb_ublox_new(&c);
        fb_ublox_tick(g, 250 * 1000000ull);
        CHECK(fb_ublox_solution(g)->num_sv == 24, "numSV %d", fb_ublox_solution(g)->num_sv);
        fb_ublox_free(g);
    }

    /* a home by the antimeridian: longitude stays in [-180, 180), the round trip holds */
    {
        const double east[3] = { 30, 5000, -10 };
        double la, lo, al, back[3];
        fb_ubx_ned_to_llh(0.5, 179.99, 100, east, &la, &lo, &al);
        fb_ubx_llh_to_ned(0.5, 179.99, 100, la, lo, al, back);
        CHECK(lo >= -180 && lo < -179.9, "east over the antimeridian: lon %.7f", lo);
        CHECK(fabs(back[0] - 30) < 0.01 && fabs(back[1] - 5000) < 0.01, "antimeridian round trip %.3f %.3f", back[0], back[1]);
    }

    printf("%s: %d failure(s)\n", fails ? "FAIL" : "OK", fails);
    return fails != 0;
}
