/* FlightBench FDM: the fixed wing's parameters and coefficient functions (docs/fdm.md §3), public for
 * tests and analytic checks. The model itself lives behind fb/fdm.h (FB_FDM_PLANE). */
#ifndef FB_PLANE_H
#define FB_PLANE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* iNav's NAV LAUNCH bungee detector: nav_fw_launch_accel default 1863 cm/s^2 on the body-X
 * accelerometer (gravity included) for nav_fw_launch_detect_time 40 ms (inav 9.1.0). */
#define FB_INAV_LAUNCH_ACCEL_MSS 18.63
#define FB_INAV_LAUNCH_DETECT_S  0.040
#define FB_LAUNCH_ACCEL_MARGIN   1.2

typedef struct {
    double mass_kg, inertia[3];
    double wing_area, span, chord;                   /* m^2, m, m */
    /* lift and drag (per rad where an angle is involved) */
    double lift0, lift_alpha, lift_q, lift_elevator;
    double alpha_stall, stall_sharpness;             /* rad, - */
    double drag0, drag_elevator, oswald;
    /* side force, roll, pitch, yaw */
    double side_beta, side_p, side_r, side_aileron, side_rudder;
    double roll_beta, roll_p, roll_r, roll_aileron, roll_rudder;
    double pitch0, pitch_alpha, pitch_q, pitch_elevator;
    double yaw_beta, yaw_p, yaw_r, yaw_aileron, yaw_rudder;
    double cp_offset[3];                             /* aerodynamic centre from the CG, body, m */
    /* propeller and motor */
    double prop_diameter_in, prop_pitch_in, prop_ct, prop_kt;
    double thrust_offset[3], prop_torque_factor, rpm_max, standin_tau_s;
    /* controls */
    double surface_deflection_deg, servo_delay_ms, servo_speed_s60, servo_centre_us, servo_travel_us;
    /* ground */
    double ground_friction, gear_height_m, gear_arm_m;   /* gear_arm: CG ahead of the main wheels */
} fb_plane_params_t;

typedef struct fb_fdm fb_fdm_t;
/* The parameters a plane FDM runs with (NULL for another kind). */
const fb_plane_params_t *fb_fdm_plane_params(const fb_fdm_t *f);

/* Lift coefficient with the stall blend, drag coefficient (alpha in rad; elevator -1..1). */
double fb_plane_cl(const fb_plane_params_t *p, double alpha);
double fb_plane_cd(const fb_plane_params_t *p, double alpha);
/* Aerodynamic force (N, body) and moment (N m, body, about the CG, cp offset included) at the given
 * air data and controls. rates: body rad/s. */
void fb_plane_aero(const fb_plane_params_t *p, double rho, double airspeed, double alpha, double beta,
                   const double rates[3], double aileron, double elevator, double rudder,
                   double force[3], double moment[3]);
/* Propeller thrust from the motor speed and the axial airspeed. */
double fb_plane_thrust(const fb_plane_params_t *p, double rho, double rpm, double airspeed);
/* The launch profile check for the core: ramp acceleration and the IMU X it gives; true = weak
 * (below FB_LAUNCH_ACCEL_MARGIN x the iNav threshold). */
bool fb_launch_weak(double speed_kmh, double duration_s, double pitch_deg, double *a_mss, double *imu_x_mss);

#ifdef __cplusplus
}
#endif
#endif
