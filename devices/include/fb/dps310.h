/* FlightBench device model: Infineon DPS310 barometer (I2C; the SPL06-001 on the Nexus-XR is
 * register-compatible and is detected by the same RotorFlight driver, barometer_dps310.c).
 *
 * Emulator-independent. The emulator's I2C controller model calls:
 *   fb_dps310_i2c_write(dev, bytes, n)   master wrote n bytes after addressing the chip (W):
 *                                        bytes[0] = register pointer, bytes[1..] = data
 *   fb_dps310_i2c_read(dev, buf, n)      master reads n bytes (R) from the current pointer,
 *                                        auto-incrementing
 * or, byte by byte as a bus controller clocks them (the same semantics):
 *   fb_dps310_i2c_start(dev)             addressed for writing: the next byte is the pointer
 *   fb_dps310_i2c_write_byte(dev, b)     false = NACK (pointer or register out of range)
 *   fb_dps310_i2c_read_byte(dev)         byte at the pointer, pointer + 1
 * Noise:    every sample adds Gaussian pressure noise of the datasheet's precision for the
 *           programmed oversampling (PM_PRC; 16x "standard" = 0.35 Pa RMS, table 16 of the
 *           DPS310 datasheet), xorshift PRNG with a fixed seed (reproducible);
 *           fb_dps310_set_noise_scale(dev, k) scales it (0 = exact values, tests).
 *           Not decoration: with a noiseless 0 m the RotorFlight altitude estimate stays 0
 *           (position.c uses the ground offset only when it is != 0.0f).
 * Fault:    fb_dps310_set_fault(dev, "freeze", on, NULL): fb_dps310_sample() stops latching new
 *           values, the result registers keep the last measurement; the fault and those values
 *           are kept across a soft reset (SOFT_RST) and fb_dps310_reset() (power cycle); a
 *           one-shot measurement started while frozen never completes (no RDY)
 * Physics:  fb_dps310_set_environment(dev, pressure_pa, temp_c)
 * Time:     fb_dps310_rate_hz()  measurement rate the firmware programmed (0 = idle/standby/one-shot)
 *           fb_dps310_sample()   called once per 1/rate: latches PSR/TMP registers, sets *_RDY
 *           fb_dps310_oneshot_us()  MEAS_CTRL 1/2 (one pressure / temperature measurement): its
 *                                conversion time (datasheet table 16, per PM_PRC / TMP_PRC), 0
 *                                otherwise; the emulator calls fb_dps310_sample() once after it,
 *                                which latches that result, sets its RDY and returns to idle
 *
 * Calibration coefficients are synthetic and chosen so that the driver's compensation formula
 * inverts exactly: c00 = FB_DPS310_C00, c10 = FB_DPS310_C10, all higher-order terms 0, c0 = 0,
 * c1 = FB_DPS310_C1. P = c00 + c10 * Praw_sc: 0.098 Pa per raw LSB at 16x oversampling (finer than
 * the 0.35 Pa noise), 30..120 kPa within the 24-bit result at 1x..4x and 16x..128x; at 8x
 * (kP 7864320) only 73.3..126.7 kPa fit (below ~2.7 km ISA the result clamps). RotorFlight uses 16x.
 * The model computes raw values from the requested pressure/temperature through that inverse,
 * so what the firmware reports equals what was set (within 24-bit rounding).
 */
#ifndef FB_DPS310_H
#define FB_DPS310_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_DPS310_I2C_ADDR 0x76
#define FB_DPS310_PRODUCT_ID 0x10
#define FB_DPS310_C00 100000   /* 20-bit, pressure offset, Pa */
#define FB_DPS310_C10 25000    /* 20-bit, pressure gain: P = c00 + c10 * Praw_sc -> Praw_sc ~ 0.05 at sea level */
#define FB_DPS310_C1  8        /* 12-bit, temperature gain: T = c1 * Traw_sc */

typedef struct fb_dps310 fb_dps310_t;

fb_dps310_t *fb_dps310_new(void);
void fb_dps310_free(fb_dps310_t *dev);
void fb_dps310_reset(fb_dps310_t *dev);

/* I2C slave side (see header comment). Return false when the register is out of range (NACK). */
bool fb_dps310_i2c_write(fb_dps310_t *dev, const uint8_t *bytes, int n);
bool fb_dps310_i2c_read(fb_dps310_t *dev, uint8_t *buf, int n);
void    fb_dps310_i2c_start(fb_dps310_t *dev);
bool    fb_dps310_i2c_write_byte(fb_dps310_t *dev, uint8_t b);
uint8_t fb_dps310_i2c_read_byte(fb_dps310_t *dev);

/* Faults of the descriptor ("freeze"); false for an unknown fault id */
bool fb_dps310_set_fault(fb_dps310_t *dev, const char *fault, bool on, const double *params);
bool fb_dps310_fault_active(const fb_dps310_t *dev, const char *fault);

void     fb_dps310_set_environment(fb_dps310_t *dev, float pressure_pa, float temp_c);
uint32_t fb_dps310_rate_hz(const fb_dps310_t *dev);   /* max(pressure rate, temperature rate) in continuous mode, else 0 */
uint32_t fb_dps310_oneshot_us(const fb_dps310_t *dev); /* pending one-shot: conversion time in us, else 0 */
void     fb_dps310_sample(fb_dps310_t *dev);
void     fb_dps310_set_noise_scale(fb_dps310_t *dev, float k);   /* default 1 */
float    fb_dps310_noise_pa(const fb_dps310_t *dev);             /* RMS at the current PM_PRC, scaled */

/* Test/debug: what the driver would compute from the current registers with the synthetic coefficients. */
float fb_dps310_expected_pressure_pa(const fb_dps310_t *dev);
float fb_dps310_expected_temp_c(const fb_dps310_t *dev);
uint8_t fb_dps310_peek(const fb_dps310_t *dev, uint8_t reg);

#ifdef __cplusplus
}
#endif
#endif
