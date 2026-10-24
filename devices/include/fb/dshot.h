/* FlightBench: DShot frame codec for an emulated timer/DMA path, and the bidirectional-DShot
 * eRPM reply (GCR) an ESC sends back. Pure functions, no state.
 *
 * Firmware side (drivers/dshot.c, dshot_dpwm.c): a 16-bit packet = 11-bit value, telemetry bit,
 * 4-bit checksum (xor of nibbles; inverted when bidirectional). It is written into the timer DMA
 * buffer as 16 compare values, MSB first: bit 1 = 14/20 of the bit period, bit 0 = 7/20, followed
 * by two zeros. With bidirectional DShot the output is inverted electrically but the compare
 * values are the same, so the decoder below works on compare values regardless of polarity.
 *
 * ESC reply (pwm_output_dshot_shared.c decodeTelemetryPacket): 12-bit eRPM period (3-bit
 * exponent, 9-bit mantissa, microseconds) + 4-bit checksum, GCR-coded to 20 bits, sent as
 * 21 bits (leading 1) at 5/4 of the DShot bit rate. The firmware captures edge timestamps; an edge
 * at bit position j means stream bit j = 1.
 */
#ifndef FB_DSHOT_H
#define FB_DSHOT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_DSHOT_MIN_THROTTLE 48
#define FB_DSHOT_MAX_THROTTLE 2047
#define FB_DSHOT_BIT_0_NUM 7     /* compare value for a 0 bit, in 1/20 of the period */
#define FB_DSHOT_BIT_1_NUM 14
#define FB_DSHOT_BIT_DEN 20

/* Build the 16-bit packet exactly like prepareDshotPacket(). */
uint16_t fb_dshot_encode(uint16_t value, bool request_telemetry, bool bidir);

/* Decode a packet; checks the checksum for the given mode. Returns false on bad checksum. */
bool fb_dshot_decode_packet(uint16_t packet, bool bidir, uint16_t *value, bool *request_telemetry);

/* Decode from 16 timer compare values (the DMA buffer) with the timer period (ARR + 1).
 * A compare value above period/2 is a 1. `bidir` selects the checksum polarity. */
bool fb_dshot_decode_ccr(const uint32_t ccr[16], uint32_t period, bool bidir, uint16_t *value, bool *request_telemetry);

/* Throttle value -> 0..1 (48..2047), 0 for the stop/command range (0..47). */
float fb_dshot_value_to_throttle(uint16_t value);

/* --- bidirectional DShot eRPM reply --- */

/* eRPM -> 16-bit telemetry value (12-bit period code + checksum). erpm 0 -> the "0x0fff" idle code. */
uint16_t fb_dshot_erpm_to_value16(uint32_t erpm);

/* 16-bit value -> 21-bit edge stream (bit 20 = leading 1, then 20 GCR bits). */
uint32_t fb_dshot_value16_to_stream21(uint16_t v16);

/* Expand a 21-bit stream into edge timestamps: edges[k] = t0 + j * bit_ticks for each set bit j
 * (j = 0 is the MSB). Returns the number of edges written (<= max, at most 21). */
int fb_dshot_stream21_to_edges(uint32_t stream21, uint32_t t0, uint32_t bit_ticks, uint32_t *edges, int max);

/* Firmware-side decoder, ported verbatim from decodeTelemetryPacket() for tests and for an
 * emulator that wants to check its own edges. buffer[] are edge timestamps with 16 ticks per bit.
 * Returns eRPM/100, 0 for the idle code, 0xffff on error. */
uint32_t fb_dshot_decode_edges_fw(const uint32_t *buffer, uint32_t count);

#ifdef __cplusplus
}
#endif
#endif
