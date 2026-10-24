/* FlightBench device model: an HD video transmitter's OSD side speaking MSP DisplayPort on a UART
 * (HDZero / DJI / Walksnail class: the flight controller sends the characters, the VTX renders).
 *
 * Emulator-independent. The device is a UART peer of the flight controller:
 *   FC -> device : MSP v1 frames. MSP_DISPLAYPORT (182) pushes ($M> "reply" direction, what
 *                  RotorFlight's io/displayport_msp.c sends), and the replies to the device's own
 *                  requests. Other frames are counted and ignored.
 *   device -> FC : MSP v1 requests ($M<): MSP_SET_OSD_CANVAS (188) with the device's canvas
 *                  (cols, rows), once per contact (first frame after start, after silence or
 *                  after fb_msp_osd_contact_lost()); MSP_FC_VARIANT (2) every (fb_msp_osd_set_poll())
 *                  FB_MSP_OSD_POLL_NS from power-on, as a VTX keeps talking to the FC. iNav
 *                  pushes DisplayPort frames only while the VTX does (io/displayport_msp_osd.c:
 *                  output() needs vtxActive, set by any MSP request on the port and cleared
 *                  after 1 s without one; "VTX sends an MSP command every 125ms or so");
 *                  RotorFlight pushes regardless and answers the requests. The reply's 4
 *                  characters are the FC variant ("INAV", "RTFL"): fb_msp_osd_fc_variant(),
 *                  which font the firmware's character codes index.
 * Parser: MSP v1 with the jumbo form (size byte 255, then the command, then a 16-bit size; frames
 * longer than 256 bytes are read and dropped, counted `oversize`). After a bad checksum the bytes
 * read as that frame are rescanned from the first '$' after its start, so a frame cut short (the FC
 * reset mid-frame) does not swallow the next one.
 * Contact: lost after silence longer than fb_msp_osd_silence_ns() = max(250 ms, 3 x the frame
 * interval), or when the emulator reports an MCU reset (fb_msp_osd_contact_lost()). Frame interval
 * = the longest of the last 8 DRAW_SCREEN gaps of at least 16 ms (shorter ones are extra DRAWs:
 * RotorFlight's fastest osd_framerate_hz is 60); until one is measured 1 s (its slowest, 1 Hz).
 * An MCU reset forgets the interval (the new boot may run another rate); after a silence the gap
 * to the next DRAW still counts, so a firmware slower than the threshold costs one contact.
 *
 *   UART in   fb_msp_osd_rx_byte()      byte the FC transmitted (from the USART TX)
 *   UART out  fb_msp_osd_tx_pop()       bytes the device transmits (emulator feeds the USART RX)
 *   time      fb_msp_osd_tick(dt_ns)    advances the model's clock (silence detection)
 *   frames    fb_msp_osd_take_frame()   true once per DRAW_SCREEN since the last call; the cells
 *                                       that differ from what was last taken are marked changed
 *
 * MSP_DISPLAYPORT sub-commands (payload[0], io/displayport_msp.h):
 *   0 HEARTBEAT     keeps the display grabbed                 (counted)
 *   1 RELEASE       the FC gives the display up               (canvas blanked, next frame full)
 *   2 CLEAR_SCREEN  blank the drawing buffer                  (every OSD frame starts with it)
 *   3 WRITE_STRING  [row][col][attr][chars...] into the drawing buffer; clipped at the canvas
 *   4 DRAW_SCREEN   show the drawing buffer                   (= one frame)
 *   5 OPTIONS       [font][mode]: iNav sends it with every clear (io/displayport_msp_osd.c
 *                   setDisplayMode: font 0, mode 0 SD 30x16 / 1 HD 50x18 / 2 60x22 / 3 53x20);
 *                   counted and kept (fb_msp_osd_options()), the canvas stays the device's
 *   6 SYS           [row][col][element]: a VTX/goggle-rendered element (voltage, bitrate, ...);
 *                   counted, not rendered (nothing here knows those values)
 * Double buffering: WRITE_STRING/CLEAR act on the drawing buffer, DRAW_SCREEN copies it to the
 * shown canvas. The drawing buffer is kept across DRAWs (writes without a CLEAR accumulate).
 *
 * attr byte of WRITE_STRING, exactly as RotorFlight 4.6 builds it (displayport_msp.c writeString):
 *   bits 0..5  font page = displayport_msp_attrs[severity] (CLI, 4 values for NONE / INFO /
 *              WARNING / CRITICAL; all 0 by default, so every cell has attr 0 unless configured)
 *   bit 6      blink (DISPLAYPORT_MSP_ATTR_BLINK): set whenever the element is blinking,
 *              whatever displayport_msp_use_device_blink says; with it OFF (default) the firmware
 *              also blinks the element itself (left out every other 250 ms phase), so the cells
 *              come and go and carry 0x40 while shown. For a blinking cell bits 0..5 come from
 *              past the attr array (attrValues[0x80 + severity]): 0 on this build
 *   bit 7      version, 0 for DisplayPort v1/v2 (DISPLAYPORT_MSP_ATTR_VERSION)
 * iNav 9.1.0 (io/displayport_msp_osd.c drawScreen, displayport_msp_osd.h) uses the same bit layout
 * with a 2-bit page (DISPLAYPORT_MSP_ATTR_FONTPAGE_MASK 0x3): its characters are 9-bit+ codes (the
 * font has 512 glyphs, getFontMetadata says 1024), the page carries bits 8..9, so a glyph is
 * (attr & 3) << 8 | byte. Blink is never set by iNav: the MSP OSD reports no text attributes, so
 * drivers/display.c blinks in software (blanks the element every other phase). iNav sends only the
 * changed cells (dirty runs) and a CLEAR + all non-blank cells once per
 * osd_msp_displayport_fullframe_interval (1 s default); no CLEAR per frame.
 * The model keeps the byte as received.
 */
#ifndef FB_MSP_OSD_H
#define FB_MSP_OSD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_MSP_OSD_ROWS_MAX 32
#define FB_MSP_OSD_COLS_MAX 64
#define FB_MSP_OSD_BLANK 0x20

#define FB_MSP_FC_VARIANT 2
#define FB_MSP_DISPLAYPORT 182
#define FB_MSP_SET_OSD_CANVAS 188

enum {
    FB_MSP_DP_HEARTBEAT = 0, FB_MSP_DP_RELEASE = 1, FB_MSP_DP_CLEAR_SCREEN = 2,
    FB_MSP_DP_WRITE_STRING = 3, FB_MSP_DP_DRAW_SCREEN = 4, FB_MSP_DP_OPTIONS = 5, FB_MSP_DP_SYS = 6,
};

#define FB_MSP_OSD_ATTR_PAGE    0x3F    /* font page (displayport_msp_attrs[severity]) */
#define FB_MSP_OSD_ATTR_BLINK   0x40
#define FB_MSP_OSD_ATTR_VERSION 0x80

/* shortest silence after which the next frame is a new contact: canvas announced again, the next
 * frame taken is full (the FC rebooted or re-initialised its display); the actual threshold is
 * fb_msp_osd_silence_ns() */
#define FB_MSP_OSD_SILENCE_NS 250000000ull
/* the device's MSP_FC_VARIANT request interval (iNav's comment: "every 125ms or so"; its timeout 1 s) */
#define FB_MSP_OSD_POLL_NS 125000000ull

typedef struct fb_msp_osd fb_msp_osd_t;

typedef struct {
    uint32_t frames;        /* DRAW_SCREEN */
    uint32_t writes;        /* WRITE_STRING */
    uint32_t clears;        /* CLEAR_SCREEN */
    uint32_t heartbeats, releases, sys, options, unknown_sub;
    uint32_t clipped;       /* characters written outside the canvas */
    uint32_t other_frames;  /* valid MSP frames that are not DisplayPort (e.g. replies) */
    uint32_t canvas_acks;   /* the FC's replies to MSP_SET_OSD_CANVAS */
    uint32_t canvas_sent;   /* MSP_SET_OSD_CANVAS requests queued */
    uint32_t bad_checksum;
    uint32_t contacts;      /* first frame after start / silence / contact_lost() */
    uint32_t jumbo;         /* jumbo frames (size byte 255) */
    uint32_t oversize;      /* jumbo frames longer than 256 bytes, dropped */
    uint32_t polls;         /* MSP_FC_VARIANT requests queued */
    uint32_t variants;      /* the FC's MSP_FC_VARIANT replies */
    uint32_t errors;        /* the FC's error replies ($M!, e.g. iNav to MSP_SET_OSD_CANVAS) */
} fb_msp_osd_stats_t;

/* rows x cols: the device's canvas (HDZero: 18 x 50), announced with MSP_SET_OSD_CANVAS */
fb_msp_osd_t *fb_msp_osd_new(int rows, int cols);
void fb_msp_osd_free(fb_msp_osd_t *dev);
/* power-on: blank canvas, parser idle, no contact; faults are kept */
void fb_msp_osd_reset(fb_msp_osd_t *dev);
int fb_msp_osd_rows(const fb_msp_osd_t *dev);
int fb_msp_osd_cols(const fb_msp_osd_t *dev);

/* --- faults (fb_msp_osd_desc()) ---
 *   link_lost  the device is silent: it ignores what the FC sends (no frames) and sends nothing;
 *              when turned off the next frame is full */
bool fb_msp_osd_set_fault(fb_msp_osd_t *dev, const char *fault, bool on, const double *params);
bool fb_msp_osd_fault_active(const fb_msp_osd_t *dev, const char *fault);

/* --- time and bytes --- */
void fb_msp_osd_tick(fb_msp_osd_t *dev, uint64_t dt_ns);
/* the FC's MCU was reset (the VTX keeps power): the next valid frame is a new contact (canvas
 * announced, next frame full), not a continuation after a pause */
void fb_msp_osd_contact_lost(fb_msp_osd_t *dev);
/* silence that ends the contact: max(FB_MSP_OSD_SILENCE_NS, 3 x the measured DRAW interval) */
uint64_t fb_msp_osd_silence_ns(const fb_msp_osd_t *dev);
/* the frame interval: the longest of the last 8 DRAW-to-DRAW gaps >= 16 ms (each capped at 1 s),
 * 0 = none measured since power-on / the last contact_lost() */
uint64_t fb_msp_osd_draw_interval_ns(const fb_msp_osd_t *dev);
/* the MSP_FC_VARIANT requests on/off (default on; kept across reset). iNav needs them; RotorFlight
 * pushes without them, so a RotorFlight board's machine turns them off (its benches stay as they were) */
void fb_msp_osd_set_poll(fb_msp_osd_t *dev, bool on);
/* virtual ns until the next MSP_FC_VARIANT request is queued (by fb_msp_osd_tick()); UINT64_MAX when off */
uint64_t fb_msp_osd_poll_in_ns(const fb_msp_osd_t *dev);
void fb_msp_osd_rx_byte(fb_msp_osd_t *dev, uint8_t b);
int  fb_msp_osd_tx_pop(fb_msp_osd_t *dev, uint8_t *buf, int max);
int  fb_msp_osd_tx_pending(const fb_msp_osd_t *dev);

/* --- the shown canvas --- */
/* true once per DRAW_SCREEN (several DRAWs since the last call count once). *full = the taker
 * must wipe its copy and take every cell (first frame of a contact, after RELEASE, after the
 * link_lost fault); fb_msp_osd_changed() then marks the cells that differ from the canvas at the
 * previous take (all non-blank ones when full). */
bool fb_msp_osd_take_frame(fb_msp_osd_t *dev, bool *full);
bool fb_msp_osd_changed(const fb_msp_osd_t *dev, int row, int col);
/* how many cells fb_msp_osd_changed() marks in the last taken frame */
int fb_msp_osd_changed_count(const fb_msp_osd_t *dev);
uint8_t fb_msp_osd_char(const fb_msp_osd_t *dev, int row, int col);
uint8_t fb_msp_osd_attr(const fb_msp_osd_t *dev, int row, int col);
/* the FC is sending (a valid frame within fb_msp_osd_silence_ns()) and link_lost is off */
bool fb_msp_osd_link_up(const fb_msp_osd_t *dev);
/* the cells of the last taken frame as the JSON array of the control protocol's `osd` message
 * (docs/protocol.md): full = every non-blank cell (or attr != 0), else the changed cells; runs of
 * consecutive cells of one row with one attr: {"r":R,"c":C,"s":"...","attr":A}. `s`: one character
 * per cell, the byte as code point U+0000..U+00FF: printable ASCII as is (`"` and backslash
 * escaped with a backslash), the rest as JSON u00NN escapes. snprintf-like: writes at most cap-1 characters + NUL, returns the full length. */
size_t fb_msp_osd_cells_json(const fb_msp_osd_t *dev, bool full, char *buf, size_t cap);
const fb_msp_osd_stats_t *fb_msp_osd_stats(const fb_msp_osd_t *dev);
/* the FC variant of the last MSP_FC_VARIANT reply ("INAV", "RTFL", ...), "" until one arrived since
 * power-on / the last contact_lost() (the MCU reset may have brought other firmware) */
const char *fb_msp_osd_fc_variant(const fb_msp_osd_t *dev);
/* the font family the FC's character codes index: "inav" (512+ glyphs, the attr page = code bits
 * 8..9), "rotorflight" (Betaflight layout, 256 glyphs), NULL = variant unknown or another one */
const char *fb_msp_osd_font(const fb_msp_osd_t *dev);
/* the last OPTIONS sub-command: font and mode bytes; false = none since power-on / contact_lost() */
bool fb_msp_osd_options(const fb_msp_osd_t *dev, int *font, int *mode);

/* MSP v1 frame builder: $M<dir> len cmd payload xor; returns the length (6 + len) */
int fb_msp_v1_frame(uint8_t *out, char dir, uint8_t cmd, const uint8_t *payload, int len);

#ifdef __cplusplus
}
#endif
#endif
