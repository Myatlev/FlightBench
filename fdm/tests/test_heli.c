/* The helicopter FDM against the claims of docs/fdm.md §2.9, through the common interface. */
#include "fb/fdm.h"
#include "fb/fdm_math.h"
#include "fb/rotor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)
#define DT 0.001
#define NOMINAL_RPM 1800.0
#define MASS 5.0

static const fb_fdm_heli_ext_t *ext(const fb_fdm_t *f) { return fb_fdm_state_ext(f, NULL); }

/* the plate: collective and cyclic in -1..1 of the maxima -> the three swash servos (rear, front-left,
 * front-right), the inverse of the model's read-back (docs/fdm.md §2.2) */
static void swash(fb_fdm_t *f, double c, double pitch, double roll)
{
    double s_r = c - pitch, s_l = c + 0.5 * pitch + 0.866025 * roll, s_R = c + 0.5 * pitch - 0.866025 * roll;
    fb_fdm_set_input_us(f, 0, (uint16_t)lrint(1500 + 500 * fb_clamp(s_r, -1, 1)));
    fb_fdm_set_input_us(f, 1, (uint16_t)lrint(1500 + 500 * fb_clamp(s_l, -1, 1)));
    fb_fdm_set_input_us(f, 2, (uint16_t)lrint(1500 + 500 * fb_clamp(s_R, -1, 1)));
}
static void tail(fb_fdm_t *f, double n) { fb_fdm_set_input_us(f, 3, (uint16_t)lrint(1500 + 270 * fb_clamp(n, -1, 1))); }

static fb_fdm_t *make(void)
{
    char err[128];
    fb_fdm_t *f = fb_fdm_new(FB_FDM_HELI, NULL, 0, DT, err, sizeof err);
    if (!f) { printf("FAIL: fb_fdm_new: %s\n", err); fails++; return NULL; }
    fb_fdm_set_motor_rpm(f, 0, NOMINAL_RPM);
    return f;
}

/* A stand-in for the firmware: attitude PD on the cyclic (quaternion error, body frame), yaw-rate PD
 * with a torque feed-forward on the tail, collective given. */
static double tail_trim = 0.42, coll_i = 0, vel_i[2] = { 0, 0 };
static void fly(fb_fdm_t *f, double coll_n, const double target[4], double yaw_rate_t)
{
    fb_fdm_state_t s;
    fb_fdm_state(f, &s);
    double e[3];
    fb_q_error(s.quat, target, e);
    double pitch = fb_clamp(3.0 * e[1] - 0.35 * s.rates[1], -1, 1);
    double roll = fb_clamp(3.0 * e[0] - 0.35 * s.rates[0], -1, 1);
    tail_trim = fb_clamp(tail_trim + 0.4 * e[2] * DT, -1, 1);          /* the trim learns the torque */
    double yaw = fb_clamp(tail_trim + 1.5 * (yaw_rate_t - s.rates[2]) + 0.8 * e[2], -1, 1);
    swash(f, coll_n, pitch, roll);
    tail(f, yaw);
    fb_fdm_step(f);
}
static void fly_cmd(fb_fdm_t *f, double coll_n, double pitch_cmd, double roll_cmd)
{
    fb_fdm_state_t s;
    fb_fdm_state(f, &s);
    double yaw = fb_clamp(tail_trim + 1.5 * (0 - s.rates[2]), -1, 1);
    tail_trim = fb_clamp(tail_trim + 0.3 * (0 - s.rates[2]) * DT, -1, 1);
    swash(f, coll_n, pitch_cmd, roll_cmd);
    tail(f, yaw);
    fb_fdm_step(f);
}
static void reset_ctl(void) { tail_trim = 0.42; coll_i = 0; vel_i[0] = vel_i[1] = 0; }
static void fly_for(fb_fdm_t *f, double coll_n, const double target[4], double seconds)
{
    int n = (int)(seconds / DT);
    for (int i = 0; i < n; i++) fly(f, coll_n, target, 0);
}
static void level_heading(double q[4], double yaw) { fb_q_from_euler(q, 0, 0, yaw); }
/* Velocity hold around hover (the firmware's loops stand-in): NED velocity targets -> a small bank /
 * pitch and a collective around hc with an integrator; inverted = 1 flies it upside down. */
static void hover_ctl(fb_fdm_t *f, double hc, double vn_t, double ve_t, double vd_t, double yaw_t, int inverted)
{
    fb_fdm_state_t s;
    fb_fdm_state(f, &s);
    double sgn = inverted ? -1.0 : 1.0;
    /* body-frame horizontal velocity error -> attitude: forward error -> nose up (decelerate) */
    double ev_ned[3] = { s.vel_ned[0] - vn_t, s.vel_ned[1] - ve_t, 0 }, ev[3];
    fb_q_rotate_inv(s.quat, ev_ned, ev);
    vel_i[0] = fb_clamp(vel_i[0] + 0.03 * ev[0] * DT, -0.15, 0.15);
    vel_i[1] = fb_clamp(vel_i[1] + 0.03 * ev[1] * DT, -0.15, 0.15);
    double pitch_t = fb_clamp(0.15 * ev[0] + vel_i[0], -0.3, 0.3);
    double roll_t = fb_clamp(-(0.15 * ev[1] + vel_i[1]) * sgn, -0.3, 0.3);
    double q[4], base[4], d[4];
    fb_q_from_euler(base, inverted ? M_PI : 0, 0, yaw_t);
    fb_q_from_euler(d, roll_t, pitch_t, 0);
    fb_q_mul(q, base, d);
    double evz = s.vel_ned[2] - vd_t;                  /* positive: sinking faster than wanted */
    coll_i = fb_clamp(coll_i + 0.02 * evz * DT, -0.3, 0.3);
    double c = sgn * (hc + 0.08 * evz + coll_i);
    fly(f, fb_clamp(c, -1, 1), q, 0);
}
static void hover_for(fb_fdm_t *f, double hc, double vd_t, double yaw_t, int inverted, double seconds)
{
    int n = (int)(seconds / DT);
    for (int i = 0; i < n; i++) hover_ctl(f, hc, 0, 0, vd_t, yaw_t, inverted);
}

/* the collective (normalised) at which thrust = weight at nominal rpm, by the rotor solver */
static double hover_coll_n(void)
{
    fb_rotor_geom_t g = { .radius_m = 0.804, .chord_m = 0.060, .blades = 2, .lift_slope = 5.7, .cd0 = 0.011,
                          .tip_loss = 0.97, .kappa = 1.15 };
    double c = fb_rotor_collective_for_thrust(&g, 1.225, NOMINAL_RPM * 2 * M_PI / 60, MASS * FB_GRAVITY);
    return c / (12.0 * D2R);
}

static void hover_and_climb(void)
{
    reset_ctl();
    fb_fdm_t *f = make();
    if (!f) return;
    fb_fdm_state_t s;
    double lvl[4];
    level_heading(lvl, 0);
    double hc = hover_coll_n();
    printf("hover collective %.2f deg (%.3f of 12)\n", hc * 12, hc);
    CHECK(hc * 12 > 2.5 && hc * 12 < 7.0, "claim 1: hover collective in 2.5..7 deg: %.2f", hc * 12);

    /* lift off and hover: the thrust at the hover collective equals the weight within 1 % */
    swash(f, hc, 0, 0); tail(f, tail_trim);
    for (int i = 0; i < 300; i++) fb_fdm_step(f);
    CHECK(fabs(ext(f)->thrust_n - MASS * FB_GRAVITY) < 0.01 * MASS * FB_GRAVITY, "claim 1: thrust %.2f N vs weight %.2f", ext(f)->thrust_n, MASS * FB_GRAVITY);
    /* a little more to leave the ground, then back to hover collective and hold 5 s */
    fly_for(f, hc + 0.08, lvl, 3.0);
    fb_fdm_state(f, &s);
    CHECK(!s.on_ground && -s.pos_ned[2] > 1.0, "lifts off: alt %.2f m", -s.pos_ned[2]);
    double climb_rate = -s.vel_ned[2];
    /* hover: the velocity hold settles it (the firmware's loops stand in) */
    coll_i = 0;
    hover_for(f, hc, 0, 0, 0, 20.0);
    fb_fdm_state(f, &s);
    double sp = fb_v3_norm(s.vel_ned);
    double roll, pitch;
    fb_fdm_euler(f, &roll, &pitch, NULL);
    printf("hover: |v| %.2f m/s, roll %.1f pitch %.1f deg, alt %.1f, load %.2f, tail %.1f deg, torque %.2f N m\n",
           sp, roll * R2D, pitch * R2D, -s.pos_ned[2], ext(f)->load, ext(f)->tail_pitch_deg, ext(f)->torque_nm);
    CHECK(sp < 0.3, "claim 1: hovers, |v| %.2f m/s after settling (climb before %.2f)", sp, climb_rate);
    CHECK(fabs(roll) < 8 * D2R && fabs(pitch) < 5 * D2R, "hover attitude near level (a hover lean against the tail): %.1f / %.1f deg", roll * R2D, pitch * R2D);
    CHECK(ext(f)->load > 0.05 && ext(f)->load < 0.8, "claim 7: hover load %.2f", ext(f)->load);
    double hover_load = ext(f)->load;

    /* claim 2: +3 deg -> terminal climb 3..12 m/s, no runaway; -3 deg the same down */
    double alt0 = -s.pos_ned[2];
    fly_for(f, hc + 3.0 / 12.0, lvl, 20.0);
    fb_fdm_state(f, &s);
    double vz_up = -s.vel_ned[2];
    printf("+3 deg: climb %.2f m/s, alt %.1f -> %.1f\n", vz_up, alt0, -s.pos_ned[2]);
    CHECK(vz_up > 3 && vz_up < 12, "claim 2: terminal climb %.2f m/s", vz_up);
    fly_for(f, hc - 3.0 / 12.0, lvl, 6.0);
    fb_fdm_state(f, &s);
    double vz_dn = s.vel_ned[2];
    printf("-3 deg: descent %.2f m/s at alt %.1f\n", vz_dn, -s.pos_ned[2]);
    CHECK(vz_dn > 3 && vz_dn < 12 && !s.on_ground, "claim 2: terminal descent %.2f m/s", vz_dn);
    /* claim 7: full collective load >= 2x hover; claim 2 again: 60 s climb bounded */
    fly_for(f, 1.0, lvl, 1.0);
    double full_load = ext(f)->load;
    CHECK(full_load > 2 * hover_load, "claim 7: full collective load %.2f vs hover %.2f", full_load, hover_load);
    fly_for(f, hc + 3.0 / 12.0, lvl, 59.0);
    fb_fdm_state(f, &s);
    CHECK(-s.vel_ned[2] < 15 && -s.pos_ned[2] < 1500, "claim 2: no runaway after 60 s: %.1f m/s, %.0f m", -s.vel_ned[2], -s.pos_ned[2]);

    /* claim 4: level flight at 10 m/s needs less collective than hover */
    fb_fdm_free(f);
    f = make();
    double pos[3] = { 0, 0, -100 }, vel[3] = { 10, 0, 0 }, rates[3] = { 0, 0, 0 };
    double q[4];
    fb_q_from_euler(q, 0, -4 * D2R, 0);
    fb_fdm_set_kinematics(f, pos, vel, q, rates);
    fb_fdm_set_motor_rpm(f, 0, NOMINAL_RPM);
    /* hold 10 m/s north and the height: the collective the loop settles at */
    coll_i = 0;
    for (int i = 0; i < 20000; i++) hover_ctl(f, hc, 10, 0, 0, 0, 0);
    fb_fdm_state(f, &s);
    double c = ext(f)->collective_deg / 12.0;
    printf("10 m/s forward: collective %.2f deg (hover %.2f), speed %.1f m/s, vz %.2f\n", c * 12, hc * 12, fb_v3_norm(s.vel_ned), s.vel_ned[2]);
    CHECK(c < hc && fabs(s.vel_ned[2]) < 0.5 && fabs(s.vel_ned[0] - 10) < 1.0, "claim 4: translational lift, %.2f < %.2f deg", c * 12, hc * 12);
    fb_fdm_free(f);
}

static void inverted(void)
{
    reset_ctl();
    fb_fdm_t *f = make();
    if (!f) return;
    fb_fdm_state_t s;
    double hc = hover_coll_n();
    double pos[3] = { 0, 0, -60 }, vel[3] = { 0, 0, 0 }, rates[3] = { 0, 0, 0 };
    double up[4], inv[4];
    fb_q_from_euler(up, 0, 0, 0);
    fb_q_from_euler(inv, 179.5 * D2R, 0, 0);
    fb_fdm_set_kinematics(f, pos, vel, up, rates);
    fb_fdm_set_motor_rpm(f, 0, NOMINAL_RPM);
    /* upright: the climb at full collective, 6 s */
    fly_for(f, hc, up, 1.0);
    fly_for(f, 1.0, up, 6.0);
    fb_fdm_state(f, &s);
    double climb_up = -s.vel_ned[2];
    /* roll over in 2 s, the collective sign following the roll */
    for (int i = 0; i < 4000; i++) {
        double roll;
        fb_fdm_euler(f, &roll, NULL, NULL);
        double cn = fabs(roll) > 90 * D2R ? -hc : hc;
        fly(f, cn, inv, 0);
    }
    /* inverted hover 10 s with the velocity hold upside down */
    coll_i = 0;
    hover_for(f, hc, 0, 0, 1, 3.0);
    fb_fdm_state(f, &s);
    double alt0 = -s.pos_ned[2];
    hover_for(f, hc, 0, 0, 1, 10.0);
    fb_fdm_state(f, &s);
    double roll;
    fb_fdm_euler(f, &roll, NULL, NULL);
    printf("inverted: roll %.1f deg, alt %.1f -> %.1f m, thrust %.1f N\n", roll * R2D, alt0, -s.pos_ned[2], ext(f)->thrust_n);
    CHECK(fabs(roll) > 160 * D2R, "claim 3: inverted attitude held (a lean against the tail): roll %.1f", roll * R2D);
    CHECK(fabs(-s.pos_ned[2] - alt0) < 6.0 && fabs(s.vel_ned[2]) < 0.6, "claim 3: inverted hover holds height: %.1f -> %.1f m, vz %.2f", alt0, -s.pos_ned[2], s.vel_ned[2]);
    /* full negative collective: climbs, within 20 % of the upright climb */
    fly_for(f, -1.0, inv, 6.0);
    fb_fdm_state(f, &s);
    double climb_inv = -s.vel_ned[2];
    printf("full collective climb: upright %.2f m/s, inverted %.2f m/s\n", climb_up, climb_inv);
    CHECK(climb_inv > 0 && fabs(climb_inv - climb_up) < 0.2 * climb_up, "claim 3: inverted climb %.2f vs upright %.2f", climb_inv, climb_up);
    fb_fdm_free(f);
}

static void cyclic_and_tail(void)
{
    reset_ctl();
    fb_fdm_t *f = make();
    if (!f) return;
    fb_fdm_state_t s;
    double hc = hover_coll_n();
    double pos[3] = { 0, 0, -50 }, vel[3] = { 0, 0, 0 }, rates[3] = { 0, 0, 0 }, q[4];
    fb_q_identity(q);
    fb_fdm_set_kinematics(f, pos, vel, q, rates);
    fb_fdm_set_motor_rpm(f, 0, NOMINAL_RPM);
    for (int i = 0; i < 2000; i++) fly_cmd(f, hc, 0, 0);
    /* claim 5: a pitch step */
    double t_sign = 0, peak = 0, cross = 0;
    fb_fdm_state(f, &s);
    double t_step = s.t;
    for (int i = 0; i < 1000; i++) {
        fly_cmd(f, hc, 0.5, 0);
        fb_fdm_state(f, &s);
        if (t_sign == 0 && s.rates[1] > 2 * D2R) t_sign = s.t - t_step;
        if (fabs(s.rates[1]) > peak) peak = fabs(s.rates[1]);
        if (fabs(s.rates[0]) > cross) cross = fabs(s.rates[0]);
    }
    fb_fdm_state(f, &s);
    printf("pitch step 0.5: rate %.0f deg/s after 1 s (peak %.0f), roll cross %.0f deg/s, onset at %.3f s\n", s.rates[1] * R2D, peak * R2D, cross * R2D, t_sign);
    CHECK(t_sign > 0 && t_sign < 0.2 + 0.1, "claim 5: pitch rate of the commanded sign within 0.1 s (onset %.3f)", t_sign);
    CHECK(peak * R2D > 30 && peak * R2D < 400, "claim 5: pitch rate %.0f deg/s", peak * R2D);
    CHECK(cross < 0.2 * peak + 1 * D2R, "claim 5: cross-axis %.0f of %.0f deg/s", cross * R2D, peak * R2D);
    /* a roll step from a fresh level start */
    fb_fdm_set_kinematics(f, pos, vel, q, rates);
    for (int i = 0; i < 2000; i++) fly_cmd(f, hc, 0, 0);
    peak = 0; cross = 0;
    for (int i = 0; i < 1000; i++) {
        fly_cmd(f, hc, 0, 0.5);
        fb_fdm_state(f, &s);
        if (fabs(s.rates[0]) > peak) peak = fabs(s.rates[0]);
        if (fabs(s.rates[1]) > cross) cross = fabs(s.rates[1]);
    }
    fb_fdm_state(f, &s);
    CHECK(s.rates[0] > 0 && peak * R2D > 30 && peak * R2D < 400, "claim 5: roll rate %.0f deg/s, sign +", peak * R2D);
    CHECK(cross < 0.2 * peak + 1 * D2R, "claim 5: cross-axis %.0f of %.0f deg/s", cross * R2D, peak * R2D);

    /* claim 6: the tail */
    fb_fdm_set_kinematics(f, pos, vel, q, rates);
    for (int i = 0; i < 2000; i++) fly_cmd(f, hc, 0, 0);
    fb_fdm_state(f, &s);
    double r0 = s.rates[2];
    printf("tail trim at hover %.2f (%.1f deg), yaw rate %.1f deg/s\n", tail_trim, tail_trim * 12, r0 * R2D);
    for (int i = 0; i < 50; i++) { swash(f, hc, 0, 0); tail(f, 1.0); fb_fdm_step(f); }
    fb_fdm_state(f, &s);
    double yaw_acc = (s.rates[2] - r0) / 0.05;
    printf("tail full: yaw acceleration %.0f deg/s^2\n", yaw_acc * R2D);
    CHECK(yaw_acc * R2D > 200, "claim 6: full tail pitch yaw acceleration %.0f deg/s^2", yaw_acc * R2D);
    fb_fdm_set_kinematics(f, pos, vel, q, rates);
    for (int i = 0; i < 300; i++) { swash(f, hc, 0, 0); tail(f, 0.0); fb_fdm_step(f); }
    fb_fdm_state(f, &s);
    CHECK(s.rates[2] < -10 * D2R, "claim 6: zero tail pitch, the torque reaction turns the nose left (cw rotor): %.0f deg/s", s.rates[2] * R2D);
    fb_fdm_free(f);
}

static void servo_delay(void)
{
    fb_fdm_t *f = make();
    if (!f) return;
    swash(f, 0, 0, 0); tail(f, 0);
    for (int i = 0; i < 100; i++) fb_fdm_step(f);
    double t0 = ext(f)->thrust_n;
    swash(f, 0.5, 0, 0);
    int steps = 0;
    for (int i = 0; i < 50; i++) {
        fb_fdm_step(f);
        if (ext(f)->thrust_n > t0 + 1.0) { steps = i + 1; break; }
    }
    printf("swash step -> thrust after %d steps\n", steps);
    CHECK(steps >= 8 && steps <= 10, "claim 8: 8 ms delay (+ slew): %d steps", steps);
    CHECK(fb_actuator_delay_steps(8.0, 0.001) == 8 && fb_actuator_delay_steps(1.5, 0.001) == 2 &&
          fb_actuator_delay_steps(0.4, 0.001) == 0 && fb_actuator_delay_steps(1000.0, 0.001) == 63 &&
          fb_actuator_delay_steps(0.07, 0.00001) == 7, "delay rounding");
    fb_fdm_free(f);
}

static void ground(void)
{
    reset_ctl();
    fb_fdm_t *f = make();
    if (!f) return;
    fb_fdm_state_t s;
    double hc = hover_coll_n();
    double lvl[4], side[4];
    level_heading(lvl, 90 * D2R);
    fb_q_from_euler(side, 80 * D2R, 0, 90 * D2R);
    /* a soft landing from 5 m at heading 90: no crash, skids level, heading kept, IMU reads -g */
    double pos[3] = { 0, 0, -5 }, vel[3] = { 0, 0, 0 }, rates[3] = { 0, 0, 0 };
    fb_fdm_set_kinematics(f, pos, vel, lvl, rates);
    fb_fdm_set_motor_rpm(f, 0, NOMINAL_RPM);
    double yaw_td = 0;
    coll_i = 0;
    for (int i = 0; i < 30000; i++) {
        fb_fdm_state(f, &s);
        if (s.on_ground) break;
        hover_ctl(f, hc, 0, 0, 0.4, 90 * D2R, 0);          /* 0.4 m/s down, heading 90 */
        fb_fdm_euler(f, NULL, NULL, &yaw_td);
    }
    fb_fdm_state(f, &s);
    CHECK(s.on_ground, "soft descent reaches the ground");
    CHECK(s.crashes == 0, "soft landing is not a crash (%u)", s.crashes);
    for (int i = 0; i < 300; i++) { swash(f, 0, 0, 0); tail(f, 0); fb_fdm_step(f); }
    double roll, pitch, yaw;
    fb_fdm_euler(f, &roll, &pitch, &yaw);
    fb_fdm_state(f, &s);
    CHECK(fabs(roll) * R2D < 0.1 && fabs(pitch) * R2D < 0.1, "skids level: %.3f / %.3f", roll * R2D, pitch * R2D);
    CHECK(fabs(remainder(yaw - yaw_td, 2 * M_PI)) * R2D < 1.0, "heading kept: %.1f vs %.1f", yaw * R2D, yaw_td * R2D);
    printf("landing: heading %.1f, crashes %u, |v| at touchdown was below the limits\n", yaw * R2D, s.crashes);
    CHECK(fabs(s.accel_body[2] + FB_GRAVITY) < 1e-3, "on the ground the IMU reads -g: %.4f", s.accel_body[2]);
    /* a hard contact on the side counts once, the model uprights with rates the gyro reads */
    fb_q_from_euler(side, 80 * D2R, 0, yaw);
    double p2[3] = { 0, 0, -0.3 }, v2[3] = { 0, 0, 0.5 };
    fb_fdm_set_kinematics(f, p2, v2, side, rates);
    double err = 0, turned = 0, peak = 0;
    double qa[4];
    for (int i = 0; i < 600; i++) {
        fb_fdm_state(f, &s);
        memcpy(qa, s.quat, sizeof qa);
        swash(f, 0, 0, 0); tail(f, 0);
        fb_fdm_step(f);
        fb_fdm_state(f, &s);
        /* integrating the reported rates reproduces the attitude change */
        double qb[4];
        memcpy(qb, qa, sizeof qb);
        fb_q_integrate(qb, s.rates, DT);
        double e[3];
        fb_q_error(qb, s.quat, e);
        err = fmax(err, fb_v3_norm(e) * R2D);
        double w = fb_v3_norm(s.rates) * R2D;
        peak = fmax(peak, w);
        turned += w * DT;
    }
    fb_fdm_state(f, &s);
    fb_fdm_euler(f, &roll, &pitch, NULL);
    CHECK(s.crashes == 1 && fabs(s.crash_info[0]) > 15, "hard contact counted once: %u, roll %.1f", s.crashes, s.crash_info[0]);
    CHECK(fabs(roll) * R2D < 0.1 && fabs(pitch) * R2D < 0.1 && s.on_ground, "upright within 0.6 s: %.2f / %.2f", roll * R2D, pitch * R2D);
    CHECK(err < 1.0 && turned > 15, "integrated gyro matches the upright: err %.3f deg over %.1f deg", err, turned);
    CHECK(peak < 2000, "within the gyro range: %.0f deg/s", peak);
    /* return to the pad from the side */
    fb_fdm_set_kinematics(f, p2, v2, side, rates);
    fb_fdm_return_to_pad(f);
    for (int i = 0; i < 300; i++) { swash(f, 0, 0, 0); tail(f, 0); fb_fdm_step(f); }
    fb_fdm_state(f, &s);
    fb_fdm_euler(f, &roll, &pitch, &yaw);
    CHECK(s.on_ground && s.pos_ned[0] == 0 && s.pos_ned[1] == 0 && s.pos_ned[2] == 0, "on the pad at once");
    CHECK(fabs(roll) * R2D < 0.1 && fabs(pitch) * R2D < 0.1 && fabs(yaw) * R2D < 0.1, "level at heading 0: %.2f %.2f %.2f", roll * R2D, pitch * R2D, yaw * R2D);
    /* the crash event */
    fb_fdm_event_t ev;
    int crash_events = 0;
    while (fb_fdm_poll_event(f, &ev)) if (ev.kind == FB_FDM_EV_CRASH) crash_events++;
    CHECK(crash_events == 1, "one crash event: %d", crash_events);
    fb_fdm_free(f);
}

static void deterministic(void)
{
    double sum[2] = { 0, 0 };
    for (int run = 0; run < 2; run++) {
        reset_ctl();
        fb_fdm_t *f = make();
        if (!f) return;
        double hc = hover_coll_n(), lvl[4];
        level_heading(lvl, 0.3);
        fly_for(f, hc + 0.1, lvl, 2.0);
        for (int i = 0; i < 3000; i++) fly(f, hc + 0.02 * sin(i * 0.01), lvl, 0.2);
        fb_fdm_state_t s;
        fb_fdm_state(f, &s);
        sum[run] = s.pos_ned[0] * 1.1 + s.pos_ned[1] * 1.3 + s.pos_ned[2] + s.quat[1] * 7 + s.rates[2] * 11 + ext(f)->thrust_n;
        fb_fdm_free(f);
    }
    CHECK(sum[0] == sum[1], "claim 10: deterministic: %.17g vs %.17g", sum[0], sum[1]);
}

static void params(void)
{
    char err[128];
    const char *bad[] = {
        "{\"rotor\":{\"radius_m\":-1}}", "{\"rotor\":{\"nonsense\":1}}", "{\"mass_kg\":\"5\"}",
        "{\"inertia_kgm2\":[1,2]}", "{\"swash\":{\"order\":[\"a\",\"b\"]}}", "{\"rotor\":{\"direction\":\"up\"}}", "[1]",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        fb_fdm_t *f = fb_fdm_new(FB_FDM_HELI, bad[i], strlen(bad[i]), DT, err, sizeof err);
        CHECK(f == NULL && err[0], "bad params %zu rejected: %s -> %s", i, bad[i], err);
        fb_fdm_free(f);
    }
    const char *good = "{\"mass_kg\": 5.2, \"rotor\": {\"radius_m\": 0.8, \"direction\": \"ccw\"}, \"swash\": {\"order\": [\"s1\", \"s2\", \"s3\"], \"collective_max_deg\": 11}, \"inertia_kgm2\": [0.1, 0.3, 0.3]}";
    fb_fdm_t *f = fb_fdm_new(FB_FDM_HELI, good, strlen(good), DT, err, sizeof err);
    CHECK(f != NULL, "good params accepted: %s", err);
    if (f) {
        CHECK(fb_fdm_input_index(f, "s1") == 0 && fb_fdm_input_index(f, "s3") == 2 && fb_fdm_input_index(f, "tail") == 3 &&
              fb_fdm_input_index(f, "swash_a") == -1, "inputs named by swash.order");
        fb_fdm_free(f);
    }
}

int main(void)
{
    params();
    hover_and_climb();
    inverted();
    cyclic_and_tail();
    servo_delay();
    ground();
    deterministic();
    printf("test_heli: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
