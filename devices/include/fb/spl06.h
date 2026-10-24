/* FlightBench device model: Goertek SPL06-001 barometer (I2C, address 0x76/0x77), the chip on
 * the SpeedyBee F405 WING's I2C1. A device of its own (id "spl06"), not an alias of dps310: the
 * two share the result/config register layout and the coefficient packing (src/baro_common.c),
 * but the SPL06 has no COEF_SRCE register (0x28), uses all nine coefficients and is detected by
 * iNav's drivers/barometer/barometer_spl06.c (tried before barometer_dps310.c in iNav's AUTO
 * probe order, so ID 0x10 is claimed by the SPL06 driver).
 *
 * Emulator-independent. The emulator's I2C controller calls, per transaction:
 *   fb_spl06_i2c_start(dev, read)      address phase; false = NACK (fault "detached")
 *   fb_spl06_i2c_write_byte(dev, b)    first byte after a write start = register pointer, then
 *                                      data with auto-increment; false = NACK
 *   fb_spl06_i2c_read_byte(dev)        byte at the pointer, pointer + 1
 * or the convenience forms fb_spl06_i2c_write(dev, bytes, n) / fb_spl06_i2c_read(dev, buf, n)
 * (they include the start).
 *
 * Time: the chip has its own clock in virtual time. The emulator advances it with
 * fb_spl06_tick(dev, dt_ns) (before every I2C transaction, and when fb_spl06_next_due_ns() says a
 * measurement completes: one timer suffices). What follows virtual time:
 *   - after power-on / fb_spl06_reset() / SOFT_RST: SENSOR_RDY at 12 ms, COEF_RDY at 40 ms
 *     (the DPS310 datasheet's start-up times; iNav waits 100 ms before its first read);
 *   - command mode (MEAS_CTRL 1 = one pressure, 2 = one temperature measurement): the result
 *     registers and PRS_RDY/TMP_RDY are latched after the conversion time of the programmed
 *     oversampling (3.6 / 5.2 / 8.4 / 14.8 / 27.6 / 53.2 / 104.4 / 206.8 ms for 1x..128x), then
 *     MEAS_CTRL returns to 0 (standby); reading before that returns the previous result;
 *   - background mode (MEAS_CTRL 5/6/7): pressure and/or temperature measured at PM_RATE /
 *     TMP_RATE (1..128 Hz), each result latched one conversion time after its measurement starts;
 *   - PRS_RDY is cleared by reading PSR_B0, TMP_RDY by reading TMP_B0; INT_STS (INT_PRS/INT_TMP,
 *     set per completed measurement when enabled in CFG_REG) is cleared by reading it.
 * The chip measures what fb_spl06_set_environment() said last when a measurement completes
 * (call it before fb_spl06_tick(); feed it from include/fb/atmosphere.h so an spl06 and a dps310
 * agree on the same altitude).
 *
 * Coefficients: a fixed synthetic set of the magnitude of real parts, with every term non-zero
 * (FB_SPL06_C*); the model inverts the full compensation formula (datasheet / iNav's
 * spl06_compensate_pressure(): Pcomp = c00 + Praw_sc (c10 + Praw_sc (c20 + Praw_sc c30))
 * + Traw_sc (c01 + Praw_sc (c11 + Praw_sc c21)), Tcomp = c0/2 + c1 Traw_sc) by Newton iteration,
 * so the driver computes the environment's pressure within its integer-Pa rounding and the
 * temperature within 0.01 degC at every oversampling.
 *
 * Noise: Gaussian pressure noise per PM_PRC (the DPS310 datasheet's precision table: the SPL06
 * figures were not at hand), xorshift PRNG with a settable seed; scale 0 = exact values.
 *
 * Faults (fb_spl06_set_fault(); kept across SOFT_RST and fb_spl06_reset()):
 *   freeze     result registers stop updating (RDY no longer set; a pending measurement never
 *              completes); the chip still answers on I2C
 *   offset     params[0] = offset_pa: added to the measured pressure (a step when turned on)
 *   detached   the chip is gone from the bus: the address phase NACKs, nothing reaches it
 *
 * Not modelled: FIFO (FIFO_EN is stored, FIFO_STS reads FIFO_EMPTY), the interrupt pin, SPI mode,
 * the effect of a missing P_SHIFT/T_SHIFT at > 8x oversampling (results read as if shifted),
 * temperature noise.
 */
#ifndef FB_SPL06_H
#define FB_SPL06_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_SPL06_I2C_ADDR   0x76
#define FB_SPL06_PRODUCT_ID 0x10   /* PROD_ID 1, REV_ID 0 */

/* synthetic calibration coefficients (12 / 20 / 16 bit two's complement fields) */
#define FB_SPL06_C0   204
#define FB_SPL06_C1   (-261)
#define FB_SPL06_C00  80469
#define FB_SPL06_C10  (-54769)
#define FB_SPL06_C01  (-2803)
#define FB_SPL06_C11  1226
#define FB_SPL06_C20  (-10787)
#define FB_SPL06_C21  229
#define FB_SPL06_C30  (-1780)

/* registers */
enum {
    FB_SPL06_PSR_B2 = 0x00, FB_SPL06_TMP_B2 = 0x03, FB_SPL06_PRS_CFG = 0x06, FB_SPL06_TMP_CFG = 0x07,
    FB_SPL06_MEAS_CFG = 0x08, FB_SPL06_CFG_REG = 0x09, FB_SPL06_INT_STS = 0x0A,
    FB_SPL06_FIFO_STS = 0x0B, FB_SPL06_RESET = 0x0C, FB_SPL06_ID = 0x0D, FB_SPL06_COEF = 0x10,
    FB_SPL06_NREGS = 0x22,
};
#define FB_SPL06_COEF_RDY   0x80
#define FB_SPL06_SENSOR_RDY 0x40
#define FB_SPL06_TMP_RDY    0x20
#define FB_SPL06_PRS_RDY    0x10

typedef struct fb_spl06 fb_spl06_t;

fb_spl06_t *fb_spl06_new(void);
void fb_spl06_free(fb_spl06_t *dev);
void fb_spl06_reset(fb_spl06_t *dev);          /* power cycle: registers, clock and start-up times */

/* I2C slave side */
bool    fb_spl06_i2c_start(fb_spl06_t *dev, bool read);
bool    fb_spl06_i2c_write_byte(fb_spl06_t *dev, uint8_t b);
uint8_t fb_spl06_i2c_read_byte(fb_spl06_t *dev);
bool    fb_spl06_i2c_write(fb_spl06_t *dev, const uint8_t *bytes, int n);
bool    fb_spl06_i2c_read(fb_spl06_t *dev, uint8_t *buf, int n);

/* physics and time */
void     fb_spl06_set_environment(fb_spl06_t *dev, double pressure_pa, double temp_c);
void     fb_spl06_tick(fb_spl06_t *dev, uint64_t dt_ns);
uint64_t fb_spl06_next_due_ns(const fb_spl06_t *dev);   /* until the next completion; UINT64_MAX = none */
uint64_t fb_spl06_samples(const fb_spl06_t *dev);       /* completed measurements (P and T) */

/* noise: scale 1 = datasheet-like precision (default), 0 = exact; seed for the PRNG */
void  fb_spl06_set_noise(fb_spl06_t *dev, float scale, uint64_t seed);
float fb_spl06_noise_pa(const fb_spl06_t *dev);          /* RMS at the current PM_PRC, scaled */

/* faults of the descriptor; false for an unknown fault id */
bool fb_spl06_set_fault(fb_spl06_t *dev, const char *fault, bool on, const double *params);
bool fb_spl06_fault_active(const fb_spl06_t *dev, const char *fault);

/* test/debug: what iNav's driver computes from the current result registers (its float formula) */
float   fb_spl06_expected_pressure_pa(const fb_spl06_t *dev);
float   fb_spl06_expected_temp_c(const fb_spl06_t *dev);
uint8_t fb_spl06_peek(const fb_spl06_t *dev, uint8_t reg);

#ifdef __cplusplus
}
#endif
#endif
