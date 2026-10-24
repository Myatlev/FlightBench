/* FlightBench device model: a brushless ESC + motor as the flight controller sees it.
 *
 *   command    fb_esc_set_dshot()      decoded DShot value (0..2047) + telemetry-request bit
 *   physics    fb_esc_set_load()       shaft load torque fraction 0..1 from the FDM (optional)
 *              fb_esc_step()           advance the motor model by dt
 *   outputs    fb_esc_rpm()/erpm()     mechanical / electrical RPM (erpm = rpm * poles / 2)
 *              fb_esc_telemetry_frame() the 10-byte KISS/BLHeli32 frame the ESC sends on its
 *                                      telemetry wire when the FC asked (sensors/esc_sensor.c)
 *              fb_esc_telemetry_due()  true once per telemetry request until the frame is taken
 *
 * Signal loss: without a DShot frame (fb_esc_set_dshot) for the signal timeout (default 100 ms,
 * descriptor prop signal_timeout_ms, fb_esc_set_signal_timeout) the throttle drops to 0 and the
 * motor spins down, as a real ESC does when the FC stops, hangs or resets. The next frame sets
 * the throttle again.
 *
 * Motor model: first-order response of RPM towards throttle * kv * V (minus a load-dependent
 * droop), time constant configurable; current from a simple quadratic law. Good enough for a
 * governor to have something to regulate; a real aerodynamic load comes from the FDM later.
 */
#ifndef FB_ESC_H
#define FB_ESC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fb_esc fb_esc_t;

typedef struct {
    float kv;             /* rpm per volt (motor), e.g. 520 for a Pyro 700-52 class */
    uint8_t poles;        /* magnet poles (firmware: motor_poles), e.g. 10 */
    float tau_s;          /* spin-up time constant */
    float max_current_a;  /* current at full throttle, full load */
    float idle_current_a;
    float voltage_v;      /* pack voltage after power-on/reset (0 -> 50 V, 12S nominal-ish) */
} fb_esc_params_t;

void fb_esc_default_params(fb_esc_params_t *p);
fb_esc_t *fb_esc_new(const fb_esc_params_t *p);   /* NULL -> defaults: kv 520, 10 poles, tau 0.4 s, 200 A, 50 V */
void fb_esc_free(fb_esc_t *dev);
void fb_esc_reset(fb_esc_t *dev);

void fb_esc_set_voltage(fb_esc_t *dev, float volts);
void fb_esc_set_load(fb_esc_t *dev, float load_0_1);          /* 0 = free spinning, 1 = full rated load */
void fb_esc_set_dshot(fb_esc_t *dev, uint16_t value, bool request_telemetry);
#define FB_ESC_SIGNAL_TIMEOUT_MS_DEFAULT 100
void fb_esc_set_signal_timeout(fb_esc_t *dev, float seconds);   /* 0 = never; kept across reset */
bool fb_esc_signal_lost(const fb_esc_t *dev);                    /* no frame for the timeout */
void fb_esc_step(fb_esc_t *dev, float dt_s);

float    fb_esc_rpm(const fb_esc_t *dev);
uint32_t fb_esc_erpm(const fb_esc_t *dev);
float    fb_esc_current_a(const fb_esc_t *dev);
float    fb_esc_throttle(const fb_esc_t *dev);                 /* 0..1 as commanded */
bool     fb_esc_armed(const fb_esc_t *dev);                    /* saw a non-zero throttle frame */

bool fb_esc_telemetry_due(const fb_esc_t *dev);
/* Fills the 10-byte frame (temp, volt 10mV BE, curr 10mA BE, mAh BE, erpm/100 BE, crc8 kiss) and
 * clears the request. Returns 10, or 0 when nothing was requested. */
int fb_esc_telemetry_frame(fb_esc_t *dev, uint8_t out[10]);

uint8_t fb_esc_crc8_kiss(const uint8_t *data, int len);       /* poly 0x07, init 0 */

/* Faults (fb_esc_desc(), fb/device.h): "telemetry_lost" = the ESC stops answering: no
 * bidirectional-DShot eRPM reply (the emulator asks fb_esc_telemetry_ok() before sending one) and
 * no KISS frame (telemetry_due() false, telemetry_frame() 0). The motor itself keeps running.
 * Returns false for an unknown fault id. Faults survive fb_esc_reset(). */
bool fb_esc_set_fault(fb_esc_t *dev, const char *fault, bool on, const double *params);
bool fb_esc_fault_active(const fb_esc_t *dev, const char *fault);
bool fb_esc_telemetry_ok(const fb_esc_t *dev);

#ifdef __cplusplus
}
#endif
#endif
