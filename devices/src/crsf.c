/* CRSF receiver model. See include/fb/crsf.h. */
#include "fb/crsf.h"
#include "fb/device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TXQ_SIZE 1024
#define NTYPES 256

struct fb_crsf {
    /* inputs */
    uint16_t raw[FB_CRSF_CHANNELS];
    int rssi_dbm, lq, snr;
    uint32_t packet_hz, linkstats_period_ms;

    /* faults (kept across reset) */
    bool link_lost, freeze, lq_fault;
    uint16_t frozen_raw[FB_CRSF_CHANNELS];
    double lq_params[3];

    /* time */
    uint64_t t_ns;
    uint64_t next_rc_ns;
    uint64_t next_ls_ns;

    /* tx queue (bytes the receiver sends to the FC) */
    uint8_t txq[TXQ_SIZE];
    int tx_head, tx_tail;

    /* rx parser (bytes from the FC) */
    uint8_t rxbuf[FB_CRSF_FRAME_MAX];
    int rx_len;
    uint32_t rx_frames, rx_crc_errors;
    uint8_t last_payload[NTYPES][FB_CRSF_FRAME_MAX];
    int8_t  last_len[NTYPES];
    uint32_t seen[NTYPES];
};

/* ---- helpers ---------------------------------------------------------------------------- */

uint8_t fb_crsf_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint16_t fb_crsf_us_to_raw(uint16_t us)
{
    float raw = 992.0f + ((float)us - 1500.0f) / 0.62477120195241f;
    if (raw < 0) raw = 0;
    if (raw > 2047) raw = 2047;
    return (uint16_t)lrintf(raw);
}

uint16_t fb_crsf_raw_to_us(uint16_t raw)
{
    return (uint16_t)lrintf(1500.0f + ((float)raw - 992.0f) * 0.62477120195241f);
}

int fb_crsf_build_frame(uint8_t *out, uint8_t type, const uint8_t *payload, int len)
{
    if (len < 0 || len > FB_CRSF_FRAME_MAX - 4) return 0;
    out[0] = FB_CRSF_SYNC;
    out[1] = (uint8_t)(len + 2);
    out[2] = type;
    if (len) memcpy(&out[3], payload, (size_t)len);
    out[3 + len] = fb_crsf_crc8(&out[2], len + 1);
    return len + 4;
}

bool fb_crsf_decode_battery(const uint8_t *p, int len, float *volts, float *amps, uint32_t *mah, uint8_t *remaining_pct)
{
    if (len < 8) return false;
    if (volts) *volts = (float)((p[0] << 8) | p[1]) / 10.0f;
    if (amps)  *amps  = (float)((p[2] << 8) | p[3]) / 10.0f;
    if (mah)   *mah   = ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 8) | p[6];
    if (remaining_pct) *remaining_pct = p[7];
    return true;
}

bool fb_crsf_decode_attitude(const uint8_t *p, int len, float *pitch_deg, float *roll_deg, float *yaw_deg)
{
    if (len < 6) return false;
    const float k = 180.0f / 3.14159265f / 10000.0f;   /* payload is radians * 10000, big-endian int16 */
    int16_t pi = (int16_t)((p[0] << 8) | p[1]), ro = (int16_t)((p[2] << 8) | p[3]), ya = (int16_t)((p[4] << 8) | p[5]);
    if (pitch_deg) *pitch_deg = pi * k;
    if (roll_deg)  *roll_deg  = ro * k;
    if (yaw_deg)   *yaw_deg   = ya * k;
    return true;
}

/* ---- lifecycle -------------------------------------------------------------------------- */

static void defaults(fb_crsf_t *d)
{
    for (int i = 0; i < FB_CRSF_CHANNELS; i++) d->raw[i] = 992;          /* centre */
    d->raw[2] = fb_crsf_us_to_raw(1000);                                   /* throttle low (AETR order is the FC's business) */
    d->rssi_dbm = -60; d->lq = 100; d->snr = 10;
    d->packet_hz = 250; d->linkstats_period_ms = 200;
    d->t_ns = 0; d->next_rc_ns = 0; d->next_ls_ns = 0;
    d->tx_head = d->tx_tail = 0;
    d->rx_len = 0; d->rx_frames = d->rx_crc_errors = 0;
    memset(d->last_len, -1, sizeof(d->last_len));
    memset(d->seen, 0, sizeof(d->seen));
}

fb_crsf_t *fb_crsf_new(void)
{
    fb_crsf_t *d = calloc(1, sizeof(*d));
    if (d) defaults(d);     /* calloc: no faults */
    return d;
}
void fb_crsf_free(fb_crsf_t *d) { free(d); }
void fb_crsf_reset(fb_crsf_t *d) { defaults(d); }

/* ---- inputs ----------------------------------------------------------------------------- */

void fb_crsf_set_channels_us(fb_crsf_t *d, const uint16_t us[FB_CRSF_CHANNELS])
{
    for (int i = 0; i < FB_CRSF_CHANNELS; i++) d->raw[i] = fb_crsf_us_to_raw(us[i]);
}

void fb_crsf_set_link(fb_crsf_t *d, int rssi_dbm, int lq_percent, int snr_db, bool link_up)
{
    d->rssi_dbm = rssi_dbm; d->lq = lq_percent; d->snr = snr_db; d->link_lost = !link_up;
}

void fb_crsf_set_rates(fb_crsf_t *d, uint32_t packet_hz, uint32_t linkstats_period_ms)
{
    d->packet_hz = packet_hz ? packet_hz : 250;
    d->linkstats_period_ms = linkstats_period_ms ? linkstats_period_ms : 200;
}

/* ---- descriptor and faults ------------------------------------------------------------- */

static const fb_param_desc_t lq_params[] = {
    { .id = "lq", .label = "link quality %", .type = FB_PARAM_INT, .has_range = true, .min = 0, .max = 100 },
    { .id = "rssi_dbm", .label = "RSSI dBm", .type = FB_PARAM_INT, .has_range = true, .min = -130, .max = 0,
      .optional = true },
    { .id = "snr_db", .label = "SNR dB", .type = FB_PARAM_INT, .has_range = true, .min = -30, .max = 30,
      .optional = true },
    { 0 },
};
static const fb_fault_desc_t crsf_faults[] = {
    { "link_lost", "radio link lost (receiver silent, LQ 0)", NULL },
    { "freeze", "receiver frozen (repeats the last frame)", NULL },
    { "lq", "link quality", lq_params },
    { 0 },
};
static const char *const crsf_signals[] = {
    "rc.ch1_us", "rc.ch2_us", "rc.ch3_us", "rc.ch4_us", "rc.ch5_us", "rc.ch6_us", "rc.ch7_us",
    "rc.ch8_us", "rc.ch9_us", "rc.ch10_us", "rc.ch11_us", "rc.ch12_us", "rc.ch13_us", "rc.ch14_us",
    "rc.ch15_us", "rc.ch16_us", "link.up", "link.rssi_dbm", "link.lq", "link.snr_db",
    "crsf.telemetry_frames", NULL,
};
static const fb_port_desc_t crsf_ports[] = {
    { .id = "uart", .kind = "uart", .dir = "both", .label = "CRSF serial" },
    { 0 },
};
static const fb_device_desc_t crsf_desc = {
    .id = "crsf", .label = "CRSF receiver", .category = "receiver", .kind = "uart",
    .faults = crsf_faults, .signals = crsf_signals, .ports = crsf_ports,
};
const fb_device_desc_t *fb_crsf_desc(void) { return &crsf_desc; }

bool fb_crsf_set_fault(fb_crsf_t *d, const char *fault, bool on, const double *params)
{
    if (!strcmp(fault, "link_lost")) {
        d->link_lost = on;
    } else if (!strcmp(fault, "freeze")) {
        if (on && !d->freeze) memcpy(d->frozen_raw, d->raw, sizeof(d->raw));
        d->freeze = on;
    } else if (!strcmp(fault, "lq")) {
        d->lq_fault = on;
        for (int i = 0; i < 3; i++) d->lq_params[i] = params ? params[i] : NAN;
    } else {
        return false;
    }
    return true;
}

bool fb_crsf_fault_active(const fb_crsf_t *d, const char *fault)
{
    if (!strcmp(fault, "link_lost")) return d->link_lost;
    if (!strcmp(fault, "freeze")) return d->freeze;
    if (!strcmp(fault, "lq")) return d->lq_fault;
    return false;
}

void fb_crsf_link_reported(const fb_crsf_t *d, int *rssi_dbm, int *lq, int *snr_db, bool *link_up)
{
    int r = d->rssi_dbm, q = d->lq, s = d->snr;
    if (d->lq_fault) {
        if (!isnan(d->lq_params[0])) q = (int)lrint(d->lq_params[0]);
        if (!isnan(d->lq_params[1])) r = (int)lrint(d->lq_params[1]);
        if (!isnan(d->lq_params[2])) s = (int)lrint(d->lq_params[2]);
    }
    if (d->link_lost) q = 0;
    if (rssi_dbm) *rssi_dbm = r;
    if (lq) *lq = q;
    if (snr_db) *snr_db = s;
    if (link_up) *link_up = !d->link_lost;
}

/* ---- tx --------------------------------------------------------------------------------- */

static void tx_push(fb_crsf_t *d, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        int next = (d->tx_head + 1) % TXQ_SIZE;
        if (next == d->tx_tail) return;                       /* full: drop (UART would overrun) */
        d->txq[d->tx_head] = b[i];
        d->tx_head = next;
    }
}

int fb_crsf_tx_pending(const fb_crsf_t *d)
{
    return (d->tx_head - d->tx_tail + TXQ_SIZE) % TXQ_SIZE;
}

int fb_crsf_tx_discard(fb_crsf_t *d)
{
    int n = fb_crsf_tx_pending(d);
    d->tx_tail = d->tx_head;
    return n;
}

uint64_t fb_crsf_next_due_ns(const fb_crsf_t *d)
{
    if (d->link_lost) return 1000000000ull / d->packet_hz;
    uint64_t next = d->next_rc_ns < d->next_ls_ns ? d->next_rc_ns : d->next_ls_ns;
    return next > d->t_ns ? next - d->t_ns : 0;
}

int fb_crsf_tx_pop(fb_crsf_t *d, uint8_t *buf, int max)
{
    int n = 0;
    while (n < max && d->tx_tail != d->tx_head) {
        buf[n++] = d->txq[d->tx_tail];
        d->tx_tail = (d->tx_tail + 1) % TXQ_SIZE;
    }
    return n;
}

static void send_rc(fb_crsf_t *d)
{
    uint8_t payload[22] = {0};
    const uint16_t *raw = d->freeze ? d->frozen_raw : d->raw;
    /* 16 x 11-bit, LSB first, packed as the little-endian bitfield struct in rx/crsf.c */
    uint32_t bitpos = 0;
    for (int ch = 0; ch < FB_CRSF_CHANNELS; ch++) {
        uint32_t v = raw[ch] & 0x7FF;
        for (int b = 0; b < 11; b++, bitpos++) {
            if (v & (1u << b)) payload[bitpos >> 3] |= (uint8_t)(1u << (bitpos & 7));
        }
    }
    uint8_t frame[FB_CRSF_FRAME_MAX];
    int n = fb_crsf_build_frame(frame, FB_CRSF_RC_CHANNELS, payload, 22);
    tx_push(d, frame, n);
}

static void send_linkstats(fb_crsf_t *d)
{
    int r, q, s;
    fb_crsf_link_reported(d, &r, &q, &s, NULL);
    uint8_t rssi = (uint8_t)(r < 0 ? -r : r);   /* sent as -dBm */
    uint8_t p[10] = {
        rssi, rssi, (uint8_t)q, (uint8_t)(int8_t)s,
        0,                       /* active antenna */
        2,                       /* rf mode (ELRS: index of packet rate; informational) */
        0,                       /* uplink tx power index */
        rssi, (uint8_t)q, (uint8_t)(int8_t)s,
    };
    uint8_t frame[FB_CRSF_FRAME_MAX];
    int n = fb_crsf_build_frame(frame, FB_CRSF_LINK_STATISTICS, p, 10);
    tx_push(d, frame, n);
}

void fb_crsf_tick(fb_crsf_t *d, uint64_t dt_ns)
{
    d->t_ns += dt_ns;
    if (d->link_lost) {                     /* receiver lost the link: silence, as a real RX would */
        d->next_rc_ns = d->t_ns; d->next_ls_ns = d->t_ns;
        return;
    }
    uint64_t rc_period = 1000000000ull / d->packet_hz;
    uint64_t ls_period = (uint64_t)d->linkstats_period_ms * 1000000ull;
    int guard = 0;
    while (d->t_ns >= d->next_rc_ns && guard++ < 64) {
        send_rc(d);
        d->next_rc_ns += rc_period;
    }
    guard = 0;
    while (d->t_ns >= d->next_ls_ns && guard++ < 8) {
        send_linkstats(d);
        d->next_ls_ns += ls_period;
    }
}

/* ---- rx (telemetry from the FC) ------------------------------------------------------- */

void fb_crsf_rx_byte(fb_crsf_t *d, uint8_t b)
{
    if (d->rx_len == 0) {
        if (b != FB_CRSF_SYNC && b != 0xEE && b != 0xEA && b != 0xC8) return;   /* sync / addresses seen from FCs */
        d->rxbuf[d->rx_len++] = b;
        return;
    }
    if (d->rx_len == 1) {
        if (b < 2 || b > FB_CRSF_FRAME_MAX - 2) { d->rx_len = 0; return; }
        d->rxbuf[d->rx_len++] = b;
        return;
    }
    d->rxbuf[d->rx_len++] = b;
    int total = d->rxbuf[1] + 2;
    if (d->rx_len < total) return;
    /* complete frame */
    int len = d->rxbuf[1];                            /* type + payload + crc */
    uint8_t crc = fb_crsf_crc8(&d->rxbuf[2], len - 1);
    if (crc == d->rxbuf[2 + len - 1]) {
        uint8_t type = d->rxbuf[2];
        int plen = len - 2;
        d->rx_frames++;
        memcpy(d->last_payload[type], &d->rxbuf[3], (size_t)plen);
        d->last_len[type] = (int8_t)plen;
        d->seen[type]++;
    } else {
        d->rx_crc_errors++;
    }
    d->rx_len = 0;
}

int fb_crsf_telemetry_last(const fb_crsf_t *d, uint8_t type, uint8_t *buf, int max, uint32_t *count)
{
    if (count) *count = d->seen[type];
    if (d->last_len[type] < 0) return -1;
    int n = d->last_len[type];
    if (n > max) n = max;
    memcpy(buf, d->last_payload[type], (size_t)n);
    return d->last_len[type];
}

uint32_t fb_crsf_rx_frames(const fb_crsf_t *d) { return d->rx_frames; }
uint32_t fb_crsf_rx_crc_errors(const fb_crsf_t *d) { return d->rx_crc_errors; }
