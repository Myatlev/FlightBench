/* FlightBench flight dynamics: the one interface every aircraft type implements (docs/fdm.md §4).
 *
 * An FDM is stepped every dt of virtual time with the flight controller's actuator outputs (µs, the
 * wiring's reversal already applied) and the motor speed(s) from the ESC model; it answers with what an
 * IMU rigidly mounted on the airframe measures, the pose, and events. Deterministic: same parameters
 * and inputs, bit-identical state. Frames: NED world (z down, the pad is the origin), FRD body; SI.
 */
#ifndef FB_FDM_H
#define FB_FDM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FB_FDM_HELI = 1, FB_FDM_PLANE = 2 } fb_fdm_kind_t;

#define FB_FDM_MAX_INPUTS 8

/* The common state block (every type). */
typedef struct {
    double t;                  /* simulated time, s */
    double pos_ned[3];         /* m from the pad; down positive; 0 = the reference point on the ground */
    double vel_ned[3];         /* ground velocity, m/s */
    double quat[4];            /* w, x, y, z, body -> NED */
    double rates[3];           /* body rates, rad/s (FRD) */
    double accel_body[3];      /* specific force, m/s^2, body (at rest on the ground: 0, 0, -g) */
    bool   on_ground;
    bool   held;               /* a hand / stand holds it (constraint API) */
    bool   posed;              /* held still at its attitude (the step after the rotation) */
    unsigned crashes;          /* one per flight segment (informational, thrust never blocked) */
    double crash_info[3];      /* last hard contact: roll_deg, pitch_deg, speed_mps */
} fb_fdm_state_t;

/* The helicopter's block (fb_fdm_state_ext for FB_FDM_HELI). */
typedef struct {
    double rotor_rpm;          /* head speed */
    double collective_deg;     /* blade pitch commanded by the plate */
    double cyclic_deg[2];      /* pitch (nose-up +), roll (right +) cyclic commanded */
    double tpp_rad[2];         /* tip-path-plane tilt: a1 (aft +), b1 (right +) */
    double thrust_n;           /* main rotor thrust along the TPP normal */
    double torque_nm;          /* rotor shaft torque */
    double tail_thrust_n;
    double tail_pitch_deg;
    double load;               /* shaft load for the ESC, 0..1 */
} fb_fdm_heli_ext_t;

/* The plane's block (fb_fdm_state_ext for FB_FDM_PLANE). */
typedef struct {
    double airspeed;
    double alpha, beta;        /* rad */
    double motor_rpm;
    double thrust_n;
    double surface[4];         /* aileron L/R, elevator, rudder after delay and slew, -1..1 */
    double aileron, elevator, rudder;   /* effective controls, -1..1 */
    int    launch;             /* 0 none, 1 aligning, 2 ramp */
} fb_fdm_plane_ext_t;

typedef enum {
    FB_FDM_EV_POSE = 1,        /* v: roll_deg, pitch_deg, yaw_deg — a hold reached its attitude */
    FB_FDM_EV_CRASH,           /* v: roll_deg, pitch_deg, speed_mps */
    FB_FDM_EV_LAUNCH_START,    /* v: a_mss, speed_mps, heading_deg, pitch_deg */
    FB_FDM_EV_LAUNCH_RELEASE,  /* v: speed_mps, height_m */
    FB_FDM_EV_MIS_MIX,         /* v: left, right — both ailerons deflected the same way */
} fb_fdm_event_kind_t;

typedef struct {
    fb_fdm_event_kind_t kind;
    double t;
    double v[4];
} fb_fdm_event_t;

typedef struct fb_fdm fb_fdm_t;

/* params_json: the aircraft file's "fdm_params" object as text (NULL / "" = the type's defaults). An
 * unknown name, a wrong type or an out-of-range value is an error: NULL back, err = "path: what". */
fb_fdm_t *fb_fdm_new(fb_fdm_kind_t kind, const char *params_json, size_t params_len, double dt_s,
                     char *err, size_t errlen);
void fb_fdm_free(fb_fdm_t *f);
fb_fdm_kind_t fb_fdm_kind(const fb_fdm_t *f);
double fb_fdm_dt(const fb_fdm_t *f);

void fb_fdm_reset(fb_fdm_t *f);           /* fresh start: at once, on the ground, level, heading 0 */
/* Back on the pad the way the IMU can follow: position / velocity jump, the attitude rotates to level
 * and heading 0 in 0.25 s with consistent rates. Rotor state and the crash counter are kept. */
void fb_fdm_return_to_pad(fb_fdm_t *f);

/* Inputs by actuator name (the aircraft file's fdm_inputs); -1 = not an input of this model. */
int  fb_fdm_input_count(const fb_fdm_t *f);
const char *fb_fdm_input_name(const fb_fdm_t *f, int index);
int  fb_fdm_input_index(const fb_fdm_t *f, const char *actuator);
void fb_fdm_set_input_us(fb_fdm_t *f, int index, uint16_t us);

/* Motor speed from an ESC model (the single source of motor state; for the heli: the head speed,
 * gear already applied). rpm < 0: no ESC bound — the speed follows the throttle input through a lag. */
void fb_fdm_set_motor_rpm(fb_fdm_t *f, int motor, double rpm);
void fb_fdm_set_throttle(fb_fdm_t *f, int motor, double throttle_0_1);   /* for the stand-in only */
double fb_fdm_load(const fb_fdm_t *f, int motor);                        /* 0..1 for fb_esc_set_load */

/* Environment: air density (kg/m^3), wind (NED, the direction the air moves to). */
void fb_fdm_set_air(fb_fdm_t *f, double rho);
void fb_fdm_set_wind(fb_fdm_t *f, const double wind_ned[3]);
double fb_fdm_air_density(double pressure_pa, double temp_c);

void fb_fdm_step(fb_fdm_t *f);

void fb_fdm_state(const fb_fdm_t *f, fb_fdm_state_t *out);
const void *fb_fdm_state_ext(const fb_fdm_t *f, size_t *size);   /* fb_fdm_heli_ext_t / fb_fdm_plane_ext_t */
void fb_fdm_imu(const fb_fdm_t *f, float gyro_dps[3], float accel_g[3]);
void fb_fdm_euler(const fb_fdm_t *f, double *roll, double *pitch, double *yaw);   /* radians, ZYX */

/* Constraints: a hand or a stand. Angles in radians; yaw NAN = keep the heading; rotate_s 0 = at once
 * (a jump the IMU does not see: only before a start). */
void fb_fdm_hold(fb_fdm_t *f, double roll, double pitch, double yaw, double rotate_s);
void fb_fdm_hold_quat(fb_fdm_t *f, const double q[4], double rotate_s);
void fb_fdm_release(fb_fdm_t *f);
/* A rail / hand launch (plane): 0, or -1 for an invalid profile or a type without launches. */
int  fb_fdm_launch(fb_fdm_t *f, double speed_mps, double duration_s, double heading, double pitch);
/* Tests and scripted starts: position, ground velocity, attitude and rates at once (a jump). */
void fb_fdm_set_kinematics(fb_fdm_t *f, const double pos_ned[3], const double vel_ned[3],
                           const double quat[4], const double rates[3]);

bool fb_fdm_poll_event(fb_fdm_t *f, fb_fdm_event_t *ev);

/* Local NED position -> geodetic, flat earth around home. */
void fb_fdm_ned_to_geo(double home_lat_deg, double home_lon_deg, const double pos_ned[3],
                       double *lat, double *lon);

#ifdef __cplusplus
}
#endif
#endif
