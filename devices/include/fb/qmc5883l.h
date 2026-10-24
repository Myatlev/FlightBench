/* FlightBench device model: QST QMC5883L 3-axis magnetometer (I2C, address 0x0D), the compass in
 * the usual GPS module (M10 + QMC5883L), for iNav 9.1.0's drivers/compass/compass_qmc5883l.c.
 *
 * Emulator-independent. The emulator's I2C controller calls, per transaction:
 *   fb_qmc5883l_i2c_start(dev, read)     address phase; false = NACK (fault "detached")
 *   fb_qmc5883l_i2c_write_byte(dev, b)   first byte after a write start = register pointer, then
 *                                        data with auto-increment (0x09..0x0B are writable, the
 *                                        rest is read-only: ACKed and ignored)
 *   fb_qmc5883l_i2c_read_byte(dev)       byte at the pointer, pointer + 1 (with CONTROL 2 ROL_PNT
 *                                        the pointer rolls from 0x06 back to 0x00)
 *
 * Registers (QMC5883L datasheet v1.0, 9.2): 0x00..0x05 X, Y, Z LSB first (int16), 0x06 STATUS
 * (DRDY bit 0, OVL bit 1, DOR bit 2), 0x07..0x08 TOUT (int16, 100 LSB/degC, offset not
 * calibrated on the chip: here 0 LSB = 0 degC), 0x09 CONTROL 1 (MODE[1:0] 00 standby 01
 * continuous, ODR[3:2] 10/50/100/200 Hz, RNG[5:4] 2 G / 8 G, OSR[7:6] 512/256/128/64), 0x0A
 * CONTROL 2 (INT_ENB bit 0, ROL_PNT bit 6, SOFT_RST bit 7), 0x0B SET/RESET period (the datasheet
 * recommends 0x01), 0x0D chip ID 0xFF. Everything 0 after power-on and SOFT_RST (standby: iNav's
 * detection checks that CONTROL 1 reads 0 after its reset).
 *
 * Measurements: in continuous mode one sample per 1/ODR of the chip's clock (fb_qmc5883l_tick in
 * virtual time; the first one 1/ODR after MODE was set), the field set by
 * fb_qmc5883l_set_field() (gauss, in the chip's own axes: the X/Y arrows printed on the chip,
 * Z = X x Y, out of the top face) times the range's sensitivity (12000 LSB/G at 2 G, 3000 LSB/G
 * at 8 G), plus the fault offset, plus Gaussian noise (sigma in mG, fixed-seed PRNG); a sample
 * beyond the range sets OVL and saturates at -32768..32767. DRDY is set per sample and cleared by
 * reading any data register (0x00..0x05); a sample arriving while DRDY is still set sets DOR
 * (cleared the same way). The data registers keep the last sample in standby.
 *
 * iNav's driver: detection = write CONTROL 2 = 0x80 (SOFT_RST), 30 ms, read ID (0xFF), read
 * CONTROL 1 (0 = standby); init = SET/RESET 0x01, CONTROL 1 = 0x1D (continuous, 200 Hz, 8 G,
 * OSR 512); read (40 Hz) = STATUS, then 6 bytes from 0x00 when DRDY.
 *
 * Mounting: the firmware turns the chip frame into its board frame with `align_mag`
 * (sensors/boardalignment.c applySensorAlignment: board = align(chip); iNav's default for an
 * external compass, align_mag DEFAULT, is CW270_DEG_FLIP). fb_mag_* below give the earth field in
 * NED and the chip-frame field for a body attitude and a mount, the inverse of that alignment.
 *
 * Faults (fb_qmc5883l_set_fault(); survive SOFT_RST and fb_qmc5883l_reset()):
 *   freeze    the data registers stop updating (DRDY keeps coming: the driver reads the same field)
 *   offset    params x_mg, y_mg, z_mg (each default 0): a hard-iron offset added in the chip frame
 *   noise     params sigma_mg: this much Gaussian noise per axis instead of the configured noise
 *   detached  the chip is gone from the bus: the address phase NACKs
 *
 * Not modelled: the INT/DRDY pin, OSR effects (noise, timing), the SET/RESET period, the
 * temperature dependence of the field.
 */
#ifndef FB_QMC5883L_H
#define FB_QMC5883L_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_QMC5883L_I2C_ADDR 0x0D
#define FB_QMC5883L_CHIP_ID  0xFF

enum {
    FB_QMC5883L_DATA_X = 0x00, FB_QMC5883L_STATUS = 0x06, FB_QMC5883L_TOUT = 0x07,
    FB_QMC5883L_CTRL1 = 0x09, FB_QMC5883L_CTRL2 = 0x0A, FB_QMC5883L_SET_RESET = 0x0B,
    FB_QMC5883L_ID = 0x0D, FB_QMC5883L_NREGS = 0x0E,
};
#define FB_QMC5883L_DRDY        0x01
#define FB_QMC5883L_OVL         0x02
#define FB_QMC5883L_DOR         0x04
#define FB_QMC5883L_ROL_PNT     0x40
#define FB_QMC5883L_SOFT_RST    0x80

/* Yerevan (40.18 N, 44.50 E, 1 km) in 2026.75, WMM2025: D 6.52 deg E, I 59.45 deg, F 49.67 uT.
 * iNav's own table (navigation_declination_gen.c) gives 6.52 deg there, so its heading with
 * inav_auto_mag_decl matches the FDM's true heading. */
#define FB_MAG_DECLINATION_DEG  6.52
#define FB_MAG_INCLINATION_DEG  59.45
#define FB_MAG_INTENSITY_UT     49.67
#define FB_QMC5883L_NOISE_MG    2.0     /* the datasheet's field resolution, as sigma per axis */

/* iNav/Betaflight sensor_align_e (drivers/sensor.h): CW0 1 .. CW270 4, CW0_FLIP 5 .. CW270_FLIP 8 */
enum {
    FB_MAG_CW0 = 1, FB_MAG_CW90, FB_MAG_CW180, FB_MAG_CW270,
    FB_MAG_CW0_FLIP, FB_MAG_CW90_FLIP, FB_MAG_CW180_FLIP, FB_MAG_CW270_FLIP,
};

typedef struct fb_qmc5883l fb_qmc5883l_t;

fb_qmc5883l_t *fb_qmc5883l_new(void);
void fb_qmc5883l_free(fb_qmc5883l_t *dev);
void fb_qmc5883l_reset(fb_qmc5883l_t *dev);     /* power cycle: registers 0, standby */

/* I2C slave, byte level */
bool    fb_qmc5883l_i2c_start(fb_qmc5883l_t *dev, bool read);
bool    fb_qmc5883l_i2c_write_byte(fb_qmc5883l_t *dev, uint8_t b);
uint8_t fb_qmc5883l_i2c_read_byte(fb_qmc5883l_t *dev);

/* environment and time */
void     fb_qmc5883l_set_field(fb_qmc5883l_t *dev, const double gauss_chip[3]);
void     fb_qmc5883l_set_temp(fb_qmc5883l_t *dev, double temp_c);
void     fb_qmc5883l_tick(fb_qmc5883l_t *dev, uint64_t dt_ns);
uint64_t fb_qmc5883l_next_due_ns(const fb_qmc5883l_t *dev);    /* UINT64_MAX = standby */
uint64_t fb_qmc5883l_samples(const fb_qmc5883l_t *dev);
double   fb_qmc5883l_lsb_per_gauss(const fb_qmc5883l_t *dev);   /* at the current range */
void     fb_qmc5883l_set_noise(fb_qmc5883l_t *dev, double sigma_mg, uint64_t seed);

bool fb_qmc5883l_set_fault(fb_qmc5883l_t *dev, const char *fault, bool on, const double *params);
bool fb_qmc5883l_fault_active(const fb_qmc5883l_t *dev, const char *fault);
uint8_t fb_qmc5883l_peek(const fb_qmc5883l_t *dev, uint8_t reg);

/* The earth field in NED, gauss: horizontal intensity cos(I) towards the declination (east of
 * true north +), vertical sin(I) down (+ in the northern hemisphere). */
void fb_mag_field_ned(double declination_deg, double inclination_deg, double intensity_ut,
                      double out_gauss[3]);
/* NED -> the body frame FRD of an attitude quaternion q = [w, x, y, z] (body -> NED) */
void fb_mag_ned_to_body(const double q[4], const double ned[3], double frd[3]);
/* FRD body -> the chip's axes for a compass mounted `align` (FB_MAG_CW*): the firmware's board
 * frame is (x, -y, -z) of FRD (X forward, Y left, Z up) and board = align(chip), so
 * chip = align^-1(board). False for an unknown align. */
bool fb_mag_body_to_chip(int align, const double frd[3], double chip[3]);
/* the alignment's name as iNav's CLI prints it ("CW270FLIP"), NULL = unknown; and back (0 = unknown) */
const char *fb_mag_align_name(int align);
int fb_mag_align_parse(const char *name);

#ifdef __cplusplus
}
#endif
#endif
