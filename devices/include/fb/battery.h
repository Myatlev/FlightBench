/* FlightBench device model: a LiPo flight battery as the flight controller's voltage and current
 * sensors see it (the SpeedyBee F405 WING's VBAT / CURR pads, iNav's ADC battery meter).
 *
 * Emulator-independent. The emulator hands the model the current the aircraft draws and time:
 *   fb_battery_step(dev, current_a, dt_s)   the state of charge integrates the current
 *   fb_battery_voltage() / _current_a() / _mah_used() / _soc()   what the pack does
 *   fb_battery_sense_v() / _sense_a()        what reaches the FC's sensor inputs (faults applied)
 *
 * Pack voltage = cells x OCV(SoC) - I x R. OCV is a simple LiPo curve, per cell, piecewise
 * linear over SoC: 0 % 3.30 V, 5 % 3.50, 10 % 3.60, 20 % 3.70, 30 % 3.75, 40 % 3.79, 50 % 3.83,
 * 60 % 3.87, 70 % 3.92, 80 % 3.98, 90 % 4.06, 100 % 4.20 V. SoC -= I dt / capacity; SoC stops
 * at 0 (the curve's 3.30 V/cell: the model does not go below it). R = resistance_mohm (default
 * 0: no sag). The current is what the emulator says the aircraft takes (on the FlightBench
 * machines: current_scale x the ESC model's current + fc_current_a); the battery does not feed
 * anything back (the ESC's supply voltage is the aircraft's, not this pack's).
 *
 * Props (fb_battery_config_t, the descriptor's): cells 4, capacity_mah 4200, start_soc 1.0,
 * resistance_mohm 0, current_scale 1.0 (x the ESC's current), fc_current_a 0.3 (FC, receiver,
 * GPS, servos at rest).
 *
 * Faults (fb_battery_set_fault(); survive fb_battery_reset()):
 *   voltage     params volts (0..60; the F405's ADC saturates at 36.3 V through its 11:1 divider):
 *               the FC's voltage sensor reads this instead of the pack
 *               (the pack itself goes on as before)
 *   disconnect  the sense leads are off: the FC reads 0 V and 0 A (the bench keeps the FC powered;
 *               the pack goes on as before)
 */
#ifndef FB_BATTERY_H
#define FB_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cells;
    double capacity_mah;
    double start_soc;           /* 0..1 at power-on / reset */
    double resistance_mohm;     /* pack internal resistance: sag = I x R */
    double current_scale;       /* the emulator's motor current x this */
    double fc_current_a;        /* the electronics' own draw */
} fb_battery_config_t;

typedef struct fb_battery fb_battery_t;

void fb_battery_default_config(fb_battery_config_t *cfg);
fb_battery_t *fb_battery_new(const fb_battery_config_t *cfg);   /* NULL = defaults */
void fb_battery_free(fb_battery_t *dev);
void fb_battery_reset(fb_battery_t *dev);      /* a fresh pack at start_soc, 0 mAh used */
const fb_battery_config_t *fb_battery_config(const fb_battery_t *dev);

double fb_battery_ocv_cell(double soc);         /* the curve above, soc clamped to 0..1 */
void   fb_battery_step(fb_battery_t *dev, double current_a, double dt_s);
double fb_battery_voltage(const fb_battery_t *dev);    /* at the pack's terminals */
double fb_battery_current_a(const fb_battery_t *dev);  /* the last step's */
double fb_battery_mah_used(const fb_battery_t *dev);
double fb_battery_soc(const fb_battery_t *dev);
double fb_battery_sense_v(const fb_battery_t *dev);    /* what the FC's VBAT input sees */
double fb_battery_sense_a(const fb_battery_t *dev);    /* ... and its current sensor */

bool fb_battery_set_fault(fb_battery_t *dev, const char *fault, bool on, const double *params);
bool fb_battery_fault_active(const fb_battery_t *dev, const char *fault);

#ifdef __cplusplus
}
#endif
#endif
