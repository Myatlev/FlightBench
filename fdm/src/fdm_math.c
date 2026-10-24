#include "fb/fdm_math.h"

#include <math.h>
#include <string.h>

double fb_clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
void fb_v3_set(double o[3], double x, double y, double z) { o[0] = x; o[1] = y; o[2] = z; }
void fb_v3_copy(double o[3], const double a[3]) { memcpy(o, a, 3 * sizeof *o); }
void fb_v3_add(double o[3], const double a[3], const double b[3]) { for (int i = 0; i < 3; i++) o[i] = a[i] + b[i]; }
void fb_v3_scale(double o[3], const double a[3], double k) { for (int i = 0; i < 3; i++) o[i] = a[i] * k; }
void fb_v3_cross(double o[3], const double a[3], const double b[3])
{
    double x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
double fb_v3_dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double fb_v3_norm(const double a[3]) { return sqrt(fb_v3_dot(a, a)); }

void fb_q_identity(double q[4]) { q[0] = 1; q[1] = q[2] = q[3] = 0; }
void fb_q_normalize(double q[4])
{
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n <= 0) { fb_q_identity(q); return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
void fb_q_mul(double o[4], const double a[4], const double b[4])
{
    double w = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    double x = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    double y = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    double z = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
    o[0] = w; o[1] = x; o[2] = y; o[3] = z;
}
void fb_q_conj(double o[4], const double q[4]) { o[0] = q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = -q[3]; }

void fb_q_from_euler(double q[4], double roll, double pitch, double yaw)
{
    double cr = cos(roll / 2), sr = sin(roll / 2), cp = cos(pitch / 2), sp = sin(pitch / 2);
    double cy = cos(yaw / 2), sy = sin(yaw / 2);
    q[0] = cr * cp * cy + sr * sp * sy;
    q[1] = sr * cp * cy - cr * sp * sy;
    q[2] = cr * sp * cy + sr * cp * sy;
    q[3] = cr * cp * sy - sr * sp * cy;
}
void fb_q_to_euler(const double q[4], double *roll, double *pitch, double *yaw)
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    if (roll) *roll = atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y));
    if (pitch) { double s = 2 * (w * y - z * x); *pitch = asin(fb_clamp(s, -1, 1)); }
    if (yaw) *yaw = atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z));
}
void fb_q_to_dcm(const double q[4], double m[3][3])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - w * z);     m[0][2] = 2 * (x * z + w * y);
    m[1][0] = 2 * (x * y + w * z);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - w * x);
    m[2][0] = 2 * (x * z - w * y);     m[2][1] = 2 * (y * z + w * x);     m[2][2] = 1 - 2 * (x * x + y * y);
}
double fb_q_dcm_element(const double q[4], int row, int col)
{
    double m[3][3];
    fb_q_to_dcm(q, m);
    return m[row][col];
}
void fb_q_rotate(const double q[4], const double v[3], double o[3])
{
    double m[3][3], r[3];
    fb_q_to_dcm(q, m);
    for (int i = 0; i < 3; i++) r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    fb_v3_copy(o, r);
}
void fb_q_rotate_inv(const double q[4], const double v[3], double o[3])
{
    double m[3][3], r[3];
    fb_q_to_dcm(q, m);
    for (int i = 0; i < 3; i++) r[i] = m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2];
    fb_v3_copy(o, r);
}
void fb_q_integrate(double q[4], const double w3[3], double dt)
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    double wx = w3[0], wy = w3[1], wz = w3[2];
    double n[4] = {
        w + 0.5 * dt * (-x * wx - y * wy - z * wz),
        x + 0.5 * dt * (w * wx + y * wz - z * wy),
        y + 0.5 * dt * (w * wy - x * wz + z * wx),
        z + 0.5 * dt * (w * wz + x * wy - y * wx),
    };
    memcpy(q, n, sizeof n);
    fb_q_normalize(q);
}
void fb_q_error(const double q[4], const double target[4], double e[3])
{
    double c[4], d[4];
    fb_q_conj(c, q);
    fb_q_mul(d, c, target);                        /* target = q * d: d rotates about body axes */
    if (d[0] < 0) for (int i = 0; i < 4; i++) d[i] = -d[i];
    double vn = sqrt(d[1] * d[1] + d[2] * d[2] + d[3] * d[3]);
    double k = vn > 1e-12 ? 2.0 * atan2(vn, d[0]) / vn : 2.0;
    e[0] = k * d[1]; e[1] = k * d[2]; e[2] = k * d[3];
}

int fb_actuator_delay_steps(double delay_ms, double dt)
{
    if (!(delay_ms > 0) || !(dt > 0)) return 0;
    double steps = delay_ms * 1e-3 / dt;
    int n = (int)floor(steps + 0.5 + 1e-9);      /* 1.5 -> 2; 7.000000000000001 -> 7 */
    if (n < 0) n = 0;
    if (n > FB_ACT_MAX_DELAY) n = FB_ACT_MAX_DELAY;
    return n;
}
void fb_actuator_init(fb_actuator_t *a, double dt, double delay_ms, double speed_s60, double full_deg)
{
    memset(a, 0, sizeof *a);
    a->delay = fb_actuator_delay_steps(delay_ms, dt);
    a->max_step = (speed_s60 > 0 && full_deg > 0) ? (60.0 / speed_s60) * dt / full_deg : 0;
}
void fb_actuator_reset(fb_actuator_t *a, double pos)
{
    for (int i = 0; i <= FB_ACT_MAX_DELAY; i++) a->buf[i] = pos;
    a->head = 0;
    a->pos = pos;
}
double fb_actuator_step(fb_actuator_t *a, double cmd)
{
    a->buf[a->head] = cmd;
    int tap = a->head - a->delay;
    if (tap < 0) tap += FB_ACT_MAX_DELAY + 1;
    double target = a->buf[tap];
    a->head = (a->head + 1) % (FB_ACT_MAX_DELAY + 1);
    if (a->max_step > 0) {
        double d = target - a->pos;
        if (d > a->max_step) d = a->max_step;
        else if (d < -a->max_step) d = -a->max_step;
        a->pos += d;
    } else {
        a->pos = target;
    }
    return a->pos;
}

double fb_us_norm(uint16_t us, double centre_us, double travel_us)
{
    if (!(travel_us > 0)) return 0;
    return fb_clamp(((double)us - centre_us) / travel_us, -1.5, 1.5);
}
