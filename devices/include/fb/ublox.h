/* FlightBench device model: u-blox GNSS receiver (M10 or M8 class) speaking UBX on a UART, for
 * iNav 9.1.0's io/gps_ublox.c (the GPS port of the SpeedyBee F405 WING is USART3).
 *
 * Emulator-independent. The receiver is a UART peer of the flight controller:
 *   UART in   fb_ublox_rx_byte()       byte the FC transmitted (from the USART TX)
 *   UART out  fb_ublox_tx_pop()        bytes the receiver transmits (feed the USART RX)
 *   baud      fb_ublox_set_fc_baud()   the baud the FC's USART runs at now (0 = unknown: every
 *                                      byte gets through). While it differs from the receiver's
 *                                      own baud (fb_ublox_baud(), 3 % tolerance) no byte gets
 *                                      through either way (a real line would carry garbage;
 *                                      bytes queued before the change are dropped at pop)
 *   time      fb_ublox_tick(dev, dt_ns)  navigation epochs; fb_ublox_next_due_ns() = until the
 *                                      next one (0 while bytes wait: one emulator deadline per
 *                                      burst); fb_ublox_byte_ns() = one byte on the line, for
 *                                      an emulator that paces the burst
 *   motion    fb_ublox_set_home() / fb_ublox_set_motion(): FDM position/velocity in NED metres
 *                                      around home (the env's home: lat/lon/alt MSL)
 *
 * What iNav does and how the model answers (every message, both paths):
 *  1. autobaud: at each of 115200, 57600, 38400, 19200, 9600, 230400 (up to
 *     gps_auto_baud_max_supported) the FC sends NMEA "$PUBX,41,1,0003,0001,<baud>,0*CS" (not
 *     CFG-PRT), <baud> always being the port's configured GPS baud (default 115200), not the
 *     baud it is probing. The receiver hears the one sent at its own baud (factory 9600) and
 *     switches UART1 to <baud>, input UBX+NMEA, output UBX only. No answer.
 *  2. MON-VER poll (0A 04, empty) -> MON-VER: swVersion, hwVersion "000A0000" (M10) or
 *     "00080000" (M8), extensions "FWVER=SPG 5.10", "PROTVER=34.10" / "FWVER=SPG 3.01",
 *     "PROTVER=18.00", "GPS;GLO;GAL;BDS". PROTVER > 23.01 selects iNav's M9+/VALSET path.
 *  3. MON-GNSS poll (0A 28, empty) -> MON-GNSS v0 (supported GPS|GLO|BDS|GAL, default, enabled,
 *     max concurrent 4 / 3). Polled again every 5 s by iNav.
 *  4. M10 path, each answered by ACK-ACK (05 01) once applied:
 *       CFG-VALSET (06 8A) NAVSPG_DYNMODEL + FIXMODE; CFG-VALSET MSGOUT NMEA GGA/GLL/GSA/RMC/VTG
 *       UART1 = 0; CFG-VALSET MSGOUT UART1 NAV-POSLLH/STATUS/VELNED/TIMEUTC/SAT = 0, NAV-PVT = 1,
 *       NAV-SIG = 1; CFG-VALSET RATE_MEAS (U2, ms) + RATE_NAV; CFG-VALSET SIGNAL_* (SBAS, GAL,
 *       BDS, BDS B1C, QZSS, GLO) = the enabled constellations.
 *     CFG-SBAS (06 16), a legacy message iNav also sends to an M10: ACK-NAK (iNav takes either).
 *     Output per epoch: NAV-PVT (01 07) and NAV-SIG (01 43).
 *  5. M8 path, each answered by ACK-ACK: CFG-NAV5 (06 24), CFG-MSG (06 01) NMEA F0 00..05 = 0,
 *     CFG-MSG NAV POSLLH/STATUS/SOL/VELNED/TIMEUTC/SVINFO = 0, NAV-PVT = 1, NAV-SAT = 1,
 *     CFG-RATE (06 08), CFG-SBAS (06 16), CFG-GNSS (06 3E).
 *     Output per epoch: NAV-PVT and NAV-SAT (01 35).
 *  Any message enabled by CFG-MSG/VALSET among NAV-POSLLH, -STATUS, -SOL (M8), -PVT, -VELNED,
 *  -TIMEUTC, -SAT, -SIG (M10) is produced at its rate (every n-th solution); NAV-SVINFO is
 *  accepted and never produced. Unknown CFG messages: ACK-NAK; a CFG-RATE/RATE_MEAS below 25 ms or
 *  navRate 0: ACK-NAK. Frames with a bad checksum are ignored. No NMEA output is modelled.
 *
 * Solution: lat/lon on WGS-84 (N/E metres -> arc on the meridian / prime-vertical radii at home),
 * hMSL = home alt - down, ellipsoid height = hMSL (no geoid model). Ground speed, course of motion
 * (kept below 0.05 m/s), vertical speed, 3D fix with gnssFixOK once fix_delay_s after power-on
 * (numSV ramps 0..3 before), numSV = sats (at most the enabled systems' synthetic satellites), pDOP, hAcc/vAcc, UTC and GPS time of week from the
 * power-on epoch + virtual time (18 leap seconds), date/time valid from the fix. Position noise:
 * first-order Gauss-Markov per axis (sigma noise_m, 5 s correlation), velocity noise 0.1 x noise_m
 * m/s white; deterministic PRNG from the seed. A UART that cannot carry an epoch's messages at the
 * current baud loses the messages that do not fit (counted).
 *
 * Faults (fb_ublox_set_fault(); survive fb_ublox_reset()):
 *   no_fix   fix type 0, gnssFixOK clear, 2 satellites; messages keep coming
 *   spoof    params north_m, east_m, alt_m: the solution is offset by that much
 *   drift    params rate_m: the solution walks away, random walk of rate_m per sqrt(s) per axis;
 *            off -> back at once
 *   silence  the receiver sends nothing and hears nothing
 *   glitch   params jump_m: the next epoch's solution is jump_m north of the truth (0 = its first
 *            message goes out with a broken checksum instead); one-shot, then off by itself
 */
#ifndef FB_UBLOX_H
#define FB_UBLOX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FB_UBLOX_M10 = 0, FB_UBLOX_M8 = 1 } fb_ublox_gen_t;

/* the descriptor's props */
typedef struct {
    fb_ublox_gen_t generation;  /* m10 */
    uint32_t baud;              /* power-on UART baud, 9600 (u-blox factory default) */
    uint32_t rate_hz;           /* power-on navigation rate, 10 (the FC's CFG-RATE overrides it) */
    double fix_delay_s;         /* 3D fix this long after power-on, 20 */
    double noise_m;             /* position noise sigma, 0.3 */
    int sats;                   /* satellites used with a fix, 16 */
    double pdop;                /* 1.2 */
    double h_acc_m;             /* reported horizontal accuracy (vAcc = 1.6 x), 1.0 */
    uint64_t seed;              /* PRNG seed, 1 */
    int64_t epoch_unix;         /* UTC at power-on, Unix seconds (2026-09-29 00:00:00) */
} fb_ublox_config_t;

typedef struct {
    double lat_deg, lon_deg, alt_msl_m;
    double vel_ned[3];
    double ground_speed_mps, course_deg;
    int fix_type;               /* 0 none, 3 = 3D */
    int num_sv;
    bool time_valid;
    uint32_t itow_ms;
    uint64_t epochs;            /* navigation solutions computed */
} fb_ublox_solution_t;

typedef struct {
    uint32_t rx_frames;         /* UBX frames from the FC with a good checksum */
    uint32_t rx_bad_checksum;
    uint32_t rx_pubx;           /* PUBX,41 applied */
    uint32_t rx_lost;           /* bytes lost to a baud mismatch */
    uint32_t acks, naks;
    uint32_t tx_frames;
    uint32_t tx_lost;           /* frames lost: baud mismatch, UART capacity, queue full */
} fb_ublox_stats_t;

typedef struct fb_ublox fb_ublox_t;

void fb_ublox_default_config(fb_ublox_config_t *cfg);
fb_ublox_t *fb_ublox_new(const fb_ublox_config_t *cfg);     /* NULL = defaults */
void fb_ublox_free(fb_ublox_t *dev);
void fb_ublox_reset(fb_ublox_t *dev);    /* power cycle: factory baud and messages, fix lost */

void fb_ublox_set_home(fb_ublox_t *dev, double lat_deg, double lon_deg, double alt_msl_m);
void fb_ublox_set_motion(fb_ublox_t *dev, const double pos_ned_m[3], const double vel_ned_mps[3]);

void     fb_ublox_set_fc_baud(fb_ublox_t *dev, uint32_t baud);
uint32_t fb_ublox_baud(const fb_ublox_t *dev);
uint64_t fb_ublox_byte_ns(const fb_ublox_t *dev);
void     fb_ublox_rx_byte(fb_ublox_t *dev, uint8_t b);
void     fb_ublox_tick(fb_ublox_t *dev, uint64_t dt_ns);
uint64_t fb_ublox_next_due_ns(const fb_ublox_t *dev);
int      fb_ublox_tx_pop(fb_ublox_t *dev, uint8_t *buf, int max);
int      fb_ublox_tx_pending(const fb_ublox_t *dev);

bool fb_ublox_set_fault(fb_ublox_t *dev, const char *fault, bool on, const double *params);
bool fb_ublox_fault_active(const fb_ublox_t *dev, const char *fault);

/* what the receiver is doing */
const fb_ublox_solution_t *fb_ublox_solution(const fb_ublox_t *dev);   /* last epoch's (faults applied) */
void     fb_ublox_stats(const fb_ublox_t *dev, fb_ublox_stats_t *st);
uint32_t fb_ublox_nav_period_ms(const fb_ublox_t *dev);
int      fb_ublox_msg_rate(const fb_ublox_t *dev, uint8_t cls, uint8_t id);   /* -1 = not a periodic message */
int      fb_ublox_dyn_model(const fb_ublox_t *dev);
uint8_t  fb_ublox_gnss_enabled(const fb_ublox_t *dev);    /* MON-GNSS bits: GPS 1, GLO 2, BDS 4, GAL 8 */

/* UBX helpers */
void fb_ubx_checksum(const uint8_t *data, int len, uint8_t *ck_a, uint8_t *ck_b);   /* over class..payload */
int  fb_ubx_frame(uint8_t *out, uint8_t cls, uint8_t id, const uint8_t *payload, int len);   /* len + 8 */
/* NED metres around home <-> WGS-84 (as the model converts). Longitude is wrapped into
 * [-180, 180) (a home near the antimeridian); the local-tangent conversion degenerates at the poles
 * (|home lat| > ~89 deg gives meaningless longitudes, never NaN). */
void fb_ubx_ned_to_llh(double lat0_deg, double lon0_deg, double alt0_m, const double ned[3],
                       double *lat_deg, double *lon_deg, double *alt_m);
void fb_ubx_llh_to_ned(double lat0_deg, double lon0_deg, double alt0_m, double lat_deg, double lon_deg,
                       double alt_m, double ned[3]);

#ifdef __cplusplus
}
#endif
#endif
