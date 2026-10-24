/* FlightBench FDM: conventional fixed wing (docs/fdm.md §3).
 *
 * Linear-coefficient aerodynamics in the wind/body axes with a stall blend, control and rate
 * derivatives, a fixed-pitch propeller on the ESC's rpm, wind, the shared rigid body, wheels that roll
 * forward only, the holder and the rail/hand launch (constraints, not aerodynamics). */
#include "fdm_internal.h"
#include "fb/fdm_math.h"
#include "fb/plane.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { IN_AIL_L, IN_AIL_R, IN_ELEV, IN_RUDDER, N_IN };
static const char *const in_names[N_IN] = { "aileron_l", "aileron_r", "elevator", "rudder" };

#define V_AERO_MIN 0.5            /* m/s: below it no aerodynamic force */
#define GROUND_TAU 0.1            /* s: the wheels bring the attitude to the ground rule */
#define GROUND_RATE_MAX (360.0 * FB_DEG)
#define GROUND_PITCH_FREE 5.0     /* m/s: below it the nose is held level */
#define GROUND_PITCH_MAX (15.0 * FB_DEG)   /* rotation on the wheels: tail strike beyond */
#define MIX_WARN_LEVEL 0.2
#define MIX_WARN_S 0.3
#define LAUNCH_ALIGN_S 1.0
#define LAUNCH_ALIGN_TOL (0.5 * FB_DEG)

typedef enum { LAUNCH_NONE, LAUNCH_ALIGN, LAUNCH_RAMP } launch_phase_t;

typedef struct {
    fb_plane_params_t p;
    fb_actuator_t act[N_IN];
    double rpm, throttle;
    bool esc_bound;
    double mix_s;
    bool mix_warned;
    launch_phase_t launch;
    double ground_q;                 /* the nose's pitch rate on the wheels */
    double launch_a, launch_speed, launch_heading, launch_pitch;
    int launch_steps, launch_i;
    fb_fdm_plane_ext_t ext;
} plane_t;

static bool plane_crash_rule(double roll_deg, double pitch_deg, double sink, double drift)
{
    (void)drift;
    return fabs(roll_deg) > 30.0 || pitch_deg < -20.0 || sink > 2.5;
}

static void plane_defaults(fb_plane_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->mass_kg = 2.0;
    fb_v3_set(p->inertia, 0.12, 0.09, 0.19);
    p->wing_area = 0.45; p->span = 1.88; p->chord = 0.24;
    p->lift0 = 0.30; p->lift_alpha = 5.0; p->lift_q = 3.0; p->lift_elevator = -0.20;
    p->alpha_stall = 15 * FB_DEG; p->stall_sharpness = 40;
    p->drag0 = 0.045; p->drag_elevator = 0.02; p->oswald = 0.85;
    p->side_beta = -0.6; p->side_p = 0; p->side_r = 0; p->side_aileron = 0; p->side_rudder = -0.15;
    p->roll_beta = -0.08; p->roll_p = -0.45; p->roll_r = 0.12; p->roll_aileron = 0.18; p->roll_rudder = 0.01;
    p->pitch0 = 0.04; p->pitch_alpha = -0.8; p->pitch_q = -12; p->pitch_elevator = 1.1;
    p->yaw_beta = 0.09; p->yaw_p = -0.03; p->yaw_r = -0.12; p->yaw_aileron = -0.01; p->yaw_rudder = 0.07;
    fb_v3_set(p->cp_offset, -0.02, 0, -0.04);
    p->prop_diameter_in = 11; p->prop_pitch_in = 7; p->prop_ct = 0.10; p->prop_kt = 0;
    fb_v3_set(p->thrust_offset, 0.3, 0, 0);
    p->prop_torque_factor = 0; p->rpm_max = 14800; p->standin_tau_s = 0.05;
    p->surface_deflection_deg = 20; p->servo_delay_ms = 10; p->servo_speed_s60 = 0.1;
    p->servo_centre_us = 1500; p->servo_travel_us = 500;
    p->ground_friction = 0.3; p->gear_height_m = 0.1; p->gear_arm_m = 0.06;
}

/* ---- coefficients (public) ----------------------------------------------------------------- */

double fb_plane_cl(const fb_plane_params_t *p, double alpha)
{
    double k = p->stall_sharpness, a0 = p->alpha_stall;
    double e1 = exp(-k * (alpha - a0)), e2 = exp(k * (alpha + a0));
    double s = (1 + e1 + e2) / ((1 + e1) * (1 + e2));
    double linear = p->lift0 + p->lift_alpha * alpha;
    double sa = sin(alpha);
    double plate = 2 * (alpha >= 0 ? 1 : -1) * sa * sa * cos(alpha);
    return (1 - s) * linear + s * plate;
}

double fb_plane_cd(const fb_plane_params_t *p, double alpha)
{
    double ar = p->span * p->span / p->wing_area;
    double cl_lin = p->lift0 + p->lift_alpha * alpha;
    return p->drag0 + cl_lin * cl_lin / (M_PI * p->oswald * ar);
}

void fb_plane_aero(const fb_plane_params_t *p, double rho, double V, double alpha, double beta,
                   const double w[3], double da, double de, double dr, double F[3], double M[3])
{
    memset(F, 0, 3 * sizeof *F);
    memset(M, 0, 3 * sizeof *M);
    if (V < V_AERO_MIN) return;
    double S = p->wing_area, b = p->span, c = p->chord;
    double qS = 0.5 * rho * V * V * S;
    double pb = b * w[0] / (2 * V), qc = c * w[1] / (2 * V), rb = b * w[2] / (2 * V);
    double CL = fb_plane_cl(p, alpha) + p->lift_q * qc + p->lift_elevator * de;
    double CD = fb_plane_cd(p, alpha) + p->drag_elevator * fabs(de);
    double CY = p->side_beta * beta + p->side_p * pb + p->side_r * rb + p->side_aileron * da + p->side_rudder * dr;
    double ca = cos(alpha), sa = sin(alpha);
    F[0] = qS * (-CD * ca + CL * sa);
    F[1] = qS * CY;
    F[2] = qS * (-CD * sa - CL * ca);
    double Cl = p->roll_beta * beta + p->roll_p * pb + p->roll_r * rb + p->roll_aileron * da + p->roll_rudder * dr;
    double Cm = p->pitch0 + p->pitch_alpha * alpha + p->pitch_q * qc + p->pitch_elevator * de;
    double Cn = p->yaw_beta * beta + p->yaw_p * pb + p->yaw_r * rb + p->yaw_aileron * da + p->yaw_rudder * dr;
    M[0] = qS * b * Cl;
    M[1] = qS * c * Cm;
    M[2] = qS * b * Cn;
    double rxf[3];
    fb_v3_cross(rxf, p->cp_offset, F);
    fb_v3_add(M, M, rxf);
}

double fb_plane_thrust(const fb_plane_params_t *p, double rho, double rpm, double V)
{
    if (rpm <= 0) return 0;
    double n = rpm / 60.0;
    double D = p->prop_diameter_in * 0.0254, pitch = p->prop_pitch_in * 0.0254;
    double v_pitch = pitch * n;
    double rolloff = v_pitch > 1e-6 ? 1.0 - V / v_pitch : 0;
    if (rolloff < 0) rolloff = 0;
    double t_static = p->prop_kt > 0 ? p->prop_kt * rpm * rpm : p->prop_ct * rho * n * n * D * D * D * D;
    return t_static * rolloff;
}

bool fb_launch_weak(double speed_kmh, double duration_s, double pitch_deg, double *a_mss, double *imu_x_mss)
{
    double a = duration_s > 0 ? speed_kmh / 3.6 / duration_s : 0;
    if (a_mss) *a_mss = a;
    if (imu_x_mss) *imu_x_mss = a + FB_GRAVITY * sin(pitch_deg * FB_DEG);
    return a < FB_LAUNCH_ACCEL_MARGIN * FB_INAV_LAUNCH_ACCEL_MSS;
}

/* ---- the model ----------------------------------------------------------------------------- */

static void plane_reset(fb_fdm_t *f)
{
    plane_t *m = f->model;
    for (int i = 0; i < N_IN; i++) fb_actuator_reset(&m->act[i], 0);
    if (!m->esc_bound) m->rpm = 0;
    m->mix_s = 0; m->mix_warned = false;
    m->launch = LAUNCH_NONE;
    memset(&m->ext, 0, sizeof m->ext);
    m->ext.motor_rpm = m->rpm;
}

static void plane_on_contact(fb_fdm_t *f) { plane_t *m = f->model; m->launch = LAUNCH_NONE; m->ground_q = 0; }
static void m_pitch_rate(fb_fdm_t *f, double q) { ((plane_t *)f->model)->ground_q = q; }

static void plane_set_motor_rpm(fb_fdm_t *f, int motor, double rpm)
{
    plane_t *m = f->model;
    if (motor != 0) return;
    if (rpm < 0) { m->esc_bound = false; return; }
    m->esc_bound = true;
    m->rpm = rpm;
}
static void plane_set_throttle(fb_fdm_t *f, int motor, double thr)
{
    plane_t *m = f->model;
    if (motor == 0) m->throttle = fb_clamp(thr, 0, 1);
}
static double plane_load(const fb_fdm_t *f, int motor)
{
    const plane_t *m = f->model;
    if (motor != 0) return 0;
    double t_static = fb_plane_thrust(&m->p, f->rho, m->p.rpm_max, 0);
    return t_static > 0 ? fb_clamp(m->ext.thrust_n / t_static, 0, 1) : 0;
}
static const void *plane_ext(const fb_fdm_t *f, size_t *size)
{
    const plane_t *m = f->model;
    if (size) *size = sizeof m->ext;
    return &m->ext;
}
static void plane_destroy(fb_fdm_t *f) { free(f->model); f->model = NULL; }

static int plane_launch(fb_fdm_t *f, double speed, double duration, double heading, double pitch)
{
    plane_t *m = f->model;
    if (!(speed > 0 && speed <= 100 && duration > 0 && duration <= 10 && fabs(pitch) <= 60 * FB_DEG)) return -1;
    if (isnan(heading)) fb_body_euler(&f->body, NULL, NULL, &heading);
    m->launch_speed = speed;
    m->launch_pitch = pitch;
    m->launch_heading = heading;
    m->launch_steps = (int)lround(duration / f->dt);
    if (m->launch_steps < 1) m->launch_steps = 1;
    m->launch_a = speed / (m->launch_steps * f->dt);
    m->launch_i = 0;
    double q[4], e[3];
    fb_q_from_euler(q, 0, pitch, heading);
    fb_q_error(f->body.quat, q, e);
    fb_body_hold_quat(&f->body, q, fb_v3_norm(e) > LAUNCH_ALIGN_TOL ? LAUNCH_ALIGN_S : 0);
    f->body.posed = f->body.hold_left <= 0;        /* no pose event for a launch's own align */
    m->launch = LAUNCH_ALIGN;
    return 0;
}

static void air_data(const fb_fdm_t *f, double v_a[3], double *V, double *alpha, double *beta)
{
    double rel[3];
    for (int i = 0; i < 3; i++) rel[i] = f->body.vel[i] - f->wind[i];
    fb_q_rotate_inv(f->body.quat, rel, v_a);
    *V = fb_v3_norm(v_a);
    *alpha = atan2(v_a[2], v_a[0]);
    *beta = *V > 1e-9 ? asin(fb_clamp(v_a[1] / *V, -1, 1)) : 0;
}

static void launch_ramp(fb_fdm_t *f, plane_t *m)
{
    fb_body_t *b = &f->body;
    double x[3] = { 1, 0, 0 }, d[3];
    fb_q_rotate(b->quat, x, d);
    m->launch_i++;
    double v = m->launch_a * m->launch_i * f->dt;
    double a_ned[3];
    for (int i = 0; i < 3; i++) { b->vel[i] = v * d[i]; b->pos[i] += b->vel[i] * f->dt; a_ned[i] = m->launch_a * d[i]; }
    memset(b->rates, 0, sizeof b->rates);
    double sf[3] = { a_ned[0], a_ned[1], a_ned[2] - FB_GRAVITY };
    fb_q_rotate_inv(b->quat, sf, b->accel_body);
    b->on_ground = false;
    b->held = false;
    bool grounded = b->pos[2] > 0 && d[2] > 0;      /* a rail pointing down meets the ground */
    if (grounded) b->pos[2] = 0;
    if (m->launch_i >= m->launch_steps || grounded) {
        m->launch = LAUNCH_NONE;
        fb_fdm_push_event(f, FB_FDM_EV_LAUNCH_RELEASE, v, -b->pos[2], 0, 0);
        if (b->pos[2] >= 0) { b->pos[2] = 0; b->on_ground = true; }
    }
    b->t += f->dt;
}

/* The wheels' rule: wings level and the heading kept exactly; below GROUND_PITCH_FREE the nose is held
 * level; above it the pitch moment rotates the nose on the main wheels against the weight on the nose
 * wheel (W · gear_arm), between level and GROUND_PITCH_MAX. The attitude is built from the angles and
 * the reported rates are the rotation that happened, so the IMU stays consistent. */
static void ground_attitude(fb_fdm_t *f, const fb_plane_params_t *p, const double M[3])
{
    fb_body_t *b = &f->body;
    const double dt = f->dt;
    double roll, pitch, yaw, q_old[4], q_new[4], e[3];
    fb_body_euler(b, &roll, &pitch, &yaw);
    memcpy(q_old, b->quat, sizeof q_old);
    double speed = fb_v3_norm(b->vel);
    double max_step = GROUND_RATE_MAX * dt;
    double roll_new = roll - fb_clamp(roll * fmin(1.0, dt / GROUND_TAU), -max_step, max_step);
    double pitch_new;
    if (speed < GROUND_PITCH_FREE) {
        pitch_new = pitch - fb_clamp(pitch * fmin(1.0, dt / GROUND_TAU), -max_step, max_step);
        m_pitch_rate(f, 0);
    } else {
        double hold_down = p->mass_kg * FB_GRAVITY * p->gear_arm_m * cos(pitch);
        double q_dot = p->inertia[1] > 0 ? (M[1] - hold_down) / p->inertia[1] : 0;
        double qn = ((plane_t *)f->model)->ground_q + q_dot * dt;
        if (pitch <= 0 && qn < 0) { qn = 0; pitch = 0; }
        if (pitch >= GROUND_PITCH_MAX && qn > 0) { qn = 0; pitch = GROUND_PITCH_MAX; }
        pitch_new = fb_clamp(pitch + fb_clamp(qn * dt, -max_step, max_step), 0, GROUND_PITCH_MAX);
        m_pitch_rate(f, qn);
    }
    fb_q_from_euler(q_new, roll_new, pitch_new, yaw);
    fb_q_error(q_old, q_new, e);
    for (int i = 0; i < 3; i++) b->rates[i] = e[i] / dt;
    memcpy(b->quat, q_new, sizeof q_new);
}

static void plane_step(fb_fdm_t *f)
{
    plane_t *m = f->model;
    const fb_plane_params_t *p = &m->p;
    fb_body_t *b = &f->body;
    const double dt = f->dt;

    /* surfaces */
    double s[N_IN];
    for (int i = 0; i < N_IN; i++)
        s[i] = fb_actuator_step(&m->act[i], fb_clamp(fb_us_norm(f->input_us[i], p->servo_centre_us, p->servo_travel_us), -1, 1));
    double l = s[IN_AIL_L], r = s[IN_AIL_R];
    if (l * r > 0 && fmin(fabs(l), fabs(r)) > MIX_WARN_LEVEL) {
        m->mix_s += dt;
        if (m->mix_s >= MIX_WARN_S && !m->mix_warned) { m->mix_warned = true; fb_fdm_push_event(f, FB_FDM_EV_MIS_MIX, l, r, 0, 0); }
    } else { m->mix_s = 0; m->mix_warned = false; }
    double da = 0.5 * (l - r), de = s[IN_ELEV], dr = s[IN_RUDDER];
    memcpy(m->ext.surface, s, sizeof m->ext.surface);
    m->ext.aileron = da; m->ext.elevator = de; m->ext.rudder = dr;

    /* motor */
    if (!m->esc_bound) m->rpm += (m->throttle * p->rpm_max - m->rpm) * dt / (p->standin_tau_s + dt);
    m->ext.motor_rpm = m->rpm;

    double v_a[3], V, alpha, beta;
    air_data(f, v_a, &V, &alpha, &beta);
    m->ext.airspeed = V; m->ext.alpha = alpha; m->ext.beta = beta;
    m->ext.thrust_n = fb_plane_thrust(p, f->rho, m->rpm, v_a[0]);

    /* the launch's phases and the holder */
    if (m->launch == LAUNCH_ALIGN) {
        if (!b->held) m->launch = LAUNCH_NONE;                  /* released by hand before the throw */
        else if (b->hold_left <= 0) {
            m->launch = LAUNCH_RAMP;
            b->held = false; b->posed = false;
            fb_fdm_push_event(f, FB_FDM_EV_LAUNCH_START, m->launch_a, m->launch_speed, m->launch_heading / FB_DEG, m->launch_pitch / FB_DEG);
        }
    }
    m->ext.launch = m->launch;
    if (m->launch == LAUNCH_RAMP) { launch_ramp(f, m); m->ext.launch = m->launch; return; }
    if (fb_body_hold_step(b)) return;
    if (b->settle_to_pad && b->settle_left > 0) {              /* a return to the pad: levelling there */
        memset(b->vel, 0, sizeof b->vel);
        b->pos[2] = 0; b->on_ground = true;
        fb_body_settle_step(b);
        fb_body_ground_reaction(b);
        b->t += dt;
        return;
    }

    /* forces and moments about the CG */
    double F[3], M[3];
    fb_plane_aero(p, f->rho, V, alpha, beta, b->rates, da, de, dr, F, M);
    double T[3] = { m->ext.thrust_n, 0, 0 }, txo[3];
    fb_v3_add(F, F, T);
    fb_v3_cross(txo, p->thrust_offset, T);
    fb_v3_add(M, M, txo);
    M[0] += -m->ext.thrust_n * p->prop_torque_factor;
    if (b->on_ground) {
        double vb[3];
        fb_q_rotate_inv(b->quat, b->vel, vb);
        F[0] += -p->ground_friction * vb[0] * p->mass_kg;
    }
    for (int i = 0; i < 3; i++) b->accel_body[i] = F[i] / p->mass_kg;

    /* attitude: the wheels' rule on the ground, the moments in the air */
    if (b->on_ground) ground_attitude(f, p, M);
    else fb_body_rotate(b, M, p->inertia);

    double a_ned[3];
    fb_q_rotate(b->quat, b->accel_body, a_ned);
    a_ned[2] += FB_GRAVITY;
    if (b->on_ground && a_ned[2] > 0) a_ned[2] = 0;             /* the ground carries the weight */
    double sf[3] = { a_ned[0], a_ned[1], a_ned[2] - FB_GRAVITY };
    fb_q_rotate_inv(b->quat, sf, b->accel_body);               /* what the IMU reads */
    for (int i = 0; i < 3; i++) b->vel[i] += a_ned[i] * dt;
    if (b->on_ground) {
        double vb[3];
        fb_q_rotate_inv(b->quat, b->vel, vb);
        vb[1] = 0;                                              /* no sideways rolling */
        if (vb[0] < 0) vb[0] = 0;                               /* no backward rolling */
        fb_q_rotate(b->quat, vb, b->vel);
        if (b->vel[2] > 0) b->vel[2] = 0;
    }
    for (int i = 0; i < 3; i++) b->pos[i] += b->vel[i] * dt;
    if (b->pos[2] >= 0) {
        if (!b->on_ground) { fb_body_contact(b, false); b->air_s = 0; m->ground_q = 0; }
        b->pos[2] = 0;
        if (b->vel[2] > 0) b->vel[2] = 0;
        b->on_ground = true;
    } else {
        b->on_ground = false;
        b->air_s += dt;
        if (b->air_s > 0.2) b->crash_armed = true;
    }
    b->t += dt;
}

static const fb_fdm_ops_t plane_ops = {
    .reset = plane_reset, .step = plane_step, .set_motor_rpm = plane_set_motor_rpm,
    .set_throttle = plane_set_throttle, .load = plane_load, .ext = plane_ext, .launch = plane_launch,
    .on_contact = plane_on_contact, .destroy = plane_destroy,
};

const fb_plane_params_t *fb_fdm_plane_params(const fb_fdm_t *f)
{
    return f && f->kind == FB_FDM_PLANE ? &((const plane_t *)f->model)->p : NULL;
}

int fb_plane_create(fb_fdm_t *f, const char *json, size_t len, char *err, size_t errlen)
{
    plane_t *m = calloc(1, sizeof *m);
    if (!m) return -1;
    plane_defaults(&m->p);
    fb_plane_params_t *p = &m->p;
    double alpha_stall_deg = p->alpha_stall / FB_DEG;
    const fb_param_t table[] = {
        { "mass_kg", FB_P_NUM, &p->mass_kg, 0, 0.05, 500, NULL },
        { "inertia_kgm2", FB_P_VEC, p->inertia, 3, 1e-4, 100, NULL },
        { "wing.area_m2", FB_P_NUM, &p->wing_area, 0, 0.01, 50, NULL },
        { "wing.span_m", FB_P_NUM, &p->span, 0, 0.1, 30, NULL },
        { "wing.chord_m", FB_P_NUM, &p->chord, 0, 0.01, 5, NULL },
        { "aero.lift0", FB_P_NUM, &p->lift0, 0, -2, 2, NULL },
        { "aero.lift_alpha", FB_P_NUM, &p->lift_alpha, 0, 0, 10, NULL },
        { "aero.lift_q", FB_P_NUM, &p->lift_q, 0, -50, 50, NULL },
        { "aero.lift_elevator", FB_P_NUM, &p->lift_elevator, 0, -5, 5, NULL },
        { "aero.alpha_stall_deg", FB_P_NUM, &alpha_stall_deg, 0, 3, 45, NULL },
        { "aero.stall_sharpness", FB_P_NUM, &p->stall_sharpness, 0, 1, 500, NULL },
        { "aero.drag0", FB_P_NUM, &p->drag0, 0, 0, 2, NULL },
        { "aero.drag_elevator", FB_P_NUM, &p->drag_elevator, 0, 0, 2, NULL },
        { "aero.oswald", FB_P_NUM, &p->oswald, 0, 0.1, 1.5, NULL },
        { "aero.side_beta", FB_P_NUM, &p->side_beta, 0, -10, 10, NULL },
        { "aero.side_p", FB_P_NUM, &p->side_p, 0, -10, 10, NULL },
        { "aero.side_r", FB_P_NUM, &p->side_r, 0, -10, 10, NULL },
        { "aero.side_aileron", FB_P_NUM, &p->side_aileron, 0, -10, 10, NULL },
        { "aero.side_rudder", FB_P_NUM, &p->side_rudder, 0, -10, 10, NULL },
        { "aero.roll_beta", FB_P_NUM, &p->roll_beta, 0, -10, 10, NULL },
        { "aero.roll_p", FB_P_NUM, &p->roll_p, 0, -50, 0, NULL },
        { "aero.roll_r", FB_P_NUM, &p->roll_r, 0, -10, 10, NULL },
        { "aero.roll_aileron", FB_P_NUM, &p->roll_aileron, 0, -10, 10, NULL },
        { "aero.roll_rudder", FB_P_NUM, &p->roll_rudder, 0, -10, 10, NULL },
        { "aero.pitch0", FB_P_NUM, &p->pitch0, 0, -2, 2, NULL },
        { "aero.pitch_alpha", FB_P_NUM, &p->pitch_alpha, 0, -20, 0, NULL },
        { "aero.pitch_q", FB_P_NUM, &p->pitch_q, 0, -200, 0, NULL },
        { "aero.pitch_elevator", FB_P_NUM, &p->pitch_elevator, 0, -10, 10, NULL },
        { "aero.yaw_beta", FB_P_NUM, &p->yaw_beta, 0, -10, 10, NULL },
        { "aero.yaw_p", FB_P_NUM, &p->yaw_p, 0, -10, 10, NULL },
        { "aero.yaw_r", FB_P_NUM, &p->yaw_r, 0, -50, 0, NULL },
        { "aero.yaw_aileron", FB_P_NUM, &p->yaw_aileron, 0, -10, 10, NULL },
        { "aero.yaw_rudder", FB_P_NUM, &p->yaw_rudder, 0, -10, 10, NULL },
        { "cp_offset_m", FB_P_VEC, p->cp_offset, 3, -5, 5, NULL },
        { "prop.diameter_in", FB_P_NUM, &p->prop_diameter_in, 0, 1, 60, NULL },
        { "prop.pitch_in", FB_P_NUM, &p->prop_pitch_in, 0, 0.5, 40, NULL },
        { "prop.ct", FB_P_NUM, &p->prop_ct, 0, 0, 1, NULL },
        { "prop.kt", FB_P_NUM, &p->prop_kt, 0, 0, 1, NULL },
        { "thrust_offset_m", FB_P_VEC, p->thrust_offset, 3, -5, 5, NULL },
        { "prop_torque_factor", FB_P_NUM, &p->prop_torque_factor, 0, -1, 1, NULL },
        { "rpm_max", FB_P_NUM, &p->rpm_max, 0, 100, 100000, NULL },
        { "standin_tau_s", FB_P_NUM, &p->standin_tau_s, 0, 0.001, 60, NULL },
        { "surface_deflection_deg", FB_P_NUM, &p->surface_deflection_deg, 0, 1, 60, NULL },
        { "servo.delay_ms", FB_P_NUM, &p->servo_delay_ms, 0, 0, 100, NULL },
        { "servo.speed_s60", FB_P_NUM, &p->servo_speed_s60, 0, 0, 2, NULL },
        { "servo.centre_us", FB_P_NUM, &p->servo_centre_us, 0, 800, 2200, NULL },
        { "servo.travel_us", FB_P_NUM, &p->servo_travel_us, 0, 50, 1000, NULL },
        { "ground_friction", FB_P_NUM, &p->ground_friction, 0, 0, 10, NULL },
        { "gear_height_m", FB_P_NUM, &p->gear_height_m, 0, 0, 2, NULL },
        { "gear_arm_m", FB_P_NUM, &p->gear_arm_m, 0, 0, 2, NULL },
    };
    f->model = m;
    f->ops = &plane_ops;
    if (fb_params_load(table, sizeof table / sizeof table[0], json, len, err, errlen)) return -1;
    p->alpha_stall = alpha_stall_deg * FB_DEG;
    f->n_inputs = N_IN;
    for (int i = 0; i < N_IN; i++) {
        f->input_names[i] = in_names[i];
        fb_actuator_init(&m->act[i], f->dt, p->servo_delay_ms, p->servo_speed_s60, p->surface_deflection_deg);
    }
    fb_body_init(&f->body, f->dt, plane_crash_rule);
    return 0;
}
