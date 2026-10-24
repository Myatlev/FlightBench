/* FlightBench FDM: the rigid body every model shares — state, integration, the ground, the holder.
 *
 * The attitude never jumps: a ground contact holds the model (velocity zero) and rolls/pitches it
 * level in `settle_s` with body rates the IMU reads; a return to the pad does the same towards
 * heading 0; a hold (a hand or a stand) rotates it to a target attitude in `rotate_s` and then keeps
 * it still with the holder's reaction on the IMU. A contact beyond the model's crash rule counts as a
 * crash once per flight segment (airborne > 0.2 s re-arms it). Thrust is never blocked.
 */
#ifndef FB_BODY_H
#define FB_BODY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* roll, pitch in degrees; sink (down +) and horizontal drift in m/s at the contact. */
typedef bool (*fb_crash_rule_t)(double roll_deg, double pitch_deg, double sink_mps, double drift_mps);

typedef struct {
    double dt, t;
    double pos[3], vel[3];     /* NED */
    double quat[4];            /* body -> NED */
    double rates[3];           /* body, rad/s */
    double accel_body[3];      /* specific force, body */
    bool on_ground;
    double air_s;              /* airborne time since the last contact */
    bool crash_armed;
    unsigned crashes;
    double crash_info[3];
    bool crashed_now;          /* set by the step that counted a crash */
    /* settling to level on the ground */
    double settle_s, settle_left, settle_yaw;
    bool settle_to_pad;
    /* the holder */
    bool held, posed, pose_now;
    double hold_q[4];
    double hold_left;
    fb_crash_rule_t crash_rule;
} fb_body_t;

void fb_body_init(fb_body_t *b, double dt, fb_crash_rule_t rule);
void fb_body_reset(fb_body_t *b);                 /* on the ground, level, heading 0, at once */
void fb_body_return_to_pad(fb_body_t *b);
void fb_body_ground_reaction(fb_body_t *b);       /* accel_body = R^T (0, 0, -g) */
void fb_body_set_kinematics(fb_body_t *b, const double pos[3], const double vel[3], const double q[4],
                            const double rates[3]);

/* The holder: rotate to q in rotate_s (0 = at once), then still. */
void fb_body_hold_quat(fb_body_t *b, const double q[4], double rotate_s);
void fb_body_release(fb_body_t *b);
/* One step of the holder: true when the body is held this step (the model skips its dynamics).
 * pose_now is set on the step the attitude is first reached (rates zero from then on). */
bool fb_body_hold_step(fb_body_t *b);

/* One step of free flight with the body force (N, gravity excluded) and moment (N m) about the CG,
 * for a model that stops on contact (helicopter): integrates rates, attitude, velocity and position;
 * at ground level and pushed into it the model is held and levelled; returns true on the step of a
 * new contact (the model may reset rotor states). Sets crashed_now when the contact is a crash. */
bool fb_body_step_stop(fb_body_t *b, const double force[3], const double moment[3], double mass,
                       const double inertia[3]);

/* Pieces for models with their own ground behaviour (plane): the rates/attitude step from a moment,
 * the velocity/position step from an NED acceleration (gravity included by the caller). */
void fb_body_rotate(fb_body_t *b, const double moment[3], const double inertia[3]);
void fb_body_translate(fb_body_t *b, const double accel_ned[3]);
/* The contact bookkeeping (crash rule, settle timer): call once when a contact begins. */
void fb_body_contact(fb_body_t *b, bool settle);
/* Advance the settle-to-level rotation if one is pending (on the ground). */
void fb_body_settle_step(fb_body_t *b);

void fb_body_euler(const fb_body_t *b, double *roll, double *pitch, double *yaw);

#ifdef __cplusplus
}
#endif
#endif
