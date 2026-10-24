/* The fixed-wing FDM against the claims of docs/fdm.md §3.9, through the common interface. Run from fdm/
 * (the scenarios are read from ../models/trainer/env/). */
#include "fb/fdm.h"
#include "fb/fdm_math.h"
#include "fb/plane.h"
#include "fb/plane_scenario.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)
#define DT 0.001
#define G FB_GRAVITY

static const fb_fdm_plane_ext_t *ext(const fb_fdm_t *f) { return fb_fdm_state_ext(f, NULL); }
static uint16_t us(double v) { return (uint16_t)lrint(1500.0 + 500.0 * fb_clamp(v, -1, 1)); }
static void controls(fb_fdm_t *f, double ail, double elev, double rud)
{
    fb_fdm_set_input_us(f, 0, us(ail)); fb_fdm_set_input_us(f, 1, us(-ail));
    fb_fdm_set_input_us(f, 2, us(elev)); fb_fdm_set_input_us(f, 3, us(rud));
}
static fb_fdm_t *make(void)
{
    char err[128];
    fb_fdm_t *f = fb_fdm_new(FB_FDM_PLANE, NULL, 0, DT, err, sizeof err);
    if (!f) { printf("FAIL: fb_fdm_new(plane): %s\n", err); fails++; }
    return f;
}
static void set_flight(fb_fdm_t *f, double alt, double speed, double pitch)
{
    double pos[3] = { 0, 0, -alt }, vel[3] = { speed * cos(pitch), 0, -speed * sin(pitch) }, q[4], w[3] = { 0, 0, 0 };
    fb_q_from_euler(q, 0, pitch, 0);
    fb_fdm_set_kinematics(f, pos, vel, q, w);
}
static char *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, fp); fclose(fp);
    b[got] = 0; if (len) *len = got;
    return b;
}
static int load_scenario(fb_scenario_t *sc, const char *name)
{
    char path[256], err[128];
    snprintf(path, sizeof path, "../models/trainer/env/%s.json", name);
    size_t n; char *txt = read_file(path, &n);
    if (!txt) { printf("FAIL: cannot read %s\n", path); fails++; return -1; }
    int rc = fb_scenario_parse(sc, txt, n, err, sizeof err);
    free(txt);
    if (rc) { printf("FAIL: %s: %s\n", path, err); fails++; }
    return rc;
}

/* ---- analytic expectations from the same coefficient functions ---------------------------- */

static double trim_moment(const fb_plane_params_t *p, double alpha, double elev)
{
    double w[3] = { 0, 0, 0 }, F[3], M[3];
    fb_plane_aero(p, 1.225, 10.0, alpha, 0, w, 0, elev, 0, F, M);
    return M[1];
}
static double trim_alpha(const fb_plane_params_t *p, double elev)
{
    double lo = -0.3, hi = 0.5;
    for (int i = 0; i < 60; i++) { double mid = 0.5 * (lo + hi); if (trim_moment(p, mid, elev) > 0) lo = mid; else hi = mid; }
    return 0.5 * (lo + hi);
}
static void lift_drag(const fb_plane_params_t *p, double alpha, double elev, double *cl, double *cd)
{
    double w[3] = { 0, 0, 0 }, F[3], M[3], V = 10.0, qS = 0.5 * 1.225 * V * V * p->wing_area;
    fb_plane_aero(p, 1.225, V, alpha, 0, w, 0, elev, 0, F, M);
    *cl = (F[0] * sin(alpha) - F[2] * cos(alpha)) / qS;
    *cd = -(F[0] * cos(alpha) + F[2] * sin(alpha)) / qS;
}
typedef struct { double alpha, cl, cd, gamma, v, sink; } glide_t;
static glide_t analytic_glide(const fb_plane_params_t *p)
{
    glide_t g;
    g.alpha = trim_alpha(p, 0);
    lift_drag(p, g.alpha, 0, &g.cl, &g.cd);
    g.gamma = atan2(g.cd, g.cl);
    g.v = sqrt(2 * p->mass_kg * G * cos(g.gamma) / (1.225 * p->wing_area * g.cl));
    g.sink = g.v * sin(g.gamma);
    return g;
}
/* level flight at full throttle: T(V) = D(V) with L = W (alpha from CL, elevator ignored) */
static double analytic_level_speed(const fb_plane_params_t *p)
{
    double lo = 5, hi = 60;
    for (int i = 0; i < 60; i++) {
        double V = 0.5 * (lo + hi), qS = 0.5 * 1.225 * V * V * p->wing_area;
        double cl = p->mass_kg * G / qS, alpha = (cl - p->lift0) / p->lift_alpha;
        double D = qS * fb_plane_cd(p, alpha);
        double T = fb_plane_thrust(p, 1.225, p->rpm_max, V);
        if (T > D) lo = V; else hi = V;
    }
    return 0.5 * (lo + hi);
}

/* a pitch-attitude / climb-rate stand-in for the pilot: elevator from a pitch target */
static void hold_pitch(fb_fdm_t *f, double pitch_t, double ail, double rud)
{
    fb_fdm_state_t s; double roll, pitch;
    fb_fdm_state(f, &s); fb_fdm_euler(f, &roll, &pitch, NULL);
    double de = fb_clamp(2.5 * (pitch_t - pitch) - 0.3 * s.rates[1], -1, 1);
    double da = fb_clamp(ail - 1.5 * roll - 0.2 * s.rates[0], -1, 1);
    controls(f, da, de, rud);
}

/* ---- tests ------------------------------------------------------------------------------- */

static glide_t G0;

static void test_rest_and_params(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    fb_fdm_state_t s;
    controls(f, 0, 0, 0);
    for (int i = 0; i < 500; i++) fb_fdm_step(f);
    fb_fdm_state(f, &s);
    CHECK(s.on_ground && fabs(s.accel_body[2] + G) < 1e-6 && fabs(s.accel_body[0]) < 1e-6, "at rest the IMU reads (0, 0, -g): %.3f %.3f %.3f", s.accel_body[0], s.accel_body[1], s.accel_body[2]);
    CHECK(fb_fdm_input_index(f, "aileron_l") == 0 && fb_fdm_input_index(f, "rudder") == 3, "inputs by name");
    const fb_plane_params_t *p = fb_fdm_plane_params(f);
    double tw = fb_plane_thrust(p, 1.225, p->rpm_max, 0) / (p->mass_kg * G);
    printf("static thrust %.1f N, T/W %.2f\n", fb_plane_thrust(p, 1.225, p->rpm_max, 0), tw);
    CHECK(tw > 1.2, "claim 2: static T/W %.2f > 1.2", tw);
    fb_fdm_free(f);
    char err[128];
    const char *bad[] = { "{\"aero\":{\"nonsense\":1}}", "{\"wing\":{\"span_m\":-1}}", "{\"aero\":{\"pitch_alpha\":1}}" };
    for (size_t i = 0; i < 3; i++) {
        f = fb_fdm_new(FB_FDM_PLANE, bad[i], strlen(bad[i]), DT, err, sizeof err);
        CHECK(!f && err[0], "bad params %zu rejected: %s", i, err);
        fb_fdm_free(f);
    }
    const char *good = "{\"mass_kg\": 2.2, \"aero\": {\"lift0\": 0.25, \"alpha_stall_deg\": 14}, \"prop\": {\"kt\": 2e-7}}";
    f = fb_fdm_new(FB_FDM_PLANE, good, strlen(good), DT, err, sizeof err);
    CHECK(f && fabs(fb_fdm_plane_params(f)->alpha_stall - 14 * D2R) < 1e-9, "good params accepted: %s", err);
    fb_fdm_free(f);
}

static void test_glide(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    const fb_plane_params_t *p = fb_fdm_plane_params(f);
    G0 = analytic_glide(p);
    printf("glide analytic: alpha %.2f deg, CL %.3f, CD %.4f, L/D %.2f, V %.2f m/s, sink %.2f m/s\n", G0.alpha * R2D, G0.cl, G0.cd, G0.cl / G0.cd, G0.v, G0.sink);
    set_flight(f, 200, 12, 0);
    controls(f, 0, 0, 0);
    fb_fdm_state_t s;
    double v_sum = 0, sink_sum = 0; int n = 0;
    for (int i = 0; i < 90000; i++) {
        fb_fdm_step(f);
        if (i >= 70000) { fb_fdm_state(f, &s); v_sum += ext(f)->airspeed; sink_sum += s.vel_ned[2]; n++; }
    }
    double v = v_sum / n, sink = sink_sum / n;
    fb_fdm_state(f, &s);
    printf("glide sim (stick fixed, 70..90 s): V %.2f m/s, sink %.2f m/s, alt %.0f m\n", v, sink, -s.pos_ned[2]);
    CHECK(fabs(v / G0.v - 1) < 0.05 && fabs(sink / G0.sink - 1) < 0.05, "claim 1: trim glide within 5 %% of analytic");
    CHECK(!s.on_ground, "still airborne");
    fb_fdm_free(f);
}

static void test_level_full_throttle(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    const fb_plane_params_t *p = fb_fdm_plane_params(f);
    double va = analytic_level_speed(p);
    set_flight(f, 100, va, 0);
    fb_fdm_set_throttle(f, 0, 1.0);
    fb_fdm_state_t s;
    double pitch_t = 0, vz_i = 0;
    for (int i = 0; i < 60000; i++) {
        fb_fdm_state(f, &s);
        vz_i = fb_clamp(vz_i + 0.002 * s.vel_ned[2] * DT * 100, -0.1, 0.1);
        pitch_t = fb_clamp(0.03 * s.vel_ned[2] + vz_i, -0.2, 0.2);    /* sinking -> nose up */
        hold_pitch(f, pitch_t, 0, 0);
        fb_fdm_step(f);
    }
    fb_fdm_state(f, &s);
    printf("level full throttle: analytic %.2f m/s, sim %.2f m/s (%+.1f %%), climb %.2f m/s, thrust %.1f N\n", va, ext(f)->airspeed, 100 * (ext(f)->airspeed / va - 1), -s.vel_ned[2], ext(f)->thrust_n);
    CHECK(fabs(ext(f)->airspeed / va - 1) < 0.08 && fabs(s.vel_ned[2]) < 1.0, "claim 2: full-throttle level speed within 8 %% of analytic");
    fb_fdm_free(f);
}

static void test_steps(void)
{
    const char *name[3] = { "aileron", "elevator", "rudder" };
    for (int axis = 0; axis < 3; axis++) {
        fb_fdm_t *f = make(); if (!f) return;
        set_flight(f, 200, G0.v, -G0.gamma);
        controls(f, 0, 0, 0);
        for (int i = 0; i < 3000; i++) fb_fdm_step(f);
        fb_fdm_state_t s;
        double peak = 0, first = 0;
        for (int i = 0; i < 1000; i++) {
            controls(f, axis == 0 ? 0.3 : 0, axis == 1 ? 0.3 : 0, axis == 2 ? 0.3 : 0);
            fb_fdm_step(f);
            fb_fdm_state(f, &s);
            double w = s.rates[axis];
            if (first == 0 && fabs(w) > 2 * D2R) first = w;
            if (fabs(w) > fabs(peak)) peak = w;
        }
        printf("%s +0.3: rate peak %.0f deg/s (first %.0f)\n", name[axis], peak * R2D, first * R2D);
        CHECK(first > 0, "claim 3: %s step gives a positive rate", name[axis]);
        CHECK(fabs(peak) * R2D > 10 && fabs(peak) * R2D < 400, "claim 3: %s rate plausible: %.0f deg/s", name[axis], peak * R2D);
        fb_fdm_free(f);
    }
}

static void test_stall(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    const fb_plane_params_t *p = fb_fdm_plane_params(f);
    double past = p->alpha_stall + 0.15;
    double cl_lin = p->lift0 + p->lift_alpha * past, cl = fb_plane_cl(p, past);
    printf("stall: CL(alpha_stall + 0.15) %.3f vs linear %.3f\n", cl, cl_lin);
    CHECK(cl < 0.7 * cl_lin, "claim 4: lift collapses past alpha_stall");
    /* full up elevator from the glide for 3 s, then neutral: the nose drops and the speed comes back */
    set_flight(f, 300, G0.v, -G0.gamma);
    controls(f, 0, 0, 0);
    for (int i = 0; i < 2000; i++) fb_fdm_step(f);
    double alpha_max = 0, v_min = 1e9;
    for (int i = 0; i < 3000; i++) { controls(f, 0, 1.0, 0); fb_fdm_step(f); alpha_max = fmax(alpha_max, ext(f)->alpha); v_min = fmin(v_min, ext(f)->airspeed); }
    fb_fdm_state_t s;
    double q_min = 0;
    for (int i = 0; i < 10000; i++) { controls(f, 0, 0, 0); fb_fdm_step(f); fb_fdm_state(f, &s); q_min = fmin(q_min, s.rates[1]); }
    printf("stall: alpha max %.1f deg, V min %.1f m/s; after release pitch rate min %.0f deg/s, V %.1f m/s\n", alpha_max * R2D, v_min, q_min * R2D, ext(f)->airspeed);
    CHECK(alpha_max > p->alpha_stall, "full up elevator stalls the model: alpha %.1f deg", alpha_max * R2D);
    CHECK(q_min < -10 * D2R && ext(f)->airspeed > 0.8 * G0.v && !s.on_ground, "claim 4: stalled model pitches down and recovers");
    fb_fdm_free(f);
}

static void test_ground(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    const fb_plane_params_t *p = fb_fdm_plane_params(f);
    fb_scenario_t sc;
    if (load_scenario(&sc, "ground-takeoff")) return;
    double v_lo = sqrt(2 * p->mass_kg * G / (1.225 * p->wing_area * fb_plane_cl(p, 0)));
    fb_scenario_start(&sc, f);
    bool ready = false, off = false;
    double t_ready = 0, v_off = 0, x_off = 0, max_lat = 0;
    fb_fdm_state_t s; fb_sc_event_t ev;
    for (int i = 0; i < 15000; i++) {
        if (ready) { fb_fdm_set_throttle(f, 0, 1.0); hold_pitch(f, 8 * D2R, 0, 0); }
        fb_fdm_step(f);
        fb_scenario_tick(&sc, f);
        while (fb_scenario_poll_event(&sc, &ev)) if (!strcmp(ev.name, "ready")) { ready = true; t_ready = ev.t; }
        fb_fdm_state(f, &s);
        if (s.on_ground) max_lat = fmax(max_lat, fabs(s.vel_ned[1]));
        if (ready && !off && s.pos_ned[2] < -0.01) { off = true; v_off = ext(f)->airspeed; x_off = s.pos_ned[0]; }
    }
    printf("takeoff: lift-off V %.1f m/s (CL(0) estimate %.1f) after %.0f m; height at %.0f s: %.1f m\n", v_off, v_lo, x_off, s.t - t_ready, -s.pos_ned[2]);
    CHECK(off && v_off > 0.6 * v_lo && v_off < 1.15 * v_lo, "claim 5: rolls from rest to a plausible lift-off speed");
    CHECK(-s.pos_ned[2] > 5, "claim 5: climbs after lift-off");
    CHECK(max_lat < 1e-9, "claim 5: no sideways rolling on the ground");
    fb_fdm_free(f);
    /* roll-out: 8 m/s, motor off, 5 m/s crosswind -> stops, never backwards or sideways */
    f = make(); if (!f) return;
    double wind[3] = { 0, 5, 0 }; fb_fdm_set_wind(f, wind);
    double pos[3] = { 0, 0, 0 }, vel[3] = { 8, 0, 0 }, q[4] = { 1, 0, 0, 0 }, w[3] = { 0, 0, 0 };
    fb_fdm_set_kinematics(f, pos, vel, q, w);
    controls(f, 0, 0, 0);
    double t_stop = -1, vmin = 1e9, lat = 0;
    for (int i = 1; i <= 30000; i++) {
        fb_fdm_step(f); fb_fdm_state(f, &s);
        vmin = fmin(vmin, s.vel_ned[0]); lat = fmax(lat, fabs(s.vel_ned[1]));
        if (t_stop < 0 && s.vel_ned[0] < 0.05) t_stop = i * DT;
    }
    printf("roll-out: stopped after %.1f s over %.1f m, min vx %.4f, lateral %.4f\n", t_stop, s.pos_ned[0], vmin, lat);
    CHECK(t_stop > 0 && vmin >= 0 && lat < 1e-9 && s.on_ground, "claim 5: rolls out and stops; no backward or sideways motion");
    fb_fdm_free(f);
}

typedef struct { double y, k; } pt1_t;
static double pt1(pt1_t *f, double x) { f->y += f->k * (x - f->y); return f->y; }

static double run_launch(double pitch_deg, double *min_z, double *imu_x_ramp, double *bungee_held_s, double *alt_release, double *alt_5s)
{
    fb_fdm_t *f = make(); if (!f) return 0;
    fb_scenario_t sc;
    if (load_scenario(&sc, "launch")) { fb_fdm_free(f); return 0; }
    fb_scenario_start(&sc, f);
    fb_fdm_state_t s; fb_sc_event_t ev; fb_fdm_event_t fe;
    controls(f, 0, 0, 0);
    bool posed = false, released = false; int ramp_n = 0; double imu_sum = 0, held = 0, run = 0; *min_z = 1;
    pt1_t lp = { 0, 1 - exp(-2 * M_PI * 15 * DT) };
    double t_rel = 0, sum = 0;
    for (int i = 0; i < 12000; i++) {
        fb_fdm_step(f);
        fb_scenario_tick(&sc, f);
        while (fb_scenario_poll_event(&sc, &ev)) if (!strcmp(ev.name, "pose") && !posed) {
            posed = true;
            fb_sc_launch_t o; fb_scenario_launch_defaults(&o); o.pitch_deg = pitch_deg; o.heading = FB_SC_HEADING_CURRENT;
            fb_scenario_command(&sc, f, FB_SC_LAUNCH, &o);
        }
        while (fb_fdm_poll_event(f, &fe)) if (fe.kind == FB_FDM_EV_LAUNCH_RELEASE) { released = true; t_rel = f ? 0 : 0; fb_fdm_state(f, &s); *alt_release = -s.pos_ned[2]; t_rel = s.t; fb_fdm_set_throttle(f, 0, 1.0); }
        fb_fdm_state(f, &s);
        *min_z = fmin(*min_z, -s.pos_ned[2]);
        if (ext(f)->launch == 2) { ramp_n++; imu_sum += s.accel_body[0]; }
        double ax = pt1(&lp, s.accel_body[0]);
        if (ax > FB_INAV_LAUNCH_ACCEL_MSS) { run += DT; held = fmax(held, run); } else run = 0;
        if (released) hold_pitch(f, 10 * D2R, 0, 0);
        if (released && s.t >= t_rel + 5.0) { *alt_5s = -s.pos_ned[2]; break; }
        sum += s.pos_ned[2] + s.vel_ned[0];
    }
    *imu_x_ramp = ramp_n ? imu_sum / ramp_n : 0;
    *bungee_held_s = held;
    fb_fdm_free(f);
    return sum;
}

static void test_launch(void)
{
    double min_z, imu_x, held, alt_rel = 0, alt5 = 0;
    double sum1 = run_launch(10, &min_z, &imu_x, &held, &alt_rel, &alt5);
    double a = 50 / 3.6 / 0.5, expect = a + G * sin(10 * D2R);
    printf("launch 10 deg: ramp IMU x %.1f (a + g sin = %.1f), bungee condition held %.0f ms, release at %.2f m, 5 s later %.1f m\n", imu_x, expect, held * 1000, alt_rel, alt5);
    CHECK(fabs(imu_x - expect) < 0.5, "claim 6: ramp IMU X = a + g sin(pitch)");
    CHECK(held >= FB_INAV_LAUNCH_DETECT_S, "claim 6: iNav bungee detector condition held >= 40 ms");
    CHECK(alt5 > alt_rel + 3, "claim 6: climbs once the motor runs: %.1f -> %.1f m", alt_rel, alt5);
    double a_mss, imu;
    CHECK(!fb_launch_weak(50, 0.5, 10, &a_mss, &imu) && fb_launch_weak(20, 2, 10, NULL, NULL), "default profile strong (%.1f m/s^2), a slow one weak", a_mss);
    double sum2 = run_launch(10, &min_z, &imu_x, &held, &alt_rel, &alt5);
    CHECK(sum1 == sum2, "claim 10: deterministic");
    run_launch(-20, &min_z, &imu_x, &held, &alt_rel, &alt5);
    printf("launch -20 deg: lowest height %.3f m\n", min_z);
    CHECK(min_z >= -1e-9, "claim 6: nose-down launch stays above ground");
}

static void test_wind(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    set_flight(f, 300, G0.v, -G0.gamma);
    controls(f, 0, 0, 0);
    for (int i = 0; i < 30000; i++) fb_fdm_step(f);
    fb_fdm_state_t s0, s1;
    fb_fdm_state(f, &s0);
    double va0 = ext(f)->airspeed;
    double wind[3] = { -5, 0, 0 };                  /* a 5 m/s headwind (the air moves south) */
    fb_fdm_set_wind(f, wind);
    fb_fdm_step(f);
    fb_fdm_state(f, &s1);
    printf("wind step: airspeed %.2f -> %.2f, ground speed %.2f -> %.2f\n", va0, ext(f)->airspeed, s0.vel_ned[0], s1.vel_ned[0]);
    CHECK(fabs(ext(f)->airspeed - va0 - 5) < 0.3 && fabs(s1.vel_ned[0] - s0.vel_ned[0]) < 0.05, "claim 7: a wind step changes airspeed, not ground speed");
    for (int i = 0; i < 60000; i++) fb_fdm_step(f);
    fb_fdm_state(f, &s1);
    printf("in wind after 60 s: airspeed %.2f (trim %.2f), ground speed %.2f\n", ext(f)->airspeed, G0.v, s1.vel_ned[0]);
    CHECK(fabs(ext(f)->airspeed / G0.v - 1) < 0.05 && s1.vel_ned[0] < G0.v - 3, "claim 7: trim airspeed kept in wind");
    fb_fdm_free(f);
}

static void test_poses(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    fb_scenario_t sc;
    if (load_scenario(&sc, "acc-calibration")) return;
    fb_scenario_start(&sc, f);
    controls(f, 0, 0, 0);
    fb_fdm_state_t s; fb_sc_event_t ev;
    int poses = 0; double err_int = 0, still_err = 0, g_err = 0;
    double q_prev[4];
    fb_fdm_state(f, &s); memcpy(q_prev, s.quat, sizeof q_prev);
    for (int i = 0; i < 28000; i++) {
        fb_fdm_step(f);
        fb_scenario_tick(&sc, f);
        fb_fdm_state(f, &s);
        /* the reported rates integrate to the attitude change */
        double qi[4]; memcpy(qi, q_prev, sizeof qi); fb_q_integrate(qi, s.rates, DT);
        double e[3]; fb_q_error(qi, s.quat, e); err_int = fmax(err_int, fb_v3_norm(e) * R2D);
        memcpy(q_prev, s.quat, sizeof q_prev);
        if (s.posed) { still_err = fmax(still_err, fb_v3_norm(s.rates)); g_err = fmax(g_err, fabs(fb_v3_norm(s.accel_body) - G)); }
        while (fb_scenario_poll_event(&sc, &ev)) if (!strcmp(ev.name, "pose")) {
            poses++;
            const fb_sc_step_t *st = &sc.steps[ev.step];
            double roll, pitch;
            fb_fdm_euler(f, &roll, &pitch, NULL);
            CHECK(fabs(roll * R2D - st->euler_deg[0]) < 0.5 && fabs(pitch * R2D - st->euler_deg[1]) < 0.5, "pose %s: %.1f / %.1f vs %.0f / %.0f", st->label, roll * R2D, pitch * R2D, st->euler_deg[0], st->euler_deg[1]);
        }
    }
    printf("poses: %d reached, integrated-rate deviation %.3f deg, rates while posed %.2g, |accel|-g %.2g\n", poses, err_int, still_err, g_err);
    CHECK(poses == 7, "claim 8: all poses reached (%d)", poses);
    CHECK(err_int < 1.0, "claim 8: IMU rates follow the pose change");
    CHECK(still_err < 1e-9 && g_err < 1e-6, "claim 8: still while posed, the holder's reaction on the IMU");
    fb_fdm_free(f);
}

static void test_mismix(void)
{
    fb_fdm_t *f = make(); if (!f) return;
    set_flight(f, 200, G0.v, 0);
    fb_fdm_set_input_us(f, 0, us(0.5)); fb_fdm_set_input_us(f, 1, us(0.5));
    fb_fdm_set_input_us(f, 2, us(0)); fb_fdm_set_input_us(f, 3, us(0));
    fb_fdm_event_t ev; int warned = 0;
    for (int i = 0; i < 600; i++) { fb_fdm_step(f); while (fb_fdm_poll_event(f, &ev)) if (ev.kind == FB_FDM_EV_MIS_MIX) warned++; }
    printf("aileron mis-mix: both +0.5 -> warning %d, roll input %.3f\n", warned, ext(f)->aileron);
    CHECK(warned == 1 && ext(f)->aileron == 0, "claim 9: same-sign ailerons warn and give no roll");
    fb_fdm_free(f);
}

int main(void)
{
    test_rest_and_params();
    test_glide();
    test_level_full_throttle();
    test_steps();
    test_stall();
    test_ground();
    test_launch();
    test_wind();
    test_poses();
    test_mismix();
    printf("test_plane: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
