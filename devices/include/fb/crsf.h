/* FlightBench device model: CRSF receiver (ExpressLRS / TBS Crossfire) on a UART.
 *
 * Emulator-independent. The receiver is a UART peer of the flight controller:
 *   FC <- RX : RC_CHANNELS_PACKED (16 x 11 bit) at the packet rate, LINK_STATISTICS periodically
 *   FC -> RX : telemetry frames (battery, attitude, flight mode, GPS, ...), kept for the app
 *
 *   inputs    fb_crsf_set_channels_us()   16 channel values in microseconds (988..2012 usable)
 *             fb_crsf_set_link()          RSSI/LQ/SNR the receiver reports, or link lost
 *             fb_crsf_set_rates()         packet rate (Hz) and link-statistics period
 *   time      fb_crsf_tick(dev, dt_ns)    advances the model; frames due are queued for TX
 *             fb_crsf_next_due_ns()       when the next frame is due (one emulator deadline per frame)
 *   UART out  fb_crsf_tx_pop()            bytes the receiver transmits (emulator feeds the USART RX)
 *   UART in   fb_crsf_rx_byte()           byte the FC transmitted (from the USART TX)
 *   telemetry fb_crsf_telemetry_last()    last payload of a given frame type + a receive counter
 *
 * Wire format (crsf_protocol.h): [0xC8][len = 1 + payload + 1][type][payload][crc8 DVB-S2 over
 * type+payload]. Channels use the 11-bit "legacy" scale: us = 1500 + (raw - 992) * 0.62477.
 * Baud is 420000; the emulator sets it on the USART, the model does not care.
 */
#ifndef FB_CRSF_H
#define FB_CRSF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_CRSF_SYNC 0xC8
#define FB_CRSF_CHANNELS 16
#define FB_CRSF_FRAME_MAX 64

/* frame types (subset of crsf_protocol.h) */
enum {
    FB_CRSF_GPS = 0x02, FB_CRSF_VARIO = 0x07, FB_CRSF_BATTERY = 0x08, FB_CRSF_ALTITUDE = 0x09,
    FB_CRSF_HEARTBEAT = 0x0B, FB_CRSF_RPM = 0x0C, FB_CRSF_TEMP = 0x0D, FB_CRSF_LINK_STATISTICS = 0x14,
    FB_CRSF_RC_CHANNELS = 0x16, FB_CRSF_ATTITUDE = 0x1E, FB_CRSF_FLIGHT_MODE = 0x21,
    FB_CRSF_DEVICE_PING = 0x28, FB_CRSF_DEVICE_INFO = 0x29, FB_CRSF_MSP_REQ = 0x7A,
    FB_CRSF_MSP_RESP = 0x7B, FB_CRSF_MSP_WRITE = 0x7C, FB_CRSF_CUSTOM_TELEM = 0x88,
};

typedef struct fb_crsf fb_crsf_t;

fb_crsf_t *fb_crsf_new(void);
void fb_crsf_free(fb_crsf_t *dev);
void fb_crsf_reset(fb_crsf_t *dev);

/* --- inputs --- */
void fb_crsf_set_channels_us(fb_crsf_t *dev, const uint16_t us[FB_CRSF_CHANNELS]);
/* rssi in dBm (negative, e.g. -70), lq 0..100, snr dB; link_up=false stops RC frames (failsafe test)
 * = the "link_lost" fault on, link_up=true = off */
void fb_crsf_set_link(fb_crsf_t *dev, int rssi_dbm, int lq_percent, int snr_db, bool link_up);
void fb_crsf_set_rates(fb_crsf_t *dev, uint32_t packet_hz, uint32_t linkstats_period_ms);

/* --- faults (declared by fb_crsf_desc(), fb/device.h) ---
 *   link_lost  receiver silent: no RC frames, no LINK_STATISTICS (LQ 0 from the FC's view)
 *   freeze     the RC frame at the moment of the fault is repeated at the packet rate while the
 *              link statistics stay live (a hung receiver the FC cannot see by silence)
 *   lq         LINK_STATISTICS report params[0] = LQ %, params[1] = RSSI dBm, params[2] = SNR dB
 *              instead of the set_link values (NaN = keep that one)
 * params: values in the descriptor's param order (may be NULL when the fault has none).
 * Returns false for an unknown fault id. Faults survive fb_crsf_reset(). */
bool fb_crsf_set_fault(fb_crsf_t *dev, const char *fault, bool on, const double *params);
bool fb_crsf_fault_active(const fb_crsf_t *dev, const char *fault);
/* what the receiver reports to the FC right now (set_link values with the faults applied) */
void fb_crsf_link_reported(const fb_crsf_t *dev, int *rssi_dbm, int *lq_percent, int *snr_db, bool *link_up);

/* --- time and bytes --- */
void fb_crsf_tick(fb_crsf_t *dev, uint64_t dt_ns);
int  fb_crsf_tx_pop(fb_crsf_t *dev, uint8_t *buf, int max);   /* returns bytes copied */
int  fb_crsf_tx_pending(const fb_crsf_t *dev);
/* Frame pacing for an emulator that delivers each frame as a burst (one deadline per frame, not per
 * byte): ns of model time until fb_crsf_tick() has something to send (0 = due now; while the link is
 * lost, one packet period: the model is silent but a restored link resumes on that grid) */
uint64_t fb_crsf_next_due_ns(const fb_crsf_t *dev);
/* drops the bytes still queued (the FC did not read them within a frame period; on the wire they
 * would have been lost to overrun); returns how many */
int  fb_crsf_tx_discard(fb_crsf_t *dev);
void fb_crsf_rx_byte(fb_crsf_t *dev, uint8_t b);

/* --- telemetry received from the FC --- */
/* Copies the last payload of `type` (without header/crc) into buf, returns its length or -1 if
 * never seen; *count = how many frames of that type arrived. */
int fb_crsf_telemetry_last(const fb_crsf_t *dev, uint8_t type, uint8_t *buf, int max, uint32_t *count);
uint32_t fb_crsf_rx_frames(const fb_crsf_t *dev);     /* total valid frames from the FC */
uint32_t fb_crsf_rx_crc_errors(const fb_crsf_t *dev);

/* --- helpers --- */
uint8_t  fb_crsf_crc8(const uint8_t *data, int len);           /* DVB-S2 poly 0xD5, init 0 */
uint16_t fb_crsf_us_to_raw(uint16_t us);                       /* legacy 11-bit scale */
uint16_t fb_crsf_raw_to_us(uint16_t raw);
/* Build one frame into out (returns total length): [sync][len][type][payload][crc] */
int fb_crsf_build_frame(uint8_t *out, uint8_t type, const uint8_t *payload, int len);
/* Decoders for common telemetry payloads (return false if length wrong) */
bool fb_crsf_decode_battery(const uint8_t *p, int len, float *volts, float *amps, uint32_t *mah, uint8_t *remaining_pct);
bool fb_crsf_decode_attitude(const uint8_t *p, int len, float *pitch_deg, float *roll_deg, float *yaw_deg);

#ifdef __cplusplus
}
#endif
#endif
