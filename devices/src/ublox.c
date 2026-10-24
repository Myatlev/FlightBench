/* u-blox M10/M8 GNSS receiver (UBX over UART). See include/fb/ublox.h. */
#include "fb/ublox.h"
#include "fb/device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT64_MAX
#define MS 1000000ull
#define TXQ 8192
#define RXMAX 1032          /* largest UBX payload we accept (iNav sends < 100 bytes) */
#define NMEA_MAX 96
#define NOISE_TAU_S 5.0
#define LEAP_S 18
#define GPS_EPOCH_UNIX 315964800ll   /* 1980-01-06 00:00:00 UTC */

#define CLS_NAV 0x01
#define CLS_ACK 0x05
#define CLS_CFG 0x06
#define CLS_MON 0x0A
#define CLS_NMEA 0xF0

/* periodic NAV messages the model can produce (ids in output order) */
enum { M_POSLLH, M_STATUS, M_SOL, M_PVT, M_VELNED, M_TIMEUTC, M_SVINFO, M_SAT, M_SIG, NMSG };
static const uint8_t msg_id[NMSG] = { 0x02, 0x03, 0x06, 0x07, 0x12, 0x21, 0x30, 0x35, 0x43 };

/* MON-GNSS bits */
#define G_GPS 1
#define G_GLO 2
#define G_BDS 4
#define G_GAL 8

struct fb_ublox {
    fb_ublox_config_t cfg;
    double home[3];                 /* lat deg, lon deg, alt MSL m */
    double pos[3], vel[3];          /* NED around home */
    uint64_t t_ns, next_epoch;
    uint32_t baud, fc_baud;
    bool in_ubx, out_ubx;
    uint8_t rate[NMSG];
    uint16_t meas_ms, nav_cycles;
    uint32_t nav_count;
    uint8_t dyn_model, gnss_enabled;
    /* UBX receive */
    int step;
    uint8_t cls, id, ck_a, ck_b;
    uint16_t len, got;
    uint8_t payload[RXMAX];
    /* NMEA receive */
    char line[NMEA_MAX];
    int line_len;                   /* -1 = not in a sentence */
    /* transmit */
    uint8_t txq[TXQ];
    int tx_head, tx_len;
    uint64_t epoch_budget;          /* bytes the line carries in this epoch */
    /* solution */
    fb_ublox_solution_t sol;
    double noise[3];
    uint64_t rng;
    double course_deg;
    bool had_fix;
    /* faults */
    bool no_fix, silence, spoof, drift, glitch;
    double spoof_ned[3], drift_rate, drift_ned[3], glitch_m;
    fb_ublox_stats_t st;
};

/* ---- small helpers ---------------------------------------------------------------------- */

static void put_u16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { put_u16(p, v); put_u16(p + 2, v >> 16); }
static void put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }
static uint32_t get_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get_u32(const uint8_t *p) { return get_u16(p) | get_u16(p + 2) << 16; }

static int32_t i32_sat(double v)
{
    if (v > 2147483647.0) return 2147483647;
    if (v < -2147483648.0) return (int32_t)-2147483647 - 1;
    return (int32_t)lrint(v);
}
static uint32_t u32_sat(double v) { return v <= 0 ? 0 : v >= 4294967295.0 ? 4294967295u : (uint32_t)lrint(v); }

static double gauss(fb_ublox_t *d)
{
    double u, v, s;
    do {
        d->rng ^= d->rng << 13; d->rng ^= d->rng >> 7; d->rng ^= d->rng << 17;
        u = (double)(d->rng >> 11) / 9007199254740992.0 * 2 - 1;
        d->rng ^= d->rng << 13; d->rng ^= d->rng >> 7; d->rng ^= d->rng << 17;
        v = (double)(d->rng >> 11) / 9007199254740992.0 * 2 - 1;
        s = u * u + v * v;
    } while (s >= 1 || s == 0);
    return u * sqrt(-2 * log(s) / s);
}

void fb_ubx_checksum(const uint8_t *data, int len, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0, b = 0;
    for (int i = 0; i < len; i++) {
        a = (uint8_t)(a + data[i]);
        b = (uint8_t)(b + a);
    }
    *ck_a = a;
    *ck_b = b;
}

int fb_ubx_frame(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *payload, int len)
{
    out[0] = 0xB5; out[1] = 0x62; out[2] = cls; out[3] = id;
    put_u16(out + 4, (uint32_t)len);
    if (len) memcpy(out + 6, payload, (size_t)len);
    fb_ubx_checksum(out + 2, len + 4, &out[6 + len], &out[7 + len]);
    return len + 8;
}

/* ---- WGS-84 ----------------------------------------------------------------------------- */

#define WGS_A 6378137.0
#define WGS_F (1.0 / 298.257223563)
#define DEG (M_PI / 180.0)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* meridian and prime-vertical radii of curvature at latitude lat (rad), plus height */
static void radii(double lat, double h, double *rm, double *rn)
{
    const double e2 = WGS_F * (2 - WGS_F);
    double s = sin(lat), w = 1 - e2 * s * s;
    *rm = WGS_A * (1 - e2) / (w * sqrt(w)) + h;
    *rn = WGS_A / sqrt(w) + h;
}

/* longitude into [-180, 180): a home near the antimeridian; values already inside stay bit-exact */
static double wrap180(double x)
{
    if (x >= 180 || x < -180) {
        x = fmod(x + 180, 360);
        if (x < 0) x += 360;
        x -= 180;
    }
    return x;
}

void fb_ubx_ned_to_llh(double lat0, double lon0, double alt0, const double ned[3],
                       double *lat, double *lon, double *alt)
{
    double rm, rn;
    radii(lat0 * DEG, alt0, &rm, &rn);
    *lat = lat0 + ned[0] / rm / DEG;
    *lon = wrap180(lon0 + ned[1] / (rn * cos(lat0 * DEG)) / DEG);
    *alt = alt0 - ned[2];
}

void fb_ubx_llh_to_ned(double lat0, double lon0, double alt0, double lat, double lon, double alt, double ned[3])
{
    double rm, rn;
    radii(lat0 * DEG, alt0, &rm, &rn);
    ned[0] = (lat - lat0) * DEG * rm;
    ned[1] = wrap180(lon - lon0) * DEG * rn * cos(lat0 * DEG);
    ned[2] = alt0 - alt;
}

/* geodetic -> ECEF (NAV-SOL) */
static void llh_to_ecef(double lat, double lon, double h, double e[3])
{
    const double e2 = WGS_F * (2 - WGS_F);
    double s = sin(lat), n = WGS_A / sqrt(1 - e2 * s * s);
    e[0] = (n + h) * cos(lat) * cos(lon);
    e[1] = (n + h) * cos(lat) * sin(lon);
    e[2] = (n * (1 - e2) + h) * s;
}

/* ---- power-on state --------------------------------------------------------------------- */

void fb_ublox_default_config(fb_ublox_config_t *c)
{
    c->generation = FB_UBLOX_M10;
    c->baud = 9600;
    c->rate_hz = 10;
    c->fix_delay_s = 20;
    c->noise_m = 0.3;
    c->sats = 16;
    c->pdop = 1.2;
    c->h_acc_m = 1.0;
    c->seed = 1;
    c->epoch_unix = 1790640000;     /* 2026-09-29 00:00:00 UTC */
}

static bool m10(const fb_ublox_t *d) { return d->cfg.generation == FB_UBLOX_M10; }

static uint32_t period_ms(const fb_ublox_t *d) { return (uint32_t)d->meas_ms * d->nav_cycles; }

static void align_epoch(fb_ublox_t *d)
{
    uint64_t p = (uint64_t)period_ms(d) * MS;
    d->next_epoch = (d->t_ns / p + 1) * p;      /* on the time grid of the new period */
}

static void power_on(fb_ublox_t *d)
{
    d->baud = d->cfg.baud;
    d->in_ubx = d->out_ubx = true;
    memset(d->rate, 0, sizeof(d->rate));        /* factory: NMEA only, which is not modelled */
    uint32_t hz = d->cfg.rate_hz ? d->cfg.rate_hz : 1;
    d->meas_ms = (uint16_t)(1000 / hz);
    d->nav_cycles = 1;
    d->nav_count = 0;
    d->dyn_model = 0;                           /* portable */
    d->gnss_enabled = m10(d) ? (G_GPS | G_GAL | G_BDS) : (G_GPS | G_GLO);
    d->step = 0;
    d->line_len = -1;
    d->tx_head = d->tx_len = 0;
    d->epoch_budget = UINT64_MAX;
    d->t_ns = 0;
    align_epoch(d);
    memset(&d->sol, 0, sizeof(d->sol));
    memset(d->noise, 0, sizeof(d->noise));
    memset(d->drift_ned, 0, sizeof(d->drift_ned));
    d->rng = d->cfg.seed * 0x9E3779B97F4A7C15ULL | 1;
    d->course_deg = 0;
    d->had_fix = false;
}

fb_ublox_t *fb_ublox_new(const fb_ublox_config_t *cfg)
{
    fb_ublox_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (cfg) d->cfg = *cfg;
    else fb_ublox_default_config(&d->cfg);
    if (!d->cfg.baud) d->cfg.baud = 9600;                          /* direct API use: no division by 0 */
    if (d->cfg.rate_hz > 1000) d->cfg.rate_hz = 1000;
    d->home[0] = 40.1792; d->home[1] = 44.4991; d->home[2] = 0;     /* the env's default home */
    power_on(d);
    return d;
}

void fb_ublox_free(fb_ublox_t *d) { free(d); }
void fb_ublox_reset(fb_ublox_t *d) { power_on(d); }

void fb_ublox_set_home(fb_ublox_t *d, double lat, double lon, double alt)
{
    d->home[0] = lat; d->home[1] = lon; d->home[2] = alt;
}

void fb_ublox_set_motion(fb_ublox_t *d, const double pos[3], const double vel[3])
{
    memcpy(d->pos, pos, sizeof(d->pos));
    memcpy(d->vel, vel, sizeof(d->vel));
}

/* ---- UART ------------------------------------------------------------------------------- */

static bool line_ok(const fb_ublox_t *d)
{
    if (!d->fc_baud) return true;
    double r = (double)d->fc_baud / d->baud;
    return r > 0.97 && r < 1.03;
}

void fb_ublox_set_fc_baud(fb_ublox_t *d, uint32_t baud) { d->fc_baud = baud; }
uint32_t fb_ublox_baud(const fb_ublox_t *d) { return d->baud; }
uint64_t fb_ublox_byte_ns(const fb_ublox_t *d) { return 10000000000ull / d->baud; }

/* queue one frame for the FC (whole or not at all) */
static void send(fb_ublox_t *d, uint8_t cls, uint8_t id, const uint8_t *payload, int len, bool corrupt)
{
    if (d->silence) return;
    uint64_t n = (uint64_t)len + 8;
    if (!line_ok(d) || d->tx_len + (int)n > TXQ || n > d->epoch_budget) {
        d->st.tx_lost++;
        return;
    }
    d->epoch_budget -= n;
    uint8_t f[RXMAX + 8];
    fb_ubx_frame(f, cls, id, payload, len);
    if (corrupt) f[len + 7] ^= 0x5A;
    for (uint64_t i = 0; i < n; i++) d->txq[(d->tx_head + d->tx_len + (int)i) % TXQ] = f[i];
    d->tx_len += (int)n;
    d->st.tx_frames++;
}

static void ack(fb_ublox_t *d, bool ok)
{
    uint8_t p[2] = { d->cls, d->id };
    send(d, CLS_ACK, ok ? 0x01 : 0x00, p, 2, false);
    if (ok) d->st.acks++;
    else d->st.naks++;
}

int fb_ublox_tx_pop(fb_ublox_t *d, uint8_t *buf, int max)
{
    if (d->tx_len && !line_ok(d)) {             /* the FC changed baud with bytes still queued */
        d->st.tx_lost++;
        d->tx_head = d->tx_len = 0;
        return 0;
    }
    int n = d->tx_len < max ? d->tx_len : max;
    for (int i = 0; i < n; i++) buf[i] = d->txq[(d->tx_head + i) % TXQ];
    d->tx_head = (d->tx_head + n) % TXQ;
    d->tx_len -= n;
    return n;
}

int fb_ublox_tx_pending(const fb_ublox_t *d) { return d->tx_len; }

/* ---- MON -------------------------------------------------------------------------------- */

static void mon_ver(fb_ublox_t *d)
{
    static const char *const m10_ext[] = { "ROM BASE 0x118B2060", "FWVER=SPG 5.10", "PROTVER=34.10",
                                           "MOD=MAX-M10S", "GPS;GLO;GAL;BDS", "SBAS;QZSS", NULL };
    static const char *const m8_ext[] = { "FWVER=SPG 3.01", "PROTVER=18.00", "GPS;GLO;GAL;BDS",
                                          "SBAS;IMES;QZSS", NULL };
    uint8_t p[40 + 30 * 6];
    memset(p, 0, sizeof(p));
    const char *const *ext = m10(d) ? m10_ext : m8_ext;
    memcpy(p, m10(d) ? "ROM SPG 5.10 (7b202e)" : "ROM CORE 3.01 (107888)", m10(d) ? 21 : 22);
    memcpy(p + 30, m10(d) ? "000A0000" : "00080000", 8);
    int n = 40;
    for (int i = 0; ext[i]; i++, n += 30) memcpy(p + n, ext[i], strlen(ext[i]));
    send(d, CLS_MON, 0x04, p, n, false);
}

static void mon_gnss(fb_ublox_t *d)
{
    uint8_t p[8] = { 0, G_GPS | G_GLO | G_BDS | G_GAL,
                     m10(d) ? (G_GPS | G_GAL | G_BDS) : (G_GPS | G_GLO), d->gnss_enabled,
                     m10(d) ? 4 : 3, 0, 0, 0 };
    send(d, CLS_MON, 0x28, p, 8, false);
}

/* ---- CFG -------------------------------------------------------------------------------- */

static int msg_index(const fb_ublox_t *d, uint8_t cls, uint8_t id)
{
    if (cls != CLS_NAV) return -1;
    for (int i = 0; i < NMSG; i++) {
        if (msg_id[i] != id) continue;
        if ((i == M_SOL || i == M_SVINFO) && m10(d)) return -1;     /* removed after protocol 23 */
        if (i == M_SIG && !m10(d)) return -1;                       /* new in protocol 27 */
        return i;
    }
    return -1;
}

static bool set_rate(fb_ublox_t *d, uint32_t meas, uint32_t nav)
{
    if (meas < 25 || meas > 65535 || nav < 1 || nav > 127) return false;
    d->meas_ms = (uint16_t)meas;
    d->nav_cycles = (uint16_t)nav;
    align_epoch(d);
    return true;
}

static void gnss_bit(fb_ublox_t *d, uint8_t bit, bool on)
{
    if (bit == G_GPS) return;                   /* GPS stays on */
    d->gnss_enabled = on ? (uint8_t)(d->gnss_enabled | bit) : (uint8_t)(d->gnss_enabled & ~bit);
}

/* CFG-VALSET: all keys known or nothing applied (NAK) */
static bool valset(fb_ublox_t *d, const uint8_t *p, int len)
{
    static const uint32_t accepted[] = {
        0x209100bb, 0x209100ca, 0x209100c0, 0x209100ac, 0x209100b1,         /* MSGOUT NMEA UART1 */
        0x20110011,                                                         /* NAVSPG_FIXMODE */
        0x10310020, 0x10310005, 0x10310007, 0x1031000d, 0x1031000f,         /* SIGNAL_* */
        0x10310024, 0x10310012, 0x10310014, 0x10310018,
    };
    if (!m10(d) || len < 4 || p[0] > 1) return false;
    uint32_t meas = d->meas_ms, nav = d->nav_cycles;
    uint8_t rate[NMSG], dyn = d->dyn_model, gnss = d->gnss_enabled;
    memcpy(rate, d->rate, sizeof(rate));
    for (int i = 4; i < len;) {
        if (i + 4 > len) return false;
        uint32_t key = get_u32(p + i);
        int size = (int)((key >> 28) & 7);
        int n = size == 1 || size == 2 ? 1 : size == 3 ? 2 : size == 4 ? 4 : size == 5 ? 8 : 0;
        if (!n || i + 4 + n > len) return false;
        uint32_t v = n == 1 ? p[i + 4] : n == 2 ? get_u16(p + i + 4) : get_u32(p + i + 4);
        i += 4 + n;
        switch (key) {
        case 0x2091002a: rate[M_POSLLH] = (uint8_t)v; break;
        case 0x2091001b: rate[M_STATUS] = (uint8_t)v; break;
        case 0x20910007: rate[M_PVT] = (uint8_t)v; break;
        case 0x20910043: rate[M_VELNED] = (uint8_t)v; break;
        case 0x2091005c: rate[M_TIMEUTC] = (uint8_t)v; break;
        case 0x20910016: rate[M_SAT] = (uint8_t)v; break;
        case 0x20910346: rate[M_SIG] = (uint8_t)v; break;
        case 0x30210001: meas = v; break;
        case 0x30210002: nav = v; break;
        case 0x20110021: dyn = (uint8_t)v; break;
        case 0x10310021: gnss = v ? (uint8_t)(gnss | G_GAL) : (uint8_t)(gnss & ~G_GAL); break;
        case 0x10310022: gnss = v ? (uint8_t)(gnss | G_BDS) : (uint8_t)(gnss & ~G_BDS); break;
        case 0x10310025: gnss = v ? (uint8_t)(gnss | G_GLO) : (uint8_t)(gnss & ~G_GLO); break;
        default: {
            bool known = false;
            for (unsigned k = 0; k < sizeof(accepted) / sizeof(accepted[0]); k++) known |= accepted[k] == key;
            if (!known) return false;
        }
        }
    }
    if ((meas != d->meas_ms || nav != d->nav_cycles) && !set_rate(d, meas, nav)) return false;
    memcpy(d->rate, rate, sizeof(rate));
    d->dyn_model = dyn;
    d->gnss_enabled = gnss;
    return true;
}

static bool legacy_cfg(fb_ublox_t *d, const uint8_t *p, int len)
{
    switch (d->id) {
    case 0x01: {                                /* CFG-MSG: set rate on the current port (3) or per port (8) */
        if (len != 3 && len != 8) return false;
        if (p[0] == CLS_NMEA) return true;      /* NMEA output is not modelled: nothing to switch */
        int i = msg_index(d, p[0], p[1]);
        if (i < 0) return false;
        d->rate[i] = len == 3 ? p[2] : p[3];    /* port 1 = UART1 */
        return true;
    }
    case 0x08:                                  /* CFG-RATE */
        return len == 6 && set_rate(d, get_u16(p), get_u16(p + 2));
    case 0x24:                                  /* CFG-NAV5 */
        if (len != 36) return false;
        if (get_u16(p) & 1) d->dyn_model = p[2];
        return true;
    case 0x16:                                  /* CFG-SBAS */
        return len == 8;
    case 0x3E: {                                /* CFG-GNSS */
        if (len < 4 || len != 4 + 8 * p[3]) return false;
        static const uint8_t bit[8] = { G_GPS, 0, G_GAL, G_BDS, 0, 0, G_GLO, 0 };
        for (int b = 0; b < p[3]; b++) {
            const uint8_t *e = p + 4 + 8 * b;
            if (e[0] < 8 && bit[e[0]]) gnss_bit(d, bit[e[0]], e[4] & 1);
        }
        return true;
    }
    default:
        return false;
    }
}

static void ubx_message(fb_ublox_t *d)
{
    const uint8_t *p = d->payload;
    int len = d->len;
    d->st.rx_frames++;
    if (!d->in_ubx) return;
    if (d->cls == CLS_MON && len == 0) {        /* polls */
        if (d->id == 0x04) mon_ver(d);
        else if (d->id == 0x28) mon_gnss(d);
        return;
    }
    if (d->cls != CLS_CFG) return;
    if (d->id == 0x8A) ack(d, valset(d, p, len));
    else ack(d, !m10(d) && legacy_cfg(d, p, len));   /* the M10 has only the VAL* interface */
}

/* $PUBX,41,portId,inProto,outProto,baudrate,autobauding*CS */
static void nmea_line(fb_ublox_t *d)
{
    char *s = d->line;
    char *star = strchr(s, '*');
    if (s[0] != '$' || !star || star - s < 2) return;
    uint8_t x = 0;
    for (char *c = s + 1; c < star; c++) x ^= (uint8_t)*c;
    if (strtoul(star + 1, NULL, 16) != x) return;
    *star = 0;
    char *f[8] = { 0 };
    int nf = 0;
    for (char *c = s + 1; nf < 8;) {
        f[nf++] = c;
        c = strchr(c, ',');
        if (!c) break;
        *c++ = 0;
    }
    if (nf < 7 || strcmp(f[0], "PUBX") || strcmp(f[1], "41") || strcmp(f[2], "1")) return;
    static const uint32_t bauds[] = { 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    uint32_t baud = (uint32_t)strtoul(f[5], NULL, 10);
    bool valid = false;
    for (unsigned i = 0; i < sizeof(bauds) / sizeof(bauds[0]); i++) valid |= bauds[i] == baud;
    if (!valid) return;
    unsigned long in = strtoul(f[3], NULL, 16), out = strtoul(f[4], NULL, 16);
    d->in_ubx = in & 1;
    d->out_ubx = out & 1;
    d->baud = baud;                             /* from the next byte on */
    d->st.rx_pubx++;
}

void fb_ublox_rx_byte(fb_ublox_t *d, uint8_t b)
{
    if (d->silence) return;
    if (!line_ok(d)) {
        d->st.rx_lost++;
        d->step = 0;
        d->line_len = -1;
        return;
    }
    switch (d->step) {
    case 0:
        if (b == 0xB5) { d->step = 1; return; }
        break;
    case 1:
        d->step = b == 0x62 ? 2 : 0;
        return;
    case 2: d->cls = b; d->ck_a = d->ck_b = b; d->step = 3; return;
    case 3: d->id = b; d->ck_b += (d->ck_a += b); d->step = 4; return;
    case 4: d->len = b; d->ck_b += (d->ck_a += b); d->step = 5; return;
    case 5:
        d->len |= (uint16_t)(b << 8);
        d->ck_b += (d->ck_a += b);
        d->got = 0;
        d->step = d->len > RXMAX ? 0 : d->len ? 6 : 7;
        return;
    case 6:
        d->payload[d->got++] = b;
        d->ck_b += (d->ck_a += b);
        if (d->got == d->len) d->step = 7;
        return;
    case 7:
        d->step = b == d->ck_a ? 8 : 0;
        if (d->step == 0) d->st.rx_bad_checksum++;
        return;
    case 8:
        d->step = 0;
        if (b == d->ck_b) ubx_message(d);
        else d->st.rx_bad_checksum++;
        return;
    }
    /* outside a UBX frame: NMEA input */
    if (b == '$') {
        d->line_len = 0;
    } else if (d->line_len < 0) {
        return;
    }
    if (b == '\r' || b == '\n') {
        d->line[d->line_len] = 0;
        nmea_line(d);
        d->line_len = -1;
        return;
    }
    if (d->line_len < NMEA_MAX - 1) d->line[d->line_len++] = (char)b;
    else d->line_len = -1;
}

/* ---- navigation epochs ------------------------------------------------------------------ */

/* synthetic sky: (gnssId, svId, elevation, azimuth), taken in order from the enabled systems */
static const uint16_t sky[][4] = {
    { 0, 2, 67, 30 }, { 2, 4, 55, 80 }, { 3, 21, 71, 190 }, { 6, 7, 48, 250 },
    { 0, 5, 44, 120 }, { 2, 9, 38, 300 }, { 3, 24, 35, 40 }, { 6, 8, 60, 160 },
    { 0, 12, 31, 210 }, { 2, 11, 27, 150 }, { 3, 26, 52, 100 }, { 6, 17, 22, 60 },
    { 0, 15, 58, 280 }, { 2, 19, 18, 20 }, { 3, 29, 20, 330 }, { 6, 18, 36, 350 },
    { 0, 18, 24, 330 }, { 2, 25, 49, 230 }, { 3, 30, 29, 260 }, { 6, 23, 15, 110 },
    { 0, 20, 15, 70 }, { 2, 31, 12, 180 }, { 3, 32, 14, 140 }, { 6, 24, 11, 200 },
    { 0, 25, 40, 20 }, { 2, 33, 33, 270 }, { 3, 35, 43, 80 }, { 6, 2, 26, 300 },
    { 0, 29, 12, 170 }, { 2, 36, 21, 60 }, { 3, 36, 17, 220 }, { 6, 3, 19, 20 },
};

static bool gnss_on(const fb_ublox_t *d, unsigned gnss_id)
{
    uint8_t bit = gnss_id == 0 ? G_GPS : gnss_id == 2 ? G_GAL : gnss_id == 3 ? G_BDS : G_GLO;
    return d->gnss_enabled & bit;
}

/* the first n satellites of the enabled systems; returns how many */
static int pick_sats(const fb_ublox_t *d, int n, int idx[32])
{
    int k = 0;
    for (int i = 0; i < 32 && k < n; i++) {
        if (gnss_on(d, sky[i][0])) idx[k++] = i;
    }
    return k;
}

static void civil(int64_t days, int *y, int *m, int *dd)
{
    /* days since 1970-01-01 -> y/m/d (H. Hinnant's civil_from_days) */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

typedef struct {
    uint32_t itow;
    int16_t week;
    int year, month, day, hour, min, sec, ms;
} gtime_t;

static gtime_t times(const fb_ublox_t *d)
{
    gtime_t g;
    int64_t unix_ms = d->cfg.epoch_unix * 1000 + (int64_t)(d->t_ns / MS);
    int64_t gps_ms = unix_ms - GPS_EPOCH_UNIX * 1000 + LEAP_S * 1000;
    g.week = (int16_t)(gps_ms / 604800000);
    g.itow = (uint32_t)(gps_ms % 604800000);
    int64_t s = unix_ms / 1000;
    g.ms = (int)(unix_ms % 1000);
    civil(s / 86400, &g.year, &g.month, &g.day);
    g.hour = (int)(s % 86400 / 3600);
    g.min = (int)(s % 3600 / 60);
    g.sec = (int)(s % 60);
    return g;
}

static void epoch(fb_ublox_t *d, double dt)
{
    fb_ublox_solution_t *s = &d->sol;
    double t = (double)d->t_ns / 1e9;
    bool fix = !d->no_fix && t >= d->cfg.fix_delay_s;
    d->had_fix |= fix;

    /* errors: receiver noise (Gauss-Markov), drift walk, spoof offset, glitch */
    double a = exp(-dt / NOISE_TAU_S), k = sqrt(1 - a * a) * d->cfg.noise_m;
    double err[3], vn[3];
    for (int i = 0; i < 3; i++) {
        d->noise[i] = a * d->noise[i] + k * gauss(d);
        if (d->drift) d->drift_ned[i] += d->drift_rate * sqrt(dt) * gauss(d);
        err[i] = d->noise[i] + d->drift_ned[i] + (d->spoof ? d->spoof_ned[i] : 0);
        vn[i] = d->vel[i] + 0.1 * d->cfg.noise_m * gauss(d);
    }
    bool corrupt = false;
    if (d->glitch) {
        if (d->glitch_m != 0) err[0] += d->glitch_m;
        else corrupt = true;
        d->glitch = false;                      /* one-shot */
    }
    double ned[3] = { d->pos[0] + err[0], d->pos[1] + err[1], d->pos[2] + err[2] };
    fb_ubx_ned_to_llh(d->home[0], d->home[1], d->home[2], ned, &s->lat_deg, &s->lon_deg, &s->alt_msl_m);
    memcpy(s->vel_ned, vn, sizeof(vn));
    s->ground_speed_mps = hypot(vn[0], vn[1]);
    if (s->ground_speed_mps >= 0.05) {
        d->course_deg = atan2(vn[1], vn[0]) / DEG;
        if (d->course_deg < 0) d->course_deg += 360;
    }
    s->course_deg = d->course_deg;
    s->fix_type = fix ? 3 : 0;
    int sky_n = pick_sats(d, 32, (int[32]){ 0 });   /* numSV no more than NAV-SAT/SIG can list */
    s->num_sv = fix ? (d->cfg.sats < sky_n ? d->cfg.sats : sky_n) : d->no_fix ? 2
              : (int)fmin(3.0, 3.0 * t / (d->cfg.fix_delay_s > 0 ? d->cfg.fix_delay_s : 1));
    s->time_valid = d->had_fix;
    s->epochs++;
    if (!d->had_fix) {                          /* nothing known yet: zero position */
        s->lat_deg = s->lon_deg = s->alt_msl_m = 0;
    }

    gtime_t g = times(d);
    s->itow_ms = g.itow;
    if (!d->out_ubx) return;

    int sat_idx[32];
    int nsat = pick_sats(d, s->num_sv, sat_idx);
    int32_t lat = i32_sat(s->lat_deg * 1e7), lon = i32_sat(s->lon_deg * 1e7);
    int32_t hmsl = i32_sat(s->alt_msl_m * 1000);
    uint32_t hacc = fix ? u32_sat(d->cfg.h_acc_m * 1000) : 99999000u;
    uint32_t vacc = fix ? u32_sat(d->cfg.h_acc_m * 1600) : 99999000u;
    uint16_t pdop = (uint16_t)(fix ? fmin(9999, lrint(d->cfg.pdop * 100)) : 9999);
    uint8_t fixok = fix ? 1 : 0;
    uint8_t valid = s->time_valid ? 0x07 : 0x00;    /* validDate | validTime | fullyResolved */
    int32_t head = i32_sat(s->course_deg * 1e5);
    uint32_t headacc = s->ground_speed_mps > 0.5 ? 100000u : 18000000u;
    uint8_t p[8 + 32 * 16];
    d->nav_count++;
    bool first = true;

    for (int m = 0; m < NMSG; m++) {
        if (!d->rate[m] || d->nav_count % d->rate[m]) continue;
        int n = 0;
        memset(p, 0, sizeof(p));
        switch (m) {
        case M_POSLLH:
            put_u32(p, g.itow); put_i32(p + 4, lon); put_i32(p + 8, lat); put_i32(p + 12, hmsl);
            put_i32(p + 16, hmsl); put_u32(p + 20, hacc); put_u32(p + 24, vacc);
            n = 28;
            break;
        case M_STATUS:
            put_u32(p, g.itow); p[4] = (uint8_t)s->fix_type; p[5] = (uint8_t)(fixok | (s->time_valid ? 0x0C : 0));
            put_u32(p + 8, d->had_fix ? (uint32_t)(d->cfg.fix_delay_s * 1000) : 0);
            put_u32(p + 12, (uint32_t)(d->t_ns / MS));
            n = 16;
            break;
        case M_SOL: {
            double e[3], v[3], la = s->lat_deg * DEG, lo = s->lon_deg * DEG;
            llh_to_ecef(la, lo, s->alt_msl_m, e);
            /* NED -> ECEF velocity */
            v[0] = -sin(la) * cos(lo) * vn[0] - sin(lo) * vn[1] - cos(la) * cos(lo) * vn[2];
            v[1] = -sin(la) * sin(lo) * vn[0] + cos(lo) * vn[1] - cos(la) * sin(lo) * vn[2];
            v[2] = cos(la) * vn[0] - sin(la) * vn[2];
            put_u32(p, g.itow); put_u16(p + 8, (uint16_t)g.week); p[10] = (uint8_t)s->fix_type;
            p[11] = (uint8_t)(fixok | (s->time_valid ? 0x0C : 0));
            for (int i = 0; i < 3; i++) {
                put_i32(p + 12 + 4 * i, d->had_fix ? i32_sat(e[i] * 100) : 0);
                put_i32(p + 28 + 4 * i, i32_sat(v[i] * 100));
            }
            put_u32(p + 24, hacc / 10); put_u32(p + 40, 10); put_u16(p + 44, pdop); p[47] = (uint8_t)s->num_sv;
            n = 52;
            break;
        }
        case M_PVT:
            put_u32(p, g.itow); put_u16(p + 4, (uint32_t)g.year); p[6] = (uint8_t)g.month; p[7] = (uint8_t)g.day;
            p[8] = (uint8_t)g.hour; p[9] = (uint8_t)g.min; p[10] = (uint8_t)g.sec; p[11] = valid;
            put_u32(p + 12, s->time_valid ? 30 : 0xFFFFFFFFu); put_i32(p + 16, g.ms * 1000000);
            p[20] = (uint8_t)s->fix_type; p[21] = fixok; p[22] = s->time_valid ? 0xE0 : 0; p[23] = (uint8_t)s->num_sv;
            put_i32(p + 24, lon); put_i32(p + 28, lat); put_i32(p + 32, hmsl); put_i32(p + 36, hmsl);
            put_u32(p + 40, hacc); put_u32(p + 44, vacc);
            for (int i = 0; i < 3; i++) put_i32(p + 48 + 4 * i, i32_sat(vn[i] * 1000));
            put_i32(p + 60, i32_sat(s->ground_speed_mps * 1000)); put_i32(p + 64, head);
            put_u32(p + 68, 100); put_u32(p + 72, headacc); put_u16(p + 76, pdop);
            n = 92;
            break;
        case M_VELNED:
            put_u32(p, g.itow);
            for (int i = 0; i < 3; i++) put_i32(p + 4 + 4 * i, i32_sat(vn[i] * 100));
            put_u32(p + 16, u32_sat(sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]) * 100));
            put_u32(p + 20, u32_sat(s->ground_speed_mps * 100)); put_i32(p + 24, head);
            put_u32(p + 28, 10); put_u32(p + 32, headacc);
            n = 36;
            break;
        case M_TIMEUTC:
            put_u32(p, g.itow); put_u32(p + 4, s->time_valid ? 30 : 0xFFFFFFFFu); put_i32(p + 8, g.ms * 1000000);
            put_u16(p + 12, (uint32_t)g.year); p[14] = (uint8_t)g.month; p[15] = (uint8_t)g.day;
            p[16] = (uint8_t)g.hour; p[17] = (uint8_t)g.min; p[18] = (uint8_t)g.sec; p[19] = valid;
            n = 20;
            break;
        case M_SAT:
            put_u32(p, g.itow); p[4] = 1; p[5] = (uint8_t)nsat;
            for (int i = 0; i < nsat; i++) {
                uint8_t *c = p + 8 + 12 * i;
                const uint16_t *sv = sky[sat_idx[i]];
                c[0] = (uint8_t)sv[0]; c[1] = (uint8_t)sv[1]; c[2] = (uint8_t)(24 + sv[2] / 3); c[3] = (uint8_t)sv[2];
                put_u16(c + 4, sv[3]);
                put_u32(c + 8, fix ? (7u | 1u << 3 | 1u << 4) : 4u);      /* quality, svUsed, healthy */
            }
            n = 8 + 12 * nsat;
            break;
        case M_SIG:
            put_u32(p, g.itow); p[4] = 0; p[5] = (uint8_t)nsat;
            for (int i = 0; i < nsat; i++) {
                uint8_t *c = p + 8 + 16 * i;
                const uint16_t *sv = sky[sat_idx[i]];
                c[0] = (uint8_t)sv[0]; c[1] = (uint8_t)sv[1]; c[6] = (uint8_t)(24 + sv[2] / 3); c[7] = fix ? 7 : 4;
                c[9] = 1;                                                     /* Klobuchar */
                put_u16(c + 10, fix ? (1u | 1u << 3 | 1u << 5) : 1u);         /* healthy, pr used, do used */
            }
            n = 8 + 16 * nsat;
            break;
        default:
            continue;                           /* NAV-SVINFO: accepted, not produced */
        }
        send(d, CLS_NAV, msg_id[m], p, n, corrupt && first);
        first = false;
    }
}

void fb_ublox_tick(fb_ublox_t *d, uint64_t dt_ns)
{
    uint64_t end = d->t_ns + dt_ns;
    while (d->next_epoch <= end) {
        uint64_t p = (uint64_t)period_ms(d) * MS;
        d->t_ns = d->next_epoch;
        d->epoch_budget = (uint64_t)d->baud / 10 * period_ms(d) / 1000;   /* bytes per epoch */
        epoch(d, (double)p / 1e9);
        d->next_epoch += p;
    }
    d->t_ns = end;
    d->epoch_budget = UINT64_MAX;               /* answers between epochs: never short of line */
}

uint64_t fb_ublox_next_due_ns(const fb_ublox_t *d)
{
    if (d->tx_len) return 0;
    return d->next_epoch > d->t_ns ? d->next_epoch - d->t_ns : 0;
}

/* ---- introspection ---------------------------------------------------------------------- */

const fb_ublox_solution_t *fb_ublox_solution(const fb_ublox_t *d) { return &d->sol; }
void fb_ublox_stats(const fb_ublox_t *d, fb_ublox_stats_t *st) { *st = d->st; }
uint32_t fb_ublox_nav_period_ms(const fb_ublox_t *d) { return period_ms(d); }
int fb_ublox_dyn_model(const fb_ublox_t *d) { return d->dyn_model; }
uint8_t fb_ublox_gnss_enabled(const fb_ublox_t *d) { return d->gnss_enabled; }

int fb_ublox_msg_rate(const fb_ublox_t *d, uint8_t cls, uint8_t id)
{
    int i = msg_index(d, cls, id);
    return i < 0 ? -1 : d->rate[i];
}

/* ---- faults and descriptor -------------------------------------------------------------- */

static double param(const double *p, int i, double def)
{
    return p && !isnan(p[i]) ? p[i] : def;
}

bool fb_ublox_set_fault(fb_ublox_t *d, const char *fault, bool on, const double *params)
{
    if (!strcmp(fault, "no_fix")) {
        d->no_fix = on;
    } else if (!strcmp(fault, "spoof")) {
        d->spoof = on;
        if (on) {
            d->spoof_ned[0] = param(params, 0, 0);
            d->spoof_ned[1] = param(params, 1, 0);
            d->spoof_ned[2] = -param(params, 2, 0);    /* alt up -> NED down */
        }
    } else if (!strcmp(fault, "drift")) {
        d->drift = on;
        if (on) d->drift_rate = param(params, 0, 0.5);
        else memset(d->drift_ned, 0, sizeof(d->drift_ned));
    } else if (!strcmp(fault, "silence")) {
        d->silence = on;
        if (on) { d->tx_len = 0; d->step = 0; d->line_len = -1; }
    } else if (!strcmp(fault, "glitch")) {
        d->glitch = on;
        if (on) d->glitch_m = param(params, 0, 100);
    } else {
        return false;
    }
    return true;
}

bool fb_ublox_fault_active(const fb_ublox_t *d, const char *fault)
{
    if (!strcmp(fault, "no_fix")) return d->no_fix;
    if (!strcmp(fault, "spoof")) return d->spoof;
    if (!strcmp(fault, "drift")) return d->drift;
    if (!strcmp(fault, "silence")) return d->silence;
    if (!strcmp(fault, "glitch")) return d->glitch;
    return false;
}

static const char *const gen_options[] = { "m10", "m8", NULL };
static const fb_param_desc_t ublox_props[] = {
    { .id = "generation", .label = "receiver generation", .type = FB_PARAM_ENUM, .options = gen_options, .def_str = "m10" },
    { .id = "baud", .label = "power-on baud", .type = FB_PARAM_INT, .has_range = true, .min = 4800, .max = 921600, .has_default = true, .def = 9600 },
    { .id = "rate_hz", .label = "power-on navigation rate Hz", .type = FB_PARAM_INT, .has_range = true, .min = 1, .max = 25, .has_default = true, .def = 10 },
    { .id = "fix_delay_s", .label = "3D fix after power-on, s", .type = FB_PARAM_FLOAT, .has_range = true, .min = 0, .max = 600, .has_default = true, .def = 20 },
    { .id = "noise_m", .label = "position noise sigma m", .type = FB_PARAM_FLOAT, .has_range = true, .min = 0, .max = 20, .has_default = true, .def = 0.3 },
    { .id = "sats", .label = "satellites with a fix", .type = FB_PARAM_INT, .has_range = true, .min = 4, .max = 32, .has_default = true, .def = 16 },
    { .id = "pdop", .label = "position DOP", .type = FB_PARAM_FLOAT, .has_range = true, .min = 0.5, .max = 50, .has_default = true, .def = 1.2 },
    { .id = "h_acc_m", .label = "reported horizontal accuracy m", .type = FB_PARAM_FLOAT, .has_range = true, .min = 0.1, .max = 100, .has_default = true, .def = 1.0 },
    { .id = "seed", .label = "noise seed", .type = FB_PARAM_INT, .has_range = true, .min = 0, .max = 2147483647, .has_default = true, .def = 1 },
    { .id = "epoch_unix", .label = "UTC at power-on (Unix s)", .type = FB_PARAM_INT, .has_range = true, .min = 315964800, .max = 4102444800.0, .has_default = true, .def = 1790640000 },
    { 0 },
};
static const fb_param_desc_t spoof_params[] = {
    { .id = "north_m", .label = "north offset m", .type = FB_PARAM_FLOAT, .has_range = true, .min = -100000, .max = 100000, .has_default = true, .def = 0 },
    { .id = "east_m", .label = "east offset m", .type = FB_PARAM_FLOAT, .has_range = true, .min = -100000, .max = 100000, .has_default = true, .def = 0 },
    { .id = "alt_m", .label = "altitude offset m", .type = FB_PARAM_FLOAT, .has_range = true, .min = -10000, .max = 10000, .has_default = true, .def = 0 },
    { 0 },
};
static const fb_param_desc_t drift_params[] = {
    { .id = "rate_m", .label = "random walk m per sqrt(s)", .type = FB_PARAM_FLOAT, .has_range = true, .min = 0, .max = 50, .has_default = true, .def = 0.5 },
    { 0 },
};
static const fb_param_desc_t glitch_params[] = {
    { .id = "jump_m", .label = "position jump m (0 = broken checksum)", .type = FB_PARAM_FLOAT, .has_range = true, .min = -100000, .max = 100000, .has_default = true, .def = 100 },
    { 0 },
};
static const fb_fault_desc_t ublox_faults[] = {
    { "no_fix", "no fix (messages keep coming, fix type 0)", NULL },
    { "spoof", "spoofed position (offset)", spoof_params },
    { "drift", "position drifts away (random walk)", drift_params },
    { "silence", "receiver silent (no bytes)", NULL },
    { "glitch", "one bad solution (jump or broken frame)", glitch_params },
    { 0 },
};
static const char *const ublox_signals[] = {
    "gps.lat_deg", "gps.lon_deg", "gps.alt_m", "gps.fix", "gps.num_sat", "gps.ground_speed_mps", "gps.course_deg", NULL,
};
static const fb_port_desc_t ublox_ports[] = {
    { .id = "uart", .kind = "uart", .dir = "both", .label = "GPS serial (UBX)" },
    { 0 },
};
/* iNav 9.1.0: FUNCTION_GPS is bit 1 of the serial function mask ("+1" adds it, keeps the rest);
 * gps_provider UBLOX, auto-config and auto-baud are its defaults; 10 Hz instead of its 8 */
static const char *const ublox_config[] = {
    "feature GPS",
    "serial {serial} +1",
    "set gps_provider = UBLOX",
    "set gps_ublox_nav_hz = 10",
    NULL,
};
static const fb_device_desc_t ublox_desc = {
    .id = "ublox", .label = "u-blox GPS (UBX)", .category = "gps", .kind = "uart",
    .props = ublox_props, .faults = ublox_faults, .signals = ublox_signals, .ports = ublox_ports,
    .config = ublox_config,
};
const fb_device_desc_t *fb_ublox_desc(void) { return &ublox_desc; }
