#include "fb/body.h"
#include "fb/fdm_math.h"

#include <math.h>
#include <string.h>

#define SETTLE_S 0.25           /* on its side (90 deg) -> 360 deg/s; worst case 180 deg -> 720 */
#define CRASH_REARM_S 0.2

void fb_body_init(fb_body_t *b, double dt, fb_crash_rule_t rule)
{
    memset(b, 0, sizeof *b);
    b->dt = dt;
    b->settle_s = SETTLE_S;
    b->crash_rule = rule;
    fb_body_reset(b);
}

void fb_body_ground_reaction(fb_body_t *b)
{
    const double up[3] = { 0, 0, -FB_GRAVITY };
    fb_q_rotate_inv(b->quat, up, b->accel_body);
}

void fb_body_reset(fb_body_t *b)
{
    b->t = 0;
    memset(b->pos, 0, sizeof b->pos);
    memset(b->vel, 0, sizeof b->vel);
    memset(b->rates, 0, sizeof b->rates);
    fb_q_identity(b->quat);
    b->on_ground = true;
    b->air_s = 0;
    b->crash_armed = true;
    b->crashes = 0;
    memset(b->crash_info, 0, sizeof b->crash_info);
    b->crashed_now = false;
    b->settle_left = 0; b->settle_yaw = 0; b->settle_to_pad = false;
    b->held = b->posed = b->pose_now = false;
    b->hold_left = 0;
    fb_body_ground_reaction(b);
}

void fb_body_return_to_pad(fb_body_t *b)
{
    memset(b->pos, 0, sizeof b->pos);
    memset(b->vel, 0, sizeof b->vel);
    memset(b->rates, 0, sizeof b->rates);
    b->on_ground = true;
    b->air_s = 0;
    b->crash_armed = true;
    b->held = b->posed = b->pose_now = false;
    b->settle_left = b->settle_s;
    b->settle_yaw = 0;
    b->settle_to_pad = true;
    fb_body_ground_reaction(b);
}

void fb_body_set_kinematics(fb_body_t *b, const double pos[3], const double vel[3], const double q[4],
                            const double rates[3])
{
    if (pos) fb_v3_copy(b->pos, pos);
    if (vel) fb_v3_copy(b->vel, vel);
    if (q) { memcpy(b->quat, q, sizeof b->quat); fb_q_normalize(b->quat); }
    if (rates) fb_v3_copy(b->rates, rates);
    b->held = b->posed = b->pose_now = false;
    b->settle_left = 0; b->settle_to_pad = false;
    b->on_ground = b->pos[2] >= 0;
    if (b->on_ground) { b->pos[2] = 0; fb_body_ground_reaction(b); }
}

void fb_body_euler(const fb_body_t *b, double *roll, double *pitch, double *yaw)
{
    fb_q_to_euler(b->quat, roll, pitch, yaw);
}

/* ---- the holder ---------------------------------------------------------------------------- */

void fb_body_hold_quat(fb_body_t *b, const double q[4], double rotate_s)
{
    memcpy(b->hold_q, q, sizeof b->hold_q);
    fb_q_normalize(b->hold_q);
    b->held = true;
    b->posed = b->pose_now = false;
    b->hold_left = rotate_s > 0 ? rotate_s : 0;
    b->settle_left = 0; b->settle_to_pad = false;
    memset(b->vel, 0, sizeof b->vel);
    if (b->hold_left <= 0) {
        memcpy(b->quat, b->hold_q, sizeof b->quat);
        memset(b->rates, 0, sizeof b->rates);
    }
}

void fb_body_release(fb_body_t *b)
{
    b->held = b->posed = b->pose_now = false;
    b->hold_left = 0;
}

bool fb_body_hold_step(fb_body_t *b)
{
    b->pose_now = false;
    if (!b->held) return false;
    double dt = b->dt;
    memset(b->vel, 0, sizeof b->vel);
    if (b->hold_left > 0) {
        double e[3];
        fb_q_error(b->quat, b->hold_q, e);
        double left = fmax(b->hold_left, dt);
        for (int i = 0; i < 3; i++) b->rates[i] = e[i] / left;
        fb_q_integrate(b->quat, b->rates, dt);
        b->hold_left -= dt;
        if (b->hold_left <= 1e-12) {
            b->hold_left = 0;
            memcpy(b->quat, b->hold_q, sizeof b->quat);
        }
    } else {
        memset(b->rates, 0, sizeof b->rates);
        if (!b->posed) { b->posed = true; b->pose_now = true; }
    }
    b->on_ground = b->pos[2] >= -1e-9;
    fb_body_ground_reaction(b);
    b->t += dt;
    return true;
}

/* ---- contact and settling ------------------------------------------------------------------ */

void fb_body_contact(fb_body_t *b, bool settle)
{
    double roll, pitch, yaw;
    fb_body_euler(b, &roll, &pitch, &yaw);
    double rd = roll / FB_DEG, pd = pitch / FB_DEG;
    double sink = b->vel[2], drift = hypot(b->vel[0], b->vel[1]);
    b->crashed_now = false;
    if (b->crash_rule && b->crash_rule(rd, pd, sink, drift) && b->crash_armed) {
        b->crash_armed = false;
        b->crashes++;
        b->crash_info[0] = rd; b->crash_info[1] = pd;
        b->crash_info[2] = sqrt(sink * sink + drift * drift);
        b->crashed_now = true;
    }
    if (settle) {
        b->settle_yaw = yaw;
        b->settle_left = b->settle_s;
        b->settle_to_pad = false;
    }
}

void fb_body_settle_step(fb_body_t *b)
{
    if (b->settle_left <= 0) return;
    double dt = b->dt, target[4], e[3];
    fb_q_from_euler(target, 0, 0, b->settle_yaw);
    fb_q_error(b->quat, target, e);
    double left = fmax(b->settle_left, dt);
    for (int i = 0; i < 3; i++) b->rates[i] = e[i] / left;
    fb_q_integrate(b->quat, b->rates, dt);
    b->settle_left -= dt;
    if (b->settle_left <= 1e-12) {
        b->settle_left = 0;
        b->settle_to_pad = false;
        memcpy(b->quat, target, sizeof target);
    }
}

/* ---- integration --------------------------------------------------------------------------- */

void fb_body_rotate(fb_body_t *b, const double moment[3], const double inertia[3])
{
    double iw[3] = { inertia[0] * b->rates[0], inertia[1] * b->rates[1], inertia[2] * b->rates[2] };
    double gyro[3];
    fb_v3_cross(gyro, b->rates, iw);
    for (int i = 0; i < 3; i++) {
        double acc = inertia[i] > 0 ? (moment[i] - gyro[i]) / inertia[i] : 0;
        b->rates[i] += acc * b->dt;
    }
    fb_q_integrate(b->quat, b->rates, b->dt);
}

void fb_body_translate(fb_body_t *b, const double a_ned[3])
{
    for (int i = 0; i < 3; i++) b->vel[i] += a_ned[i] * b->dt;
    for (int i = 0; i < 3; i++) b->pos[i] += b->vel[i] * b->dt;
}

bool fb_body_step_stop(fb_body_t *b, const double force[3], const double moment[3], double mass,
                       const double inertia[3])
{
    double dt = b->dt;
    double q0[4];
    memcpy(q0, b->quat, sizeof q0);
    b->crashed_now = false;

    for (int i = 0; i < 3; i++) b->accel_body[i] = mass > 0 ? force[i] / mass : 0;
    fb_body_rotate(b, moment, inertia);

    double a_ned[3];
    fb_q_rotate(b->quat, b->accel_body, a_ned);
    a_ned[2] += FB_GRAVITY;

    bool pad = b->settle_to_pad && b->settle_left > 0;
    bool touching = pad || (b->pos[2] >= 0.0 && (a_ned[2] > 0 || b->vel[2] > 0));
    bool new_contact = false;
    if (touching) {
        memcpy(b->quat, q0, sizeof q0);          /* the moments do not act on the ground */
        if (!b->on_ground) { fb_body_contact(b, true); new_contact = true; }
        b->on_ground = true;
        b->air_s = 0;
        b->pos[2] = 0.0;
        memset(b->vel, 0, sizeof b->vel);
        memset(b->rates, 0, sizeof b->rates);
        fb_body_settle_step(b);
        fb_body_ground_reaction(b);
    } else {
        b->on_ground = false;
        b->settle_left = 0;
        b->air_s += dt;
        if (b->air_s > CRASH_REARM_S) b->crash_armed = true;
        fb_body_translate(b, a_ned);
        if (b->pos[2] > 0.0) b->pos[2] = 0.0;
    }
    b->t += dt;
    return new_contact;
}
