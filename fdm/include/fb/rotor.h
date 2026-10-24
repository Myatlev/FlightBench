/* FlightBench FDM: a rotor disc by blade-element theory with momentum-theory inflow (docs/fdm.md §2.3).
 *
 * Non-dimensional, uniform inflow, untwisted blades, linear lift; the inflow and the thrust are solved
 * together (fixed point, warm-started). Sign conventions: thrust and torque coefficients carry the sign
 * of the lift (negative collective = negative thrust: the same physics inverted); lambda_c is the climb
 * inflow ratio (positive when the disc moves against its thrust direction, i.e. climbing when the thrust
 * points up); mu the in-plane advance ratio (>= 0). The induced inflow follows the thrust sign. In the
 * vortex-ring region of descent momentum theory has no solution; the induced velocity is capped at its
 * hover value there, which keeps thrust continuous.
 */
#ifndef FB_ROTOR_H
#define FB_ROTOR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double radius_m;
    double chord_m;
    double blades;
    double lift_slope;        /* per rad, 5.7 */
    double cd0;               /* profile drag coefficient of the section */
    double tip_loss;          /* B, 0.97 */
    double kappa;             /* induced-power factor, 1.15 */
    double stall_rad;         /* blade stall angle at 0.75 R; 0 = 14 deg (lift limited there, no post-stall model) */
} fb_rotor_geom_t;

typedef struct {
    double ct, cq, ch;        /* thrust, torque, in-plane (H) force coefficients */
    double lambda_i, lambda;  /* induced and total inflow ratio */
    double thrust_n, torque_nm, h_force_n;
    double solidity, tip_speed, disc_area;
} fb_rotor_out_t;

double fb_rotor_solidity(const fb_rotor_geom_t *g);
/* theta0 in rad; omega rad/s; lambda_i_io: the previous solution in, the new one out (0 = cold). */
void fb_rotor_solve(const fb_rotor_geom_t *g, double rho, double omega, double theta0, double lambda_c,
                    double mu, double *lambda_i_io, fb_rotor_out_t *o);
/* The collective (rad) that gives the thrust at hover (lambda_c = mu = 0): bisection on fb_rotor_solve. */
double fb_rotor_collective_for_thrust(const fb_rotor_geom_t *g, double rho, double omega, double thrust_n);

#ifdef __cplusplus
}
#endif
#endif
