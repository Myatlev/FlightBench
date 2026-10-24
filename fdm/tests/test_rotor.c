/* The rotor disc (docs/fdm.md §2.3): blade-element thrust with momentum inflow. */
#include "fb/rotor.h"

#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define D2R (M_PI / 180.0)

int main(void)
{
    fb_rotor_geom_t g = { .radius_m = 0.804, .chord_m = 0.060, .blades = 2, .lift_slope = 5.7, .cd0 = 0.011,
                          .tip_loss = 0.97, .kappa = 1.15 };
    double rho = 1.225, omega = 1800 * 2 * M_PI / 60;
    fb_rotor_out_t o;
    double li = 0;

    CHECK(fabs(fb_rotor_solidity(&g) - 0.0475) < 0.001, "solidity %.4f", fb_rotor_solidity(&g));

    /* hover: thrust grows with collective, inflow consistent with momentum theory */
    fb_rotor_solve(&g, rho, omega, 0, 0, 0, &li, &o);
    CHECK(fabs(o.thrust_n) < 1.0 && fabs(o.lambda_i) < 1e-3, "zero collective: no thrust (%.2f N)", o.thrust_n);
    double prev = 0;
    for (int d = 1; d <= 12; d++) {
        li = 0;
        fb_rotor_solve(&g, rho, omega, d * D2R, 0, 0, &li, &o);
        CHECK(o.thrust_n > prev, "thrust grows with collective: %d deg %.1f N (prev %.1f)", d, o.thrust_n, prev);
        double lh2 = o.ct / 2;
        CHECK(fabs(o.lambda_i * o.lambda_i - lh2) < 0.01 * lh2 + 1e-9, "momentum theory at %d deg: li^2 %.3g vs ct/2 %.3g", d, o.lambda_i * o.lambda_i, lh2);
        CHECK(o.torque_nm > 0, "torque positive at %d deg", d);
        prev = o.thrust_n;
    }
    /* a 700 at 1800 rpm hovers 5 kg between 2.5 and 7 degrees */
    double coll = fb_rotor_collective_for_thrust(&g, rho, omega, 5.0 * 9.80665);
    li = 0;
    fb_rotor_solve(&g, rho, omega, coll, 0, 0, &li, &o);
    printf("hover collective for 5 kg: %.2f deg, torque %.2f N m, power %.0f W\n", coll / D2R, o.torque_nm, o.torque_nm * omega);
    CHECK(coll / D2R > 2.5 && coll / D2R < 7.0, "hover collective %.2f deg", coll / D2R);
    CHECK(fabs(o.thrust_n - 5.0 * 9.80665) < 0.5, "collective_for_thrust gives the thrust: %.1f N", o.thrust_n);
    CHECK(o.torque_nm * omega > 400 && o.torque_nm * omega < 2000, "hover power %.0f W plausible for a 700", o.torque_nm * omega);

    /* climb reduces thrust at fixed collective, descent raises it (inflow damping), continuous in descent */
    double t0 = o.thrust_n, lprev = o.thrust_n;
    for (int k = 1; k <= 8; k++) {
        double lc = 0.01 * k;
        li = 0;
        fb_rotor_solve(&g, rho, omega, coll, lc, 0, &li, &o);
        CHECK(o.thrust_n < lprev, "climb lambda_c %.2f reduces thrust: %.1f N", lc, o.thrust_n);
        lprev = o.thrust_n;
    }
    lprev = t0;
    for (int k = 1; k <= 8; k++) {
        double lc = -0.01 * k;
        li = 0;
        fb_rotor_solve(&g, rho, omega, coll, lc, 0, &li, &o);
        CHECK(o.thrust_n > lprev - 1e-9, "descent lambda_c %.2f raises thrust: %.1f N (prev %.1f)", lc, o.thrust_n, lprev);
        CHECK(isfinite(o.thrust_n) && o.thrust_n < 12 * t0, "descent stays finite and bounded by the stall: %.1f N", o.thrust_n);
        lprev = o.thrust_n;
    }
    /* forward flight: induced inflow drops (translational lift) */
    li = 0;
    fb_rotor_solve(&g, rho, omega, coll, 0, 0.1, &li, &o);
    double t_fwd = o.thrust_n, li_fwd = o.lambda_i;
    li = 0;
    fb_rotor_solve(&g, rho, omega, coll, 0, 0, &li, &o);
    CHECK(li_fwd < o.lambda_i && t_fwd > o.thrust_n, "mu 0.1: less induced inflow (%.4f vs %.4f), more thrust (%.1f vs %.1f)", li_fwd, o.lambda_i, t_fwd, o.thrust_n);

    /* negative collective: the mirror image */
    li = 0;
    fb_rotor_solve(&g, rho, omega, -coll, 0, 0, &li, &o);
    CHECK(fabs(o.thrust_n + 5.0 * 9.80665) < 0.5, "negative collective mirrors the thrust: %.1f N", o.thrust_n);
    CHECK(o.lambda_i < 0, "induced inflow follows the thrust sign: %.4f", o.lambda_i);
    CHECK(o.torque_nm > 0, "the torque to spin the rotor stays positive: %.2f", o.torque_nm);

    /* the warm start converges to the same answer */
    double li_a = 0, li_b = 0.05;
    fb_rotor_out_t oa, ob;
    fb_rotor_solve(&g, rho, omega, 8 * D2R, 0.02, 0.05, &li_a, &oa);
    fb_rotor_solve(&g, rho, omega, 8 * D2R, 0.02, 0.05, &li_b, &ob);
    CHECK(fabs(oa.thrust_n - ob.thrust_n) < 0.05 * fabs(oa.thrust_n), "warm start converges: %.2f vs %.2f N", oa.thrust_n, ob.thrust_n);

    printf("test_rotor: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
