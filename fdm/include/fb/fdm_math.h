/* FlightBench FDM: small vector / quaternion / actuator helpers shared by the models. Plain C11. */
#ifndef FB_FDM_MATH_H
#define FB_FDM_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_GRAVITY 9.80665
#define FB_DEG (3.14159265358979323846 / 180.0)

double fb_clamp(double v, double lo, double hi);
void fb_v3_set(double out[3], double x, double y, double z);
void fb_v3_copy(double out[3], const double a[3]);
void fb_v3_add(double out[3], const double a[3], const double b[3]);
void fb_v3_scale(double out[3], const double a[3], double k);
void fb_v3_cross(double out[3], const double a[3], const double b[3]);
double fb_v3_dot(const double a[3], const double b[3]);
double fb_v3_norm(const double a[3]);

/* Quaternion w, x, y, z; body -> NED. */
void fb_q_identity(double q[4]);
void fb_q_normalize(double q[4]);
void fb_q_mul(double out[4], const double a[4], const double b[4]);
void fb_q_conj(double out[4], const double q[4]);
void fb_q_from_euler(double q[4], double roll, double pitch, double yaw);      /* ZYX, radians */
void fb_q_to_euler(const double q[4], double *roll, double *pitch, double *yaw);
void fb_q_to_dcm(const double q[4], double m[3][3]);                         /* m: body -> NED */
void fb_q_rotate(const double q[4], const double v_body[3], double v_ned[3]);
void fb_q_rotate_inv(const double q[4], const double v_ned[3], double v_body[3]);
/* Integrate body rates over dt (first order, renormalised). */
void fb_q_integrate(double q[4], const double rates[3], double dt);
/* The body-frame rotation vector that takes q to target (target = q * rot), shortest way. */
void fb_q_error(const double q[4], const double target[4], double rotvec[3]);
/* The i,j element of the DCM straight from the quaternion (e.g. 2,2 = body z . NED down). */
double fb_q_dcm_element(const double q[4], int row, int col);

/* An actuator with a transport delay (whole steps, at most FB_ACT_MAX_DELAY) and a slew limit. */
#define FB_ACT_MAX_DELAY 63
typedef struct {
    double buf[FB_ACT_MAX_DELAY + 1];
    int head, delay;
    double pos;              /* after the slew, -1..1 */
    double max_step;         /* slew limit per step (0 = none) */
} fb_actuator_t;
/* delay_ms rounded to whole steps and capped; speed_s60 = seconds per 60 deg of deflection with
 * full_deg the deflection at +-1 (0 = no slew limit). */
void fb_actuator_init(fb_actuator_t *a, double dt, double delay_ms, double speed_s60, double full_deg);
int  fb_actuator_delay_steps(double delay_ms, double dt);
void fb_actuator_reset(fb_actuator_t *a, double pos);
double fb_actuator_step(fb_actuator_t *a, double cmd);   /* push a command, get the position */

/* µs -> -1..1 with centre and travel (the µs from the centre to full deflection). */
double fb_us_norm(uint16_t us, double centre_us, double travel_us);

#ifdef __cplusplus
}
#endif
#endif
