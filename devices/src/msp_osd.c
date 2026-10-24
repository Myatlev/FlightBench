/* MSP DisplayPort OSD device (HD VTX side). See include/fb/msp_osd.h. */
#include "fb/msp_osd.h"
#include "fb/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { P_IDLE, P_M, P_DIR, P_LEN, P_CMD, P_JLO, P_JHI, P_PAYLOAD, P_CK };

/* raw bytes of the frame being parsed, from its '$' (rescanned after a bad checksum): header 3,
 * size, cmd, jumbo size 2, payload up to 256, checksum */
#define RAW_MAX (3 + 1 + 1 + 2 + 256 + 1)
#define DRAW_UNMEASURED_NS 1000000000ull    /* the firmware's slowest OSD rate: osd_framerate_hz 1 */
/* The frame interval is the longest of the last GAPS DRAW-to-DRAW gaps. Gaps shorter than the
 * firmware's fastest frame (osd_framerate_hz 60) are extra DRAWs, not frames: at boot RotorFlight
 * sends 3 DRAWs within 2 ms after each boot (measured in the emulator, 2 Hz and 12 Hz alike). */
#define GAPS 8
#define DRAW_MIN_NS 16000000ull

struct fb_msp_osd {
    int rows, cols;
    uint8_t draw_ch[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];     /* drawing buffer */
    uint8_t draw_at[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];
    uint8_t show_ch[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];     /* shown canvas */
    uint8_t show_at[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];
    uint8_t took_ch[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];     /* shown canvas at the last take */
    uint8_t took_at[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];
    uint8_t changed[FB_MSP_OSD_ROWS_MAX][FB_MSP_OSD_COLS_MAX];
    int nchanged;
    bool frame_pending, full_pending;

    /* parser */
    int st;
    char dir;
    uint8_t cmd, ck;
    uint16_t len, n;            /* payload size (jumbo: up to 65535) and bytes read */
    bool oversize;              /* jumbo frame larger than payload[]: read and dropped */
    uint8_t payload[256];
    uint8_t raw[RAW_MAX];
    int raw_n;                  /* -1: not recorded (oversize) */

    /* link */
    uint64_t now_ns, last_rx_ns;
    uint64_t last_draw_ns;      /* the last DRAW_SCREEN (have_draw; cleared on reset/contact_lost) */
    uint64_t gaps[GAPS];        /* the last DRAW-to-DRAW intervals >= DRAW_MIN_NS */
    int gap_n, gap_i;
    bool have_draw;
    bool contact;
    bool poll_on;               /* fb_msp_osd_set_poll(): the MSP_FC_VARIANT requests (default on) */
    uint64_t next_poll_ns;      /* the next MSP_FC_VARIANT request */
    char variant[5];            /* the FC's MSP_FC_VARIANT reply, "" = none */
    bool have_options;
    uint8_t opt_font, opt_mode; /* the last OPTIONS sub-command */
    uint8_t tx[64];
    int tx_len;

    bool link_lost;         /* fault, kept across reset */
    fb_msp_osd_stats_t stats;
};

int fb_msp_v1_frame(uint8_t *out, char dir, uint8_t cmd, const uint8_t *payload, int len)
{
    uint8_t ck = (uint8_t)len ^ cmd;
    out[0] = '$';
    out[1] = 'M';
    out[2] = (uint8_t)dir;
    out[3] = (uint8_t)len;
    out[4] = cmd;
    for (int i = 0; i < len; i++) {
        out[5 + i] = payload[i];
        ck ^= payload[i];
    }
    out[5 + len] = ck;
    return 6 + len;
}

static void blank(uint8_t ch[][FB_MSP_OSD_COLS_MAX], uint8_t at[][FB_MSP_OSD_COLS_MAX])
{
    memset(ch, FB_MSP_OSD_BLANK, FB_MSP_OSD_ROWS_MAX * FB_MSP_OSD_COLS_MAX);
    memset(at, 0, FB_MSP_OSD_ROWS_MAX * FB_MSP_OSD_COLS_MAX);
}

void fb_msp_osd_reset(fb_msp_osd_t *d)
{
    blank(d->draw_ch, d->draw_at);
    blank(d->show_ch, d->show_at);
    blank(d->took_ch, d->took_at);
    memset(d->changed, 0, sizeof(d->changed));
    d->nchanged = 0;
    d->frame_pending = false;
    d->full_pending = true;
    d->st = P_IDLE;
    d->raw_n = 0;
    d->contact = false;
    d->have_draw = false;
    d->gap_n = d->gap_i = 0;
    d->tx_len = 0;
    d->next_poll_ns = d->now_ns + FB_MSP_OSD_POLL_NS;
    d->variant[0] = 0;
    d->have_options = false;
}

fb_msp_osd_t *fb_msp_osd_new(int rows, int cols)
{
    fb_msp_osd_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->rows = rows < 1 ? 1 : rows > FB_MSP_OSD_ROWS_MAX ? FB_MSP_OSD_ROWS_MAX : rows;
    d->cols = cols < 1 ? 1 : cols > FB_MSP_OSD_COLS_MAX ? FB_MSP_OSD_COLS_MAX : cols;
    d->poll_on = true;
    fb_msp_osd_reset(d);
    return d;
}

void fb_msp_osd_free(fb_msp_osd_t *d) { free(d); }
int fb_msp_osd_rows(const fb_msp_osd_t *d) { return d->rows; }
int fb_msp_osd_cols(const fb_msp_osd_t *d) { return d->cols; }
const fb_msp_osd_stats_t *fb_msp_osd_stats(const fb_msp_osd_t *d) { return &d->stats; }
const char *fb_msp_osd_fc_variant(const fb_msp_osd_t *d) { return d->variant; }

const char *fb_msp_osd_font(const fb_msp_osd_t *d)
{
    return !strcmp(d->variant, "INAV") ? "inav" : !strcmp(d->variant, "RTFL") ? "rotorflight" : NULL;
}

bool fb_msp_osd_options(const fb_msp_osd_t *d, int *font, int *mode)
{
    if (d->have_options) {
        if (font) *font = d->opt_font;
        if (mode) *mode = d->opt_mode;
    }
    return d->have_options;
}

/* ---- descriptor and faults ------------------------------------------------------------- */

static const fb_fault_desc_t msp_osd_faults[] = {
    { "link_lost", "OSD device silent (ignores the FC, sends nothing: no picture)", NULL },
    { 0 },
};
static const char *const msp_osd_signals[] = { "osd.frames", NULL };
static const fb_port_desc_t msp_osd_ports[] = {
    { .id = "uart", .kind = "uart", .dir = "both", .label = "MSP DisplayPort serial" },
    { 0 },
};
/* CLI lines the core adds to the custom defaults / the CLI script when the device is wired, by the
 * board's firmware family ({serial} = the pad's serial index, the CLI's `serial <n>`). */
/* RotorFlight 4.6: MSP on the port at 115200, the MSP DisplayPort on it, the OSD device left at AUTO,
 * and a small element layout for the 50x18 HD canvas. Positions: x | y << 5 | 2048 (OSD profile 1);
 * x has 5 bits in RotorFlight 4.6, so columns 0..31 only. */
static const char *const msp_osd_config_rotorflight[] = {
    "serial {serial} 1 115200 57600 0 115200",
    "set displayport_msp_serial = {serial}",
    /* AUTO, not MSP, even if a session profile set MSP: the device's MSP_SET_OSD_CANVAS then finds
     * the OSD not yet MSP + HD, and the firmware sets both, saves (the 50x18 canvas with them) and
     * reboots; with MSP + HD already set it would keep the canvas in RAM only (msp.c) */
    "set osd_displayport_device = AUTO",
    "set osd_flymode_pos = 2081",       /* x 1,  y 1  */
    "set osd_tim_1_pos = 2104",         /* x 24, y 1  timer 1: on time */
    "set osd_tim_2_pos = 2136",         /* x 24, y 2  timer 2: total armed time */
    "set osd_disarmed_pos = 2197",      /* x 21, y 4  */
    "set osd_crosshairs_pos = 2328",    /* x 24, y 8  */
    "set osd_warnings_pos = 2451",      /* x 19, y 12 */
    "set osd_esc_rpm_pos = 2529",       /* x 1,  y 15 motor rpm (bidirectional DShot) */
    "set osd_vbat_pos = 2561",          /* x 1,  y 16 */
    "set osd_debug_pos = 2593",         /* x 1,  y 17 debug[0..3]; debug_mode GOVERNOR: [2] = head speed */
    NULL,
};
/* iNav 9.1.0: FUNCTION_MSP_OSD is bit 25 of the serial function mask (io/serial.h:59, 33554432) and
 * the only function on the port; mspOsdSerialInit opens it at the peripheral baud, the 4th baud of
 * `serial` (io/displayport_msp_osd.c:425-426, fc/cli.c:881-910: msp, gps, telemetry, peripheral).
 * fc_init.c:559-576 picks FrSky OSD, then this MSP OSD, then the MAX7456, so the board's chip stays
 * idle. osd_video_system HDZERO = mode HD_5018, the 50x18 canvas (displayport_msp_osd.c:460-464;
 * AUTO/PAL would draw 30x16). FEATURE_OSD is in the target's DEFAULT_FEATURES; set anyway. */
/* The layout: iNav's default (io/osd.c pgResetFn_osdLayoutsConfig) is for 30 columns and hides the
 * horizon; `osd_layout <layout> <item> <col> <row> V` (fc/cli.c cliOsdLayout; items io/osd.h
 * osd_items_e) spreads it over 50x18. The horizon, its sidebars and the crosshairs are drawn
 * around the canvas centre whatever their position (osdCrosshairPosition: cols / 2, rows / 2 =
 * 25, 9; sidebars 7 columns out, 3 + 1 rows up and down): nothing else goes into columns 18..32
 * of rows 5..13. */
static const char *const msp_osd_config_inav[] = {
    "feature OSD",
    "serial {serial} 33554432 115200 115200 0 115200",
    "set osd_video_system = HDZERO",
    "osd_layout 0 7 2 1 V",         /* FLYMODE */
    "osd_layout 0 14 2 2 V",        /* GPS_SATS */
    "osd_layout 0 24 23 1 V",       /* HEADING */
    "osd_layout 0 0 44 1 V",        /* RSSI_VALUE */
    "osd_layout 0 3 0 0 V",         /* ARTIFICIAL_HORIZON (at the centre) */
    "osd_layout 0 4 0 0 V",         /* HORIZON_SIDEBARS (around the horizon) */
    "osd_layout 0 2 0 0 V",         /* CROSSHAIRS (at the centre) */
    "osd_layout 0 13 8 9 V",        /* GPS_SPEED, left of the horizon */
    "osd_layout 0 15 36 9 V",       /* ALTITUDE, right of the horizon */
    "osd_layout 0 22 24 14 V",      /* HOME_DIR */
    "osd_layout 0 23 26 14 V",      /* HOME_DIST */
    "osd_layout 0 30 11 15 V",      /* MESSAGES */
    "osd_layout 0 9 2 14 V",        /* THROTTLE_POS */
    "osd_layout 0 11 2 15 V",       /* CURRENT_DRAW */
    "osd_layout 0 1 2 16 V",        /* MAIN_BATT_VOLTAGE */
    "osd_layout 0 12 10 16 V",      /* MAH_DRAWN */
    "osd_layout 0 28 41 16 V",      /* ONTIME_FLYTIME */
    NULL,
};
static const fb_config_fw_t msp_osd_config[] = {
    { "rotorflight", msp_osd_config_rotorflight },
    { "inav", msp_osd_config_inav },
    { 0 },
};
static const fb_device_desc_t msp_osd_desc = {
    .id = "msp_osd", .label = "HD VTX OSD (MSP DisplayPort)", .category = "osd", .kind = "uart",
    .faults = msp_osd_faults, .signals = msp_osd_signals, .ports = msp_osd_ports,
    .config_fw = msp_osd_config,
};
const fb_device_desc_t *fb_msp_osd_desc(void) { return &msp_osd_desc; }

bool fb_msp_osd_set_fault(fb_msp_osd_t *d, const char *fault, bool on, const double *params)
{
    (void)params;
    if (strcmp(fault, "link_lost")) {
        return false;
    }
    if (on) {
        d->st = P_IDLE;
        d->tx_len = 0;
        d->frame_pending = false;
    } else if (d->link_lost) {
        d->full_pending = true;     /* the picture comes back whole */
        d->contact = false;         /* and the canvas is announced again */
    }
    d->link_lost = on;
    return true;
}

bool fb_msp_osd_fault_active(const fb_msp_osd_t *d, const char *fault)
{
    return !strcmp(fault, "link_lost") && d->link_lost;
}

/* ---- link ------------------------------------------------------------------------------ */

uint64_t fb_msp_osd_draw_interval_ns(const fb_msp_osd_t *d)
{
    uint64_t iv = 0;
    for (int i = 0; i < d->gap_n; i++) {
        iv = d->gaps[i] > iv ? d->gaps[i] : iv;
    }
    return iv;
}

uint64_t fb_msp_osd_silence_ns(const fb_msp_osd_t *d)
{
    uint64_t iv = d->gap_n ? fb_msp_osd_draw_interval_ns(d) : DRAW_UNMEASURED_NS;
    return 3 * iv > FB_MSP_OSD_SILENCE_NS ? 3 * iv : FB_MSP_OSD_SILENCE_NS;
}

void fb_msp_osd_contact_lost(fb_msp_osd_t *d)
{
    d->contact = false;
    d->have_draw = false;       /* the rebooted firmware may run another frame rate: measure again */
    d->gap_n = d->gap_i = 0;
    d->st = P_IDLE;
    d->variant[0] = 0;
    d->have_options = false;
}

static void poll(fb_msp_osd_t *d)
{
    /* one request in flight at most: while the FC's USART does not take bytes (it has not opened
     * the port yet) the queue does not grow */
    if (!d->link_lost && !d->tx_len) {
        d->tx_len += fb_msp_v1_frame(d->tx + d->tx_len, '<', FB_MSP_FC_VARIANT, NULL, 0);
        d->stats.polls++;
    }
}

void fb_msp_osd_set_poll(fb_msp_osd_t *d, bool on) { d->poll_on = on; }

uint64_t fb_msp_osd_poll_in_ns(const fb_msp_osd_t *d)
{
    if (!d->poll_on) {
        return UINT64_MAX;
    }
    return d->next_poll_ns > d->now_ns ? d->next_poll_ns - d->now_ns : 0;
}

void fb_msp_osd_tick(fb_msp_osd_t *d, uint64_t dt_ns)
{
    d->now_ns += dt_ns;
    if (d->poll_on && d->now_ns >= d->next_poll_ns) {
        poll(d);
        /* on the grid from power-on; a late tick does not queue the missed ones */
        d->next_poll_ns += (d->now_ns - d->next_poll_ns) / FB_MSP_OSD_POLL_NS * FB_MSP_OSD_POLL_NS
                           + FB_MSP_OSD_POLL_NS;
    }
    if (d->contact && d->now_ns - d->last_rx_ns > fb_msp_osd_silence_ns(d)) {
        /* have_draw stays: if the firmware was only slower than the threshold, the gap to its next
         * DRAW is the new frame interval (one lost contact, then the threshold fits) */
        d->contact = false;
    }
}

bool fb_msp_osd_link_up(const fb_msp_osd_t *d)
{
    return d->contact && !d->link_lost && d->now_ns - d->last_rx_ns <= fb_msp_osd_silence_ns(d);
}

int fb_msp_osd_tx_pending(const fb_msp_osd_t *d) { return d->tx_len; }

int fb_msp_osd_tx_pop(fb_msp_osd_t *d, uint8_t *buf, int max)
{
    int n = d->tx_len < max ? d->tx_len : max;
    if (n <= 0) return 0;
    memcpy(buf, d->tx, n);
    memmove(d->tx, d->tx + n, d->tx_len - n);
    d->tx_len -= n;
    return n;
}

static void announce_canvas(fb_msp_osd_t *d)
{
    uint8_t p[2] = { (uint8_t)d->cols, (uint8_t)d->rows };
    if (d->tx_len + 8 <= (int)sizeof(d->tx)) {
        d->tx_len += fb_msp_v1_frame(d->tx + d->tx_len, '<', FB_MSP_SET_OSD_CANVAS, p, 2);
        d->stats.canvas_sent++;
    }
}

/* ---- DisplayPort ----------------------------------------------------------------------- */

static void displayport(fb_msp_osd_t *d, const uint8_t *p, int n)
{
    if (n < 1) {
        d->stats.unknown_sub++;
        return;
    }
    switch (p[0]) {
    case FB_MSP_DP_HEARTBEAT:
        d->stats.heartbeats++;
        break;
    case FB_MSP_DP_RELEASE:
        d->stats.releases++;
        blank(d->draw_ch, d->draw_at);
        blank(d->show_ch, d->show_at);
        d->full_pending = true;
        break;
    case FB_MSP_DP_CLEAR_SCREEN:
        d->stats.clears++;
        blank(d->draw_ch, d->draw_at);
        break;
    case FB_MSP_DP_WRITE_STRING:
        if (n < 4) {
            d->stats.unknown_sub++;
            break;
        }
        d->stats.writes++;
        for (int i = 4; i < n; i++) {
            int r = p[1], c = p[2] + (i - 4);
            if (r >= d->rows || c >= d->cols) {
                d->stats.clipped++;
                continue;
            }
            d->draw_ch[r][c] = p[i];
            d->draw_at[r][c] = p[3];
        }
        break;
    case FB_MSP_DP_DRAW_SCREEN:
        d->stats.frames++;
        if (d->have_draw && d->now_ns - d->last_draw_ns >= DRAW_MIN_NS) {
            uint64_t gap = d->now_ns - d->last_draw_ns;
            d->gaps[d->gap_i] = gap < DRAW_UNMEASURED_NS ? gap : DRAW_UNMEASURED_NS;
            d->gap_i = (d->gap_i + 1) % GAPS;
            d->gap_n += d->gap_n < GAPS;
        }
        d->last_draw_ns = d->now_ns;
        d->have_draw = true;
        memcpy(d->show_ch, d->draw_ch, sizeof(d->show_ch));
        memcpy(d->show_at, d->draw_at, sizeof(d->show_at));
        d->frame_pending = true;
        break;
    case FB_MSP_DP_OPTIONS:
        d->stats.options++;
        if (n >= 3) {
            d->have_options = true;
            d->opt_font = p[1];
            d->opt_mode = p[2];
        }
        break;
    case FB_MSP_DP_SYS:
        d->stats.sys++;
        break;
    default:
        d->stats.unknown_sub++;
        break;
    }
}

static void frame_done(fb_msp_osd_t *d)
{
    d->last_rx_ns = d->now_ns;
    if (!d->contact) {
        /* a new contact: the FC (re)started talking; announce the canvas like an HD VTX */
        d->contact = true;
        d->stats.contacts++;
        d->full_pending = true;
        announce_canvas(d);
    }
    if (d->cmd == FB_MSP_DISPLAYPORT && d->dir == '>') {
        displayport(d, d->payload, d->len);
    } else if (d->cmd == FB_MSP_SET_OSD_CANVAS && d->dir == '>') {
        d->stats.canvas_acks++;
    } else if (d->cmd == FB_MSP_FC_VARIANT && d->dir == '>' && d->len >= 4) {
        d->stats.variants++;
        for (int i = 0; i < 4; i++) {
            uint8_t c = d->payload[i];
            d->variant[i] = c >= 0x20 && c < 0x7f ? (char)c : '?';
        }
        d->variant[4] = 0;
    } else if (d->dir == '!') {
        d->stats.errors++;
    } else {
        d->stats.other_frames++;
    }
}

/* one byte through the parser; returns false on a bad checksum (the caller rescans d->raw) */
static bool parse(fb_msp_osd_t *d, uint8_t b)
{
    if (d->st != P_IDLE && d->raw_n >= 0) {
        if (d->raw_n < RAW_MAX) {
            d->raw[d->raw_n++] = b;
        } else {
            d->raw_n = -1;
        }
    }
    switch (d->st) {
    case P_IDLE:
        if (b == '$') {
            d->st = P_M;
            d->raw[0] = b;
            d->raw_n = 1;
        }
        break;
    case P_M:
        if (b == 'M') {
            d->st = P_DIR;
        } else {
            d->st = P_IDLE;
            if (b == '$') parse(d, b);
        }
        break;
    case P_DIR:
        if (b == '>' || b == '<' || b == '!') {
            d->dir = (char)b;
            d->st = P_LEN;
        } else {
            d->st = P_IDLE;
            if (b == '$') parse(d, b);
        }
        break;
    case P_LEN:
        d->len = b;             /* 255 = jumbo: the 16-bit size follows the command */
        d->ck = b;
        d->n = 0;
        d->oversize = false;
        d->st = P_CMD;
        break;
    case P_CMD:
        d->cmd = b;
        d->ck ^= b;
        d->st = d->len == 255 ? P_JLO : d->len ? P_PAYLOAD : P_CK;
        break;
    case P_JLO:
        d->len = b;
        d->ck ^= b;
        d->st = P_JHI;
        break;
    case P_JHI:
        d->len |= (uint16_t)(b << 8);
        d->ck ^= b;
        d->stats.jumbo++;
        d->oversize = d->len > sizeof(d->payload);
        d->st = d->len ? P_PAYLOAD : P_CK;
        break;
    case P_PAYLOAD:
        if (d->n < sizeof(d->payload)) {
            d->payload[d->n] = b;
        }
        d->n++;
        d->ck ^= b;
        if (d->n == d->len) d->st = P_CK;
        break;
    default:
        d->st = P_IDLE;
        if (b != d->ck) {
            d->stats.bad_checksum++;
            return false;
        }
        if (d->oversize) {
            d->stats.oversize++;    /* nothing the device uses is that long: dropped */
        } else {
            frame_done(d);
        }
        break;
    }
    return true;
}

void fb_msp_osd_rx_byte(fb_msp_osd_t *d, uint8_t b)
{
    if (d->link_lost) {
        return;
    }
    /* A bad checksum usually means the frame was cut (the FC reset mid-frame) and the bytes read
     * as its payload are the next frames: rescan them from the first '$' after the frame's start.
     * The queue is always a suffix of (the previous frame's raw bytes + b), so it fits. */
    uint8_t q[RAW_MAX + 1];
    int qn = 0, qi = 0;
    q[qn++] = b;
    while (qi < qn) {
        if (parse(d, q[qi++]) || d->raw_n < 0) {
            continue;
        }
        int k = 1;
        while (k < d->raw_n && d->raw[k] != '$') k++;
        uint8_t rest[RAW_MAX + 1];
        int rn = 0;
        for (int i = k; i < d->raw_n; i++) rest[rn++] = d->raw[i];
        for (int i = qi; i < qn; i++) rest[rn++] = q[i];
        memcpy(q, rest, rn);
        qn = rn;
        qi = 0;
        d->raw_n = 0;
    }
}

/* ---- taking frames --------------------------------------------------------------------- */

bool fb_msp_osd_take_frame(fb_msp_osd_t *d, bool *full)
{
    if (!d->frame_pending || d->link_lost) {
        return false;
    }
    bool f = d->full_pending;
    d->nchanged = 0;
    for (int r = 0; r < d->rows; r++) {
        for (int c = 0; c < d->cols; c++) {
            d->changed[r][c] = f ? d->show_ch[r][c] != FB_MSP_OSD_BLANK || d->show_at[r][c]
                                 : d->show_ch[r][c] != d->took_ch[r][c] || d->show_at[r][c] != d->took_at[r][c];
            d->nchanged += d->changed[r][c];
        }
    }
    memcpy(d->took_ch, d->show_ch, sizeof(d->took_ch));
    memcpy(d->took_at, d->show_at, sizeof(d->took_at));
    d->frame_pending = false;
    d->full_pending = false;
    if (full) *full = f;
    return true;
}

static bool in(const fb_msp_osd_t *d, int r, int c)
{
    return r >= 0 && r < d->rows && c >= 0 && c < d->cols;
}

bool fb_msp_osd_changed(const fb_msp_osd_t *d, int r, int c) { return in(d, r, c) && d->changed[r][c]; }
int fb_msp_osd_changed_count(const fb_msp_osd_t *d) { return d->nchanged; }
uint8_t fb_msp_osd_char(const fb_msp_osd_t *d, int r, int c) { return in(d, r, c) ? d->took_ch[r][c] : FB_MSP_OSD_BLANK; }
uint8_t fb_msp_osd_attr(const fb_msp_osd_t *d, int r, int c) { return in(d, r, c) ? d->took_at[r][c] : 0; }

/* ---- the taken frame as JSON cells ---------------------------------------------------------- */

typedef struct { char *buf; size_t cap, n; } out_t;

static void put(out_t *o, const char *s, size_t k)
{
    for (size_t i = 0; i < k; i++, o->n++) {
        if (o->n + 1 < o->cap) o->buf[o->n] = s[i];
    }
}

static void putf(out_t *o, const char *fmt, int a, int b)
{
    char t[48];
    int k = snprintf(t, sizeof t, fmt, a, b);
    put(o, t, (size_t)k);
}

static bool wanted(const fb_msp_osd_t *d, bool full, int r, int c)
{
    return full ? d->took_ch[r][c] != FB_MSP_OSD_BLANK || d->took_at[r][c] : d->changed[r][c];
}

size_t fb_msp_osd_cells_json(const fb_msp_osd_t *d, bool full, char *buf, size_t cap)
{
    out_t o = { buf, cap, 0 };
    bool first = true;
    put(&o, "[", 1);
    for (int r = 0; r < d->rows; r++) {
        for (int c = 0; c < d->cols;) {
            if (!wanted(d, full, r, c)) {
                c++;
                continue;
            }
            uint8_t at = d->took_at[r][c];
            putf(&o, first ? "{\"r\":%d,\"c\":%d,\"s\":\"" : ",{\"r\":%d,\"c\":%d,\"s\":\"", r, c);
            first = false;
            for (; c < d->cols && wanted(d, full, r, c) && d->took_at[r][c] == at; c++) {
                uint8_t ch = d->took_ch[r][c];
                if (ch == '"' || ch == '\\') {
                    char e[2] = { '\\', (char)ch };
                    put(&o, e, 2);
                } else if (ch >= 0x20 && ch < 0x7f) {
                    char e = (char)ch;
                    put(&o, &e, 1);
                } else {
                    putf(&o, "\\u%04x", ch, 0);
                }
            }
            putf(&o, "\",\"attr\":%u}", at, 0);
        }
    }
    put(&o, "]", 1);
    if (cap) buf[o.n < cap ? o.n : cap - 1] = 0;
    return o.n;
}
