/* FlightBench FDM: conventional single-rotor helicopter (docs/fdm.md §2).
 *
 * A main rotor disc with a first-order tip-path plane, blade-element thrust with momentum inflow, hub
 * moments from flapping stiffness and thrust offset, a tail rotor, quadratic fuselage drag, a rigid
 * body stopped by the ground. The head speed comes from the ESC model; the rotor's torque goes back
 * to it as the shaft load. */
#include "fdm_internal.h"
#include "fb/fdm_math.h"
#include "fb/rotor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { IN_REAR, IN_LEFT, IN_RIGHT, IN_TAIL, N_IN };
enum { DIR_CW, DIR_CCW };
static const char *const dir_names[] = { "cw", "ccw", NULL };

typedef struct {
    double mass_kg, inertia[3];
    fb_rotor_geom_t rotor;
    double lock_number, hub_stiffness, hub_height, nominal_rpm, rated_torque_nm, rotor_stall_deg, tail_stall_deg;
    int direction;
    fb_rotor_geom_t tail;
    double tail_ratio, tail_arm, tail_height, tail_pitch_max_deg, tail_power_fraction;
    double collective_max_deg, cyclic_max_deg;
    const char *swash_order[3];
    double drag_area[3];
    double servo_travel_us[2];       /* swash, tail */
    double servo_delay_ms, servo_speed_s60;
    double standin_tau_s;
} heli_params_t;

typedef struct {
    heli_params_t p;
    fb_actuator_t act[N_IN];
    double tpp[2];                   /* a1 (aft +), b1 (right +) */
    double lambda_i_main, lambda_i_tail;
    double rpm;                      /* head speed */
    bool esc_bound;
    double throttle;                 /* stand-in only */
    fb_fdm_heli_ext_t ext;
} heli_t;

static bool heli_crash_rule(double roll_deg, double pitch_deg, double sink, double drift)
{
    return fabs(roll_deg) > 15.0 || fabs(pitch_deg) > 15.0 || sink > 1.0 || drift > 2.0;
}

static void heli_defaults(heli_params_t *p)
{
    memset(p, 0, sizeof *p);
    p->mass_kg = 5.0;
    fb_v3_set(p->inertia, 0.12, 0.35, 0.30);
    p->rotor = (fb_rotor_geom_t){ .radius_m = 0.804, .chord_m = 0.060, .blades = 2, .lift_slope = 5.7,
                                  .cd0 = 0.011, .tip_loss = 0.97, .kappa = 1.15 };
    p->lock_number = 4.5;
    p->hub_stiffness = 120.0;
    p->hub_height = 0.22;
    p->nominal_rpm = 1800;
    p->rated_torque_nm = 32.0;
    p->rotor_stall_deg = 14;
    p->tail_stall_deg = 14;
    p->direction = DIR_CW;
    p->tail = (fb_rotor_geom_t){ .radius_m = 0.1475, .chord_m = 0.030, .blades = 2, .lift_slope = 5.7,
                                 .cd0 = 0.011, .tip_loss = 0.97, .kappa = 1.15 };
    p->tail_ratio = 5.0;
    p->tail_arm = 0.85;
    p->tail_height = 0.05;
    p->tail_pitch_max_deg = 12;
    p->tail_power_fraction = 0.08;
    p->collective_max_deg = 12;
    p->cyclic_max_deg = 12;
    p->swash_order[0] = "swash_a"; p->swash_order[1] = "swash_b"; p->swash_order[2] = "swash_c";
    fb_v3_set(p->drag_area, 0.045, 0.09, 0.07);
    p->servo_travel_us[0] = 500; p->servo_travel_us[1] = 270;
    p->servo_delay_ms = 8;
    p->servo_speed_s60 = 0.09;
    p->standin_tau_s = 3.0;
}

static void heli_reset(fb_fdm_t *f)
{
    heli_t *h = f->model;
    for (int i = 0; i < N_IN; i++) fb_actuator_reset(&h->act[i], 0);
    h->tpp[0] = h->tpp[1] = 0;
    h->lambda_i_main = h->lambda_i_tail = 0;
    if (!h->esc_bound) h->rpm = 0;
    memset(&h->ext, 0, sizeof h->ext);
    h->ext.rotor_rpm = h->rpm;
}

static void heli_on_contact(fb_fdm_t *f)
{
    heli_t *h = f->model;
    h->tpp[0] = h->tpp[1] = 0;
}

static void heli_set_motor_rpm(fb_fdm_t *f, int motor, double rpm)
{
    heli_t *h = f->model;
    if (motor != 0) return;
    if (rpm < 0) { h->esc_bound = false; return; }
    h->esc_bound = true;
    h->rpm = rpm;
}

static void heli_set_throttle(fb_fdm_t *f, int motor, double thr)
{
    heli_t *h = f->model;
    if (motor == 0) h->throttle = fb_clamp(thr, 0, 1);
}

static double heli_load(const fb_fdm_t *f, int motor)
{
    const heli_t *h = f->model;
    return motor == 0 ? h->ext.load : 0;
}

static const void *heli_ext(const fb_fdm_t *f, size_t *size)
{
    const heli_t *h = f->model;
    if (size) *size = sizeof h->ext;
    return &h->ext;
}

static void heli_destroy(fb_fdm_t *f)
{
    heli_t *h = f->model;
    if (!h) return;
    for (int i = 0; i < 3; i++) free((void *)h->p.swash_order[i]);
    free(h);
    f->model = NULL;
}

static void heli_step(fb_fdm_t *f)
{
    heli_t *h = f->model;
    const heli_params_t *p = &h->p;
    const double dt = f->dt;
    fb_body_t *b = &f->body;

    /* actuators */
    double s[N_IN];
    for (int i = 0; i < N_IN; i++) {
        double travel = i == IN_TAIL ? p->servo_travel_us[1] : p->servo_travel_us[0];
        s[i] = fb_actuator_step(&h->act[i], fb_us_norm(f->input_us[i], 1500.0, travel));
    }
    double coll = (s[IN_REAR] + s[IN_LEFT] + s[IN_RIGHT]) / 3.0 * p->collective_max_deg;
    double cyc_pitch = ((s[IN_LEFT] + s[IN_RIGHT]) / 2.0 - s[IN_REAR]) * (2.0 / 3.0) * p->cyclic_max_deg;
    double cyc_roll = (s[IN_LEFT] - s[IN_RIGHT]) / (2.0 * sin(60.0 * FB_DEG)) * p->cyclic_max_deg;
    double tail_pitch = s[IN_TAIL] * p->tail_pitch_max_deg;
    h->ext.collective_deg = coll;
    h->ext.cyclic_deg[0] = cyc_pitch; h->ext.cyclic_deg[1] = cyc_roll;
    h->ext.tail_pitch_deg = tail_pitch;

    /* head speed */
    if (!h->esc_bound) {
        double target = h->throttle * p->nominal_rpm * 1.1;
        h->rpm += (target - h->rpm) * (dt / fmax(p->standin_tau_s, dt));
    }
    double omega = h->rpm * 2.0 * M_PI / 60.0;
    h->ext.rotor_rpm = h->rpm;

    /* air velocity in the body frame */
    double v_rel[3], v_a[3];
    for (int i = 0; i < 3; i++) v_rel[i] = b->vel[i] - f->wind[i];
    fb_q_rotate_inv(b->quat, v_rel, v_a);

    /* tip-path plane: first-order lag towards the cyclic, lagging the body rates */
    double tau_f = p->lock_number > 0 && omega > 1e-3 ? 16.0 / (p->lock_number * omega) : 0.02;
    if (tau_f < dt) tau_f = dt;
    double a1_cmd = cyc_pitch * FB_DEG, b1_cmd = cyc_roll * FB_DEG;
    if (omega > 1e-3 && !b->on_ground) {
        h->tpp[0] += ((a1_cmd - h->tpp[0]) / tau_f - b->rates[1]) * dt;
        h->tpp[1] += ((b1_cmd - h->tpp[1]) / tau_f - b->rates[0]) * dt;
    } else {
        h->tpp[0] += (a1_cmd - h->tpp[0]) * (dt / tau_f);
        h->tpp[1] += (b1_cmd - h->tpp[1]) * (dt / tau_f);
    }
    double a1 = fb_clamp(h->tpp[0], -0.35, 0.35), b1 = fb_clamp(h->tpp[1], -0.35, 0.35);
    double n[3] = { -sin(a1), sin(b1), -cos(a1) * cos(b1) };       /* the TPP normal, thrust direction */
    double nn = fb_v3_norm(n);
    for (int i = 0; i < 3; i++) n[i] /= nn;

    /* main rotor: flow through the disc and in the plane */
    double vn = fb_v3_dot(v_a, n);                                /* along the thrust: climbing > 0 */
    double v_p[3];
    for (int i = 0; i < 3; i++) v_p[i] = v_a[i] - vn * n[i];
    double vp = fb_v3_norm(v_p);
    double vt = omega * p->rotor.radius_m;
    double lambda_c = vt > 1e-6 ? vn / vt : 0, mu = vt > 1e-6 ? vp / vt : 0;
    fb_rotor_out_t ro;
    fb_rotor_solve(&p->rotor, f->rho, omega, coll * FB_DEG, lambda_c, mu, &h->lambda_i_main, &ro);
    double T = ro.thrust_n, Q = ro.torque_nm;

    double force[3] = { 0, 0, 0 }, moment[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) force[i] += T * n[i];
    if (vp > 1e-6) for (int i = 0; i < 3; i++) force[i] -= ro.h_force_n * v_p[i] / vp;
    double K = p->hub_stiffness + T * p->hub_height;
    moment[0] += K * b1;
    moment[1] += K * a1;
    double dir = p->direction == DIR_CW ? 1.0 : -1.0;
    moment[2] += -dir * Q;                                         /* torque reaction */

    /* tail rotor: thrust along -y at (-arm, 0, -height); positive pitch = nose right */
    double omega_t = omega * p->tail_ratio;
    double vtt = omega_t * p->tail.radius_m;
    double lambda_ct = vtt > 1e-6 ? -v_a[1] / vtt : 0;
    double mu_t = vtt > 1e-6 ? hypot(v_a[0], v_a[2]) / vtt : 0;
    fb_rotor_out_t to;
    fb_rotor_solve(&p->tail, f->rho, omega_t, tail_pitch * FB_DEG, lambda_ct, mu_t, &h->lambda_i_tail, &to);
    double Tt = to.thrust_n;
    force[1] += -Tt;
    moment[0] += -p->tail_height * Tt;
    moment[2] += p->tail_arm * Tt;

    /* fuselage drag */
    for (int i = 0; i < 3; i++) force[i] += -0.5 * f->rho * fabs(v_a[i]) * v_a[i] * p->drag_area[i];

    h->ext.tpp_rad[0] = a1; h->ext.tpp_rad[1] = b1;
    h->ext.thrust_n = T; h->ext.torque_nm = Q; h->ext.tail_thrust_n = Tt;
    h->ext.load = p->rated_torque_nm > 0 ? fb_clamp(fabs(Q) * (1.0 + p->tail_power_fraction) / p->rated_torque_nm, 0, 1.5) : 0;

    if (fb_body_hold_step(b)) return;
    if (fb_body_step_stop(b, force, moment, p->mass_kg, p->inertia)) heli_on_contact(f);
}

static const fb_fdm_ops_t heli_ops = {
    .reset = heli_reset, .step = heli_step, .set_motor_rpm = heli_set_motor_rpm,
    .set_throttle = heli_set_throttle, .load = heli_load, .ext = heli_ext, .launch = NULL,
    .on_contact = heli_on_contact, .destroy = heli_destroy,
};

int fb_heli_create(fb_fdm_t *f, const char *json, size_t len, char *err, size_t errlen)
{
    heli_t *h = calloc(1, sizeof *h);
    if (!h) return -1;
    heli_defaults(&h->p);
    for (int i = 0; i < 3; i++) {                                  /* owned copies for the loader */
        const char *src = h->p.swash_order[i];
        char *c = malloc(strlen(src) + 1);
        if (!c) { free(h); return -1; }
        strcpy(c, src);
        h->p.swash_order[i] = c;
    }
    heli_params_t *p = &h->p;
    const fb_param_t table[] = {
        { "mass_kg", FB_P_NUM, &p->mass_kg, 0, 0.05, 500, NULL },
        { "inertia_kgm2", FB_P_VEC, p->inertia, 3, 1e-4, 100, NULL },
        { "rotor.radius_m", FB_P_NUM, &p->rotor.radius_m, 0, 0.05, 5, NULL },
        { "rotor.chord_m", FB_P_NUM, &p->rotor.chord_m, 0, 0.005, 0.5, NULL },
        { "rotor.blades", FB_P_NUM, &p->rotor.blades, 0, 2, 8, NULL },
        { "rotor.lift_slope", FB_P_NUM, &p->rotor.lift_slope, 0, 1, 8, NULL },
        { "rotor.cd0", FB_P_NUM, &p->rotor.cd0, 0, 0, 0.1, NULL },
        { "rotor.tip_loss", FB_P_NUM, &p->rotor.tip_loss, 0, 0.8, 1, NULL },
        { "rotor.kappa", FB_P_NUM, &p->rotor.kappa, 0, 1, 2, NULL },
        { "rotor.stall_deg", FB_P_NUM, &p->rotor_stall_deg, 0, 5, 30, NULL },
        { "rotor.lock_number", FB_P_NUM, &p->lock_number, 0, 1, 20, NULL },
        { "rotor.hub_stiffness_nm_per_rad", FB_P_NUM, &p->hub_stiffness, 0, 0, 5000, NULL },
        { "rotor.hub_height_m", FB_P_NUM, &p->hub_height, 0, 0, 2, NULL },
        { "rotor.nominal_rpm", FB_P_NUM, &p->nominal_rpm, 0, 100, 20000, NULL },
        { "rotor.rated_torque_nm", FB_P_NUM, &p->rated_torque_nm, 0, 0.01, 1000, NULL },
        { "rotor.direction", FB_P_ENUM, &p->direction, 0, 0, 0, dir_names },
        { "tail.radius_m", FB_P_NUM, &p->tail.radius_m, 0, 0.01, 2, NULL },
        { "tail.chord_m", FB_P_NUM, &p->tail.chord_m, 0, 0.002, 0.2, NULL },
        { "tail.blades", FB_P_NUM, &p->tail.blades, 0, 2, 8, NULL },
        { "tail.lift_slope", FB_P_NUM, &p->tail.lift_slope, 0, 1, 8, NULL },
        { "tail.cd0", FB_P_NUM, &p->tail.cd0, 0, 0, 0.1, NULL },
        { "tail.stall_deg", FB_P_NUM, &p->tail_stall_deg, 0, 5, 30, NULL },
        { "tail.ratio", FB_P_NUM, &p->tail_ratio, 0, 0.5, 20, NULL },
        { "tail.arm_m", FB_P_NUM, &p->tail_arm, 0, 0.05, 5, NULL },
        { "tail.height_m", FB_P_NUM, &p->tail_height, 0, -1, 1, NULL },
        { "tail.pitch_max_deg", FB_P_NUM, &p->tail_pitch_max_deg, 0, 1, 45, NULL },
        { "tail.power_fraction", FB_P_NUM, &p->tail_power_fraction, 0, 0, 1, NULL },
        { "swash.collective_max_deg", FB_P_NUM, &p->collective_max_deg, 0, 1, 30, NULL },
        { "swash.cyclic_max_deg", FB_P_NUM, &p->cyclic_max_deg, 0, 1, 30, NULL },
        { "swash.order", FB_P_NAMES, p->swash_order, 3, 0, 0, NULL },
        { "drag_area_m2", FB_P_VEC, p->drag_area, 3, 0, 10, NULL },
        { "servo.swash_travel_us", FB_P_NUM, &p->servo_travel_us[0], 0, 50, 1000, NULL },
        { "servo.tail_travel_us", FB_P_NUM, &p->servo_travel_us[1], 0, 50, 1000, NULL },
        { "servo.delay_ms", FB_P_NUM, &p->servo_delay_ms, 0, 0, 100, NULL },
        { "servo.speed_s60", FB_P_NUM, &p->servo_speed_s60, 0, 0, 2, NULL },
        { "standin_tau_s", FB_P_NUM, &p->standin_tau_s, 0, 0.01, 60, NULL },
    };
    f->model = h;
    f->ops = &heli_ops;
    if (fb_params_load(table, sizeof table / sizeof table[0], json, len, err, errlen)) return -1;
    p->rotor.stall_rad = p->rotor_stall_deg * FB_DEG;
    p->tail.stall_rad = p->tail_stall_deg * FB_DEG;
    f->n_inputs = N_IN;
    f->input_names[IN_REAR] = p->swash_order[0];
    f->input_names[IN_LEFT] = p->swash_order[1];
    f->input_names[IN_RIGHT] = p->swash_order[2];
    f->input_names[IN_TAIL] = "tail";
    for (int i = 0; i < N_IN; i++) {
        double full = i == IN_TAIL ? p->tail_pitch_max_deg : p->collective_max_deg;
        fb_actuator_init(&h->act[i], f->dt, p->servo_delay_ms, p->servo_speed_s60, full);
    }
    fb_body_init(&f->body, f->dt, heli_crash_rule);
    return 0;
}
