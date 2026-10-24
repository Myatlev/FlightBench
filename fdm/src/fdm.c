#include "fdm_internal.h"
#include "fb/fdm_math.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- events -------------------------------------------------------------------------------- */

void fb_fdm_push_event(fb_fdm_t *f, fb_fdm_event_kind_t kind, double a, double b, double c, double d)
{
    int slot = (f->ev_head + f->ev_count) % FB_FDM_MAX_EVENTS;
    if (f->ev_count == FB_FDM_MAX_EVENTS) { f->ev_head = (f->ev_head + 1) % FB_FDM_MAX_EVENTS; slot = (f->ev_head + f->ev_count - 1) % FB_FDM_MAX_EVENTS; }
    else f->ev_count++;
    fb_fdm_event_t *e = &f->events[slot];
    e->kind = kind; e->t = f->body.t;
    e->v[0] = a; e->v[1] = b; e->v[2] = c; e->v[3] = d;
}

bool fb_fdm_poll_event(fb_fdm_t *f, fb_fdm_event_t *ev)
{
    if (!f || f->ev_count == 0) return false;
    *ev = f->events[f->ev_head];
    f->ev_head = (f->ev_head + 1) % FB_FDM_MAX_EVENTS;
    f->ev_count--;
    return true;
}

/* ---- parameters ---------------------------------------------------------------------------- */

static int param_fail(char *err, size_t errlen, const char *path, const char *what)
{
    if (err && errlen) snprintf(err, errlen, "%s: %s", path, what);
    return -1;
}

static const fb_param_t *find_param(const fb_param_t *table, size_t count, const char *path)
{
    for (size_t i = 0; i < count; i++) if (!strcmp(table[i].path, path)) return &table[i];
    return NULL;
}

static int load_leaf(const fb_param_t *p, const fb_json_node_t *v, const char *path, char *err, size_t errlen)
{
    char what[160];
    switch (p->kind) {
    case FB_P_NUM: {
        if (v->type != FB_JSON_NUMBER) return param_fail(err, errlen, path, "a number");
        double x = fb_json_number(v);
        if (!isfinite(x) || x < p->lo || x > p->hi) {
            snprintf(what, sizeof what, "a number in %g..%g", p->lo, p->hi);
            return param_fail(err, errlen, path, what);
        }
        *(double *)p->dst = x;
        return 0;
    }
    case FB_P_VEC: {
        if (v->type != FB_JSON_ARRAY || v->count != p->n) {
            snprintf(what, sizeof what, "an array of %d numbers", p->n);
            return param_fail(err, errlen, path, what);
        }
        double *d = (double *)p->dst;
        for (int i = 0; i < p->n; i++) {
            const fb_json_node_t *e = fb_json_at(v, i);
            if (!e || e->type != FB_JSON_NUMBER) return param_fail(err, errlen, path, "numbers only");
            double x = fb_json_number(e);
            if (!isfinite(x) || x < p->lo || x > p->hi) {
                snprintf(what, sizeof what, "numbers in %g..%g", p->lo, p->hi);
                return param_fail(err, errlen, path, what);
            }
            d[i] = x;
        }
        return 0;
    }
    case FB_P_NAMES: {
        if (v->type != FB_JSON_ARRAY || v->count != p->n) {
            snprintf(what, sizeof what, "an array of %d names", p->n);
            return param_fail(err, errlen, path, what);
        }
        const char **names = (const char **)p->dst;
        for (int i = 0; i < p->n; i++) {
            const fb_json_node_t *e = fb_json_at(v, i);
            if (!e || e->type != FB_JSON_STRING) return param_fail(err, errlen, path, "names only");
            char buf[64];
            size_t need = fb_json_str(e, buf, sizeof buf);
            if (need == 0 || need >= sizeof buf) return param_fail(err, errlen, path, "a name of 1..63 bytes");
            char *copy = malloc(need + 1);
            if (!copy) return param_fail(err, errlen, path, "out of memory");
            memcpy(copy, buf, need + 1);
            free((void *)names[i]);
            names[i] = copy;
        }
        return 0;
    }
    case FB_P_ENUM: {
        if (v->type != FB_JSON_STRING) return param_fail(err, errlen, path, "a name");
        for (int i = 0; p->choices[i]; i++) {
            if (fb_json_str_eq(v, p->choices[i])) { *(int *)p->dst = i; return 0; }
        }
        return param_fail(err, errlen, path, "not one of the choices");
    }
    }
    return param_fail(err, errlen, path, "unknown kind");
}

static int walk(const fb_param_t *table, size_t count, const fb_json_node_t *obj, const char *prefix,
                char *err, size_t errlen)
{
    const fb_json_node_t *k = obj + 1;
    for (int i = 0; i < obj->count; i++, k = (k + 1) + k[1].skip) {
        const fb_json_node_t *v = k + 1;
        char key[64], path[128];
        size_t need = fb_json_str(k, key, sizeof key);
        if (need == 0 || need >= sizeof key) return param_fail(err, errlen, prefix[0] ? prefix : "(root)", "a key of 1..63 bytes");
        snprintf(path, sizeof path, "%s%s%s", prefix, prefix[0] ? "." : "", key);
        const fb_param_t *p = find_param(table, count, path);
        if (p) {
            if (load_leaf(p, v, path, err, errlen)) return -1;
        } else if (v->type == FB_JSON_OBJECT) {
            if (walk(table, count, v, path, err, errlen)) return -1;
        } else {
            return param_fail(err, errlen, path, "unknown parameter");
        }
    }
    return 0;
}

int fb_params_load(const fb_param_t *table, size_t count, const char *json, size_t len, char *err, size_t errlen)
{
    if (!json || len == 0) return 0;
    char perr[96];
    int n = fb_json_parse(json, len, NULL, 0, perr, sizeof perr);
    if (n < 0) return param_fail(err, errlen, "fdm_params", perr);
    fb_json_node_t *nodes = malloc((size_t)n * sizeof *nodes);
    if (!nodes) return param_fail(err, errlen, "fdm_params", "out of memory");
    fb_json_parse(json, len, nodes, n, perr, sizeof perr);
    int rc;
    if (nodes[0].type != FB_JSON_OBJECT) rc = param_fail(err, errlen, "fdm_params", "an object");
    else rc = walk(table, count, nodes, "", err, errlen);
    free(nodes);
    return rc;
}

/* ---- the object ---------------------------------------------------------------------------- */

fb_fdm_t *fb_fdm_new(fb_fdm_kind_t kind, const char *params_json, size_t params_len, double dt_s,
                     char *err, size_t errlen)
{
    if (err && errlen) err[0] = 0;
    if (!(dt_s > 0) || dt_s > 0.1) { if (err) snprintf(err, errlen, "dt: 0 < dt <= 0.1 s"); return NULL; }
    fb_fdm_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->kind = kind;
    f->dt = dt_s;
    f->rho = 1.225;
    int rc;
    switch (kind) {
    case FB_FDM_HELI: rc = fb_heli_create(f, params_json, params_len, err, errlen); break;
    case FB_FDM_PLANE: rc = fb_plane_create(f, params_json, params_len, err, errlen); break;
    default: rc = -1; if (err) snprintf(err, errlen, "kind: unknown");
    }
    if (rc) { fb_fdm_free(f); return NULL; }
    for (int i = 0; i < f->n_inputs; i++) f->input_us[i] = 1500;
    fb_fdm_reset(f);
    return f;
}

void fb_fdm_free(fb_fdm_t *f)
{
    if (!f) return;
    if (f->ops && f->ops->destroy) f->ops->destroy(f);
    free(f);
}

fb_fdm_kind_t fb_fdm_kind(const fb_fdm_t *f) { return f->kind; }
double fb_fdm_dt(const fb_fdm_t *f) { return f->dt; }

void fb_fdm_reset(fb_fdm_t *f)
{
    fb_body_reset(&f->body);
    f->ev_head = f->ev_count = 0;
    if (f->ops->reset) f->ops->reset(f);
}

void fb_fdm_return_to_pad(fb_fdm_t *f)
{
    fb_body_return_to_pad(&f->body);
    if (f->ops->on_contact) f->ops->on_contact(f);
}

int fb_fdm_input_count(const fb_fdm_t *f) { return f->n_inputs; }
const char *fb_fdm_input_name(const fb_fdm_t *f, int index)
{
    return index >= 0 && index < f->n_inputs ? f->input_names[index] : NULL;
}
int fb_fdm_input_index(const fb_fdm_t *f, const char *actuator)
{
    for (int i = 0; i < f->n_inputs; i++) if (!strcmp(f->input_names[i], actuator)) return i;
    return -1;
}
void fb_fdm_set_input_us(fb_fdm_t *f, int index, uint16_t us)
{
    if (index >= 0 && index < f->n_inputs) f->input_us[index] = us;
}

void fb_fdm_set_motor_rpm(fb_fdm_t *f, int motor, double rpm) { if (f->ops->set_motor_rpm) f->ops->set_motor_rpm(f, motor, rpm); }
void fb_fdm_set_throttle(fb_fdm_t *f, int motor, double thr) { if (f->ops->set_throttle) f->ops->set_throttle(f, motor, thr); }
double fb_fdm_load(const fb_fdm_t *f, int motor) { return f->ops->load ? f->ops->load(f, motor) : 0; }

void fb_fdm_set_air(fb_fdm_t *f, double rho) { if (rho > 0) f->rho = rho; }
void fb_fdm_set_wind(fb_fdm_t *f, const double w[3]) { fb_v3_copy(f->wind, w); }
double fb_fdm_air_density(double pressure_pa, double temp_c)
{
    return pressure_pa / (287.05 * (temp_c + 273.15));
}

void fb_fdm_step(fb_fdm_t *f)
{
    f->ops->step(f);
    if (f->body.crashed_now) {
        fb_fdm_push_event(f, FB_FDM_EV_CRASH, f->body.crash_info[0], f->body.crash_info[1], f->body.crash_info[2], 0);
        f->body.crashed_now = false;
    }
    if (f->body.pose_now) {
        double r, p, y;
        fb_body_euler(&f->body, &r, &p, &y);
        fb_fdm_push_event(f, FB_FDM_EV_POSE, r / FB_DEG, p / FB_DEG, y / FB_DEG, 0);
        f->body.pose_now = false;
    }
}

void fb_fdm_state(const fb_fdm_t *f, fb_fdm_state_t *o)
{
    const fb_body_t *b = &f->body;
    memset(o, 0, sizeof *o);
    o->t = b->t;
    fb_v3_copy(o->pos_ned, b->pos);
    fb_v3_copy(o->vel_ned, b->vel);
    memcpy(o->quat, b->quat, sizeof o->quat);
    fb_v3_copy(o->rates, b->rates);
    fb_v3_copy(o->accel_body, b->accel_body);
    o->on_ground = b->on_ground;
    o->held = b->held;
    o->posed = b->posed;
    o->crashes = b->crashes;
    memcpy(o->crash_info, b->crash_info, sizeof o->crash_info);
}

const void *fb_fdm_state_ext(const fb_fdm_t *f, size_t *size)
{
    if (f->ops->ext) return f->ops->ext(f, size);
    if (size) *size = 0;
    return NULL;
}

void fb_fdm_imu(const fb_fdm_t *f, float gyro_dps[3], float accel_g[3])
{
    for (int i = 0; i < 3; i++) {
        gyro_dps[i] = (float)(f->body.rates[i] / FB_DEG);
        accel_g[i] = (float)(f->body.accel_body[i] / FB_GRAVITY);
    }
}

void fb_fdm_euler(const fb_fdm_t *f, double *roll, double *pitch, double *yaw)
{
    fb_body_euler(&f->body, roll, pitch, yaw);
}

void fb_fdm_hold(fb_fdm_t *f, double roll, double pitch, double yaw, double rotate_s)
{
    double q[4];
    if (isnan(yaw)) fb_body_euler(&f->body, NULL, NULL, &yaw);
    fb_q_from_euler(q, roll, pitch, yaw);
    fb_body_hold_quat(&f->body, q, rotate_s);
}
void fb_fdm_hold_quat(fb_fdm_t *f, const double q[4], double rotate_s) { fb_body_hold_quat(&f->body, q, rotate_s); }
void fb_fdm_release(fb_fdm_t *f) { fb_body_release(&f->body); }
int fb_fdm_launch(fb_fdm_t *f, double speed_mps, double duration_s, double heading, double pitch)
{
    return f->ops->launch ? f->ops->launch(f, speed_mps, duration_s, heading, pitch) : -1;
}
void fb_fdm_set_kinematics(fb_fdm_t *f, const double pos[3], const double vel[3], const double q[4], const double rates[3])
{
    fb_body_set_kinematics(&f->body, pos, vel, q, rates);
}

void fb_fdm_ned_to_geo(double home_lat_deg, double home_lon_deg, const double pos_ned[3], double *lat, double *lon)
{
    const double R = 6371000.0;
    double lat0 = home_lat_deg * FB_DEG;
    if (lat) *lat = home_lat_deg + (pos_ned[0] / R) / FB_DEG;
    if (lon) *lon = home_lon_deg + (pos_ned[1] / (R * cos(lat0))) / FB_DEG;
}
