/* FlightBench device model: TDK InvenSense ICM-42688-P (6-axis IMU) on SPI.
 *
 * Emulator-independent: the emulator owns the SPI controller, the chip-select GPIO, the INT1 line
 * and the clock; this model only knows bytes, registers and physical inputs.
 *
 *   SPI          fb_icm42688p_cs()        chip select asserted / released (frames a transaction)
 *                fb_icm42688p_xfer()      full-duplex byte exchange, returns the MISO byte
 *   physics      fb_icm42688p_set_motion() latest true angular rate / specific force / temperature
 *   time         fb_icm42688p_odr_hz()    current output data rate (what the firmware programmed)
 *                fb_icm42688p_sample()    called by the emulator once per 1/ODR: latches data,
 *                                         returns true when INT1 should pulse (data ready)
 *
 * Register behaviour follows the datasheet (DS-000347) as far as the RotorFlight/Betaflight driver
 * (accgyro_spi_icm426xx.c) exercises it: bank select, soft reset, WHO_AM_I, PWR_MGMT0,
 * GYRO/ACCEL_CONFIG0 (FSR + ODR), INT_CONFIG*, INT_SOURCE0, INT_STATUS (DATA_RDY, clear on read),
 * TEMP/ACCEL/GYRO data registers (big-endian int16). FIFO, APEX and self-test are not modelled.
 *
 * Axes: inputs are in the chip's own frame (X, Y, Z of the package). Mounting orientation is the
 * board/wiring layer's job, not this file's.
 */
#ifndef FB_ICM42688P_H
#define FB_ICM42688P_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_ICM42688P_WHO_AM_I 0x47

typedef struct fb_icm42688p fb_icm42688p_t;

fb_icm42688p_t *fb_icm42688p_new(void);
void fb_icm42688p_free(fb_icm42688p_t *dev);
void fb_icm42688p_reset(fb_icm42688p_t *dev);       /* power-on / soft reset state */

/* SPI slave side. Mode 0/3, MSB first, 8-bit words. First byte after CS assert is the
 * address with bit 7 = read; following bytes are data with auto-incrementing address. */
void    fb_icm42688p_cs(fb_icm42688p_t *dev, bool asserted);
uint8_t fb_icm42688p_xfer(fb_icm42688p_t *dev, uint8_t mosi);

/* Physical inputs in chip frame: gyro in deg/s, accel in g (specific force, so +1 g on Z at rest
 * with Z up), temperature in degC. Values are latched into the data registers at the next sample. */
void fb_icm42688p_set_motion(fb_icm42688p_t *dev, const float gyro_dps[3], const float accel_g[3], float temp_c);

/* Output data rate currently programmed (0 when gyro/accel are off). Gyro and accel share the
 * rate in this model (the firmware programs both to the same ODR). */
uint32_t fb_icm42688p_odr_hz(const fb_icm42688p_t *dev);

/* Emulator calls this once per 1/ODR of virtual time while odr_hz() != 0. Latches the current
 * motion into the data registers, sets INT_STATUS.DATA_RDY. Returns true when INT1 should be
 * pulsed (data-ready routed to INT1 and enabled). */
bool fb_icm42688p_sample(fb_icm42688p_t *dev);

/* INT1 pulse width the firmware configured (100 µs or 8 µs) and polarity (true = active high). */
uint32_t fb_icm42688p_int1_pulse_ns(const fb_icm42688p_t *dev);
bool     fb_icm42688p_int1_active_high(const fb_icm42688p_t *dev);

/* Faults (fb_icm42688p_desc(), fb/device.h): "freeze" = the data registers stop updating at
 * sample(); DATA_RDY and INT1 keep coming, so the driver keeps reading the same values.
 * Returns false for an unknown fault id. Faults survive fb_icm42688p_reset(). */
bool fb_icm42688p_set_fault(fb_icm42688p_t *dev, const char *fault, bool on, const double *params);
bool fb_icm42688p_fault_active(const fb_icm42688p_t *dev, const char *fault);

/* Introspection for tests / debug panels. */
uint8_t fb_icm42688p_peek(const fb_icm42688p_t *dev, uint8_t bank, uint8_t reg);
bool    fb_icm42688p_is_on(const fb_icm42688p_t *dev);   /* gyro or accel in low-noise mode */

#ifdef __cplusplus
}
#endif
#endif
