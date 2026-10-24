#include "fb/rotor.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

double fb_rotor_solidity(const fb_rotor_geom_t *g)
{
    return g->blades * g->chord_m / (PI * g->radius_m);
}

/* Blade-element thrust coefficient for a given total inflow ratio. The blade's angle of attack at
 * 0.75 R, theta0 - lambda / 0.75, is limited to +-stall: beyond it the lift stops growing (a steep
 * descent or a wild collective cannot make the blades lift without bound). */
static double ct_of(const fb_rotor_geom_t *g, double sigma, double theta0, double mu, double lambda)
{
    double B = g->tip_loss;
    double stall = g->stall_rad > 0 ? g->stall_rad : 14.0 * 3.14159265358979323846 / 180.0;
    double lo = 0.75 * (theta0 - stall), hi = 0.75 * (theta0 + stall);
    if (lambda < lo) lambda = lo;
    else if (lambda > hi) lambda = hi;
    return 0.5 * sigma * g->lift_slope * (theta0 * (B * B * B / 3.0 + mu * mu * B / 2.0) - lambda * B * B / 2.0);
}

void fb_rotor_solve(const fb_rotor_geom_t *g, double rho, double omega, double theta0, double lambda_c,
                    double mu, double *lambda_i_io, fb_rotor_out_t *o)
{
    memset(o, 0, sizeof *o);
    double sigma = fb_rotor_solidity(g);
    double vt = omega * g->radius_m;
    double area = PI * g->radius_m * g->radius_m;
    o->solidity = sigma; o->tip_speed = vt; o->disc_area = area;
    if (!(vt > 1e-6)) { if (lambda_i_io) *lambda_i_io = 0; return; }
    if (mu < 0) mu = 0;

    double li = lambda_i_io ? *lambda_i_io : 0.0;
    double ct = 0;
    for (int it = 0; it < 40; it++) {
        double lambda = lambda_c + li;
        ct = ct_of(g, sigma, theta0, mu, lambda);
        double lh = sqrt(fabs(ct) / 2.0);                 /* hover inflow for this thrust */
        double den = sqrt(mu * mu + lambda * lambda);
        if (den < lh) den = lh;                           /* vortex-ring guard: induced <= hover value */
        double li_new = den > 1e-9 ? (ct / 2.0) / den : 0.0;
        double next = 0.5 * li + 0.5 * li_new;            /* relaxed fixed point */
        double change = fabs(next - li);
        li = next;
        if (change < 1e-9 * (1.0 + fabs(li))) break;
    }
    double lambda = lambda_c + li;
    ct = ct_of(g, sigma, theta0, mu, lambda);
    o->ct = ct;
    o->lambda_i = li; o->lambda = lambda;
    /* torque: induced + climb + profile; H force: profile drag of the advancing/retreating blades */
    o->cq = g->kappa * ct * li + ct * lambda_c + (sigma * g->cd0 / 8.0) * (1.0 + 4.6 * mu * mu);
    o->ch = sigma * g->cd0 * mu / 4.0;
    double q = rho * area * vt * vt;
    o->thrust_n = ct * q;
    o->torque_nm = o->cq * q * g->radius_m;
    o->h_force_n = o->ch * q;
    if (lambda_i_io) *lambda_i_io = li;
}

double fb_rotor_collective_for_thrust(const fb_rotor_geom_t *g, double rho, double omega, double thrust_n)
{
    double lo = -30.0 * PI / 180.0, hi = 30.0 * PI / 180.0;
    fb_rotor_out_t o;
    for (int i = 0; i < 60; i++) {
        double mid = 0.5 * (lo + hi), li = 0;
        fb_rotor_solve(g, rho, omega, mid, 0, 0, &li, &o);
        if (o.thrust_n < thrust_n) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}
