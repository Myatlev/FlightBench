/* Environment scenario runner. See include/fb/plane_scenario.h. */
#include "fb/plane_scenario.h"
#include "fb/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG (M_PI / 180.0)

void fb_scenario_launch_defaults(fb_sc_launch_t *l)
{
    l->speed_kmh = 50; l->duration_s = 0.5; l->pitch_deg = 10; l->heading_deg = NAN;
    l->heading = FB_SC_HEADING_CURRENT;
}

static void launch_unset(fb_sc_launch_t *l)
{
    l->speed_kmh = l->duration_s = l->pitch_deg = l->heading_deg = NAN;
    l->heading = FB_SC_HEADING_UNSET;
}

void fb_scenario_launch_merge(const fb_sc_launch_t *d, const fb_sc_launch_t *o, fb_sc_launch_t *out)
{
    fb_sc_launch_t r;
    fb_scenario_launch_defaults(&r);
    const fb_sc_launch_t *layer[2] = { d, o };
    for (int i = 0; i < 2; i++) {
        const fb_sc_launch_t *l = layer[i];
        if (!l) continue;
        if (!isnan(l->speed_kmh)) r.speed_kmh = l->speed_kmh;
        if (!isnan(l->duration_s)) r.duration_s = l->duration_s;
        if (!isnan(l->pitch_deg)) r.pitch_deg = l->pitch_deg;
        if (l->heading != FB_SC_HEADING_UNSET) { r.heading = l->heading; r.heading_deg = l->heading_deg; }
    }
    *out = r;
}

int fb_scenario_do_launch(fb_fdm_t *p, const fb_sc_launch_t *l)
{
    double hdg = l->heading == FB_SC_HEADING_VALUE ? l->heading_deg * DEG : NAN;
    return fb_fdm_launch(p, l->speed_kmh / 3.6, l->duration_s, hdg, l->pitch_deg * DEG);
}

/* ---- parsing ------------------------------------------------------------------------------ */

typedef struct { char *err; size_t n; } perr_t;

static int fail(perr_t *e, const char *where, const char *what)
{
    snprintf(e->err, e->n, "%s: %s", where, what);
    return -1;
}

/* optional number: 1 set, 0 absent, -1 wrong type; null counts as absent unless null_ok */
static int opt_num(const fb_json_node_t *o, const char *key, double *out, bool *is_null)
{
    const fb_json_node_t *v = fb_json_get(o, key);
    if (is_null) *is_null = false;
    if (!v) return 0;
    if (v->type == FB_JSON_NULL) { if (is_null) *is_null = true; return 0; }
    if (v->type != FB_JSON_NUMBER) return -1;
    *out = fb_json_number(v);
    return isfinite(*out) ? 1 : -1;
}

static int parse_launch(const fb_json_node_t *o, fb_sc_launch_t *l, perr_t *e, const char *where)
{
    char w[96];
    static const char *const keys[3] = { "speed_kmh", "duration_s", "pitch_deg" };
    double *dst[3] = { &l->speed_kmh, &l->duration_s, &l->pitch_deg };
    for (int i = 0; i < 3; i++) {
        if (opt_num(o, keys[i], dst[i], NULL) < 0) {
            snprintf(w, sizeof w, "%s.%s", where, keys[i]);
            return fail(e, w, "need a number");
        }
    }
    bool nul;
    int r = opt_num(o, "heading_deg", &l->heading_deg, &nul);
    if (r < 0) { snprintf(w, sizeof w, "%s.heading_deg", where); return fail(e, w, "need a number or null"); }
    if (r > 0) l->heading = FB_SC_HEADING_VALUE;
    else if (nul) l->heading = FB_SC_HEADING_CURRENT;
    if ((!isnan(l->speed_kmh) && !(l->speed_kmh > 0 && l->speed_kmh <= 360)) ||
        (!isnan(l->duration_s) && !(l->duration_s > 0 && l->duration_s <= 10)) ||
        (!isnan(l->pitch_deg) && fabs(l->pitch_deg) > 60))
        return fail(e, where, "speed_kmh (0, 360], duration_s (0, 10], |pitch_deg| <= 60");
    return 0;
}

static int copy_str(const fb_json_node_t *o, const char *key, char *buf, size_t n, perr_t *e,
                    const char *where)
{
    const fb_json_node_t *v = fb_json_get(o, key);
    buf[0] = 0;
    if (!v || v->type == FB_JSON_NULL) return 0;
    if (v->type != FB_JSON_STRING || fb_json_str(v, buf, n) >= n) {
        char w[96];
        snprintf(w, sizeof w, "%s.%s", where, key);
        return fail(e, w, "need a string (< 48 bytes)");
    }
    return 0;
}

static int parse_step(const fb_json_node_t *o, fb_sc_step_t *s, perr_t *e, int i)
{
    char w[64];
    snprintf(w, sizeof w, "steps[%d]", i);
    if (!o || o->type != FB_JSON_OBJECT) return fail(e, w, "need an object");
    memset(s, 0, sizeof *s);
    s->index = -1;
    s->rotate_s = 1.0;
    s->euler_deg[2] = NAN;
    launch_unset(&s->launch);

    bool nul;
    const fb_json_node_t *at = fb_json_get(o, "at_s");
    if (!at) return fail(e, w, "at_s missing (a number, or null = on command)");
    int r = opt_num(o, "at_s", &s->at_s, &nul);
    if (r < 0 || (r > 0 && s->at_s < 0)) return fail(e, w, "at_s: need a number >= 0 or null");
    if (nul) s->at_s = NAN;

    char act[16];
    const fb_json_node_t *a = fb_json_get(o, "action");
    if (!a || a->type != FB_JSON_STRING || fb_json_str(a, act, sizeof act) >= sizeof act)
        return fail(e, w, "action: need pad|hold|release|launch|mark");
    if (!strcmp(act, "pad")) s->action = FB_SC_PAD;
    else if (!strcmp(act, "hold")) s->action = FB_SC_HOLD;
    else if (!strcmp(act, "release")) s->action = FB_SC_RELEASE;
    else if (!strcmp(act, "launch")) s->action = FB_SC_LAUNCH;
    else if (!strcmp(act, "mark")) s->action = FB_SC_MARK;
    else return fail(e, w, "action: need pad|hold|release|launch|mark");

    if (s->action == FB_SC_HOLD) {
        const fb_json_node_t *eu = fb_json_get(o, "euler_deg"), *q = fb_json_get(o, "quat");
        if (!eu == !q) return fail(e, w, "hold: need exactly one of euler_deg [roll,pitch,yaw|null] or quat [w,x,y,z]");
        if (eu) {
            if (eu->type != FB_JSON_ARRAY || eu->count != 3) return fail(e, w, "euler_deg: need 3 elements");
            for (int k = 0; k < 3; k++) {
                const fb_json_node_t *v = fb_json_at(eu, k);
                if (k == 2 && v->type == FB_JSON_NULL) { s->euler_deg[2] = NAN; continue; }
                if (v->type != FB_JSON_NUMBER) return fail(e, w, "euler_deg: need numbers (yaw may be null)");
                s->euler_deg[k] = fb_json_number(v);
            }
        } else {
            if (q->type != FB_JSON_ARRAY || q->count != 4) return fail(e, w, "quat: need 4 numbers");
            double n = 0;
            for (int k = 0; k < 4; k++) {
                const fb_json_node_t *v = fb_json_at(q, k);
                if (v->type != FB_JSON_NUMBER) return fail(e, w, "quat: need 4 numbers");
                s->quat[k] = fb_json_number(v);
                n += s->quat[k] * s->quat[k];
            }
            if (!(n > 1e-12)) return fail(e, w, "quat: zero");
            s->has_quat = true;
        }
        r = opt_num(o, "rotate_s", &s->rotate_s, NULL);
        if (r < 0 || s->rotate_s < 0 || s->rotate_s > 60) return fail(e, w, "rotate_s: need 0..60");
    }
    if (s->action == FB_SC_LAUNCH && parse_launch(o, &s->launch, e, w)) return -1;
    if (copy_str(o, "event", s->event, sizeof s->event, e, w)) return -1;
    if (copy_str(o, "label", s->label, sizeof s->label, e, w)) return -1;
    double idx;
    r = opt_num(o, "index", &idx, NULL);
    if (r < 0 || (r > 0 && (idx != floor(idx) || idx < 0 || idx > 1000)))
        return fail(e, w, "index: need an integer 0..1000");
    if (r > 0) s->index = (int)idx;
    return 0;
}

int fb_scenario_parse(fb_scenario_t *sc, const char *json, size_t len, char *err, size_t errlen)
{
    perr_t e = { err, errlen };
    memset(sc, 0, sizeof *sc);
    sc->pending = -1;
    fb_scenario_launch_defaults(&sc->launch);
    int n = fb_json_parse(json, len, NULL, 0, err, errlen);
    if (n < 0) return -1;
    fb_json_node_t *nodes = malloc(sizeof(*nodes) * (size_t)n);
    if (!nodes) return fail(&e, "scenario", "out of memory");
    int rc = -1;
    fb_json_parse(json, len, nodes, n, err, errlen);
    const fb_json_node_t *root = nodes;
    if (root->type != FB_JSON_OBJECT) { fail(&e, "scenario", "need an object"); goto out; }
    if (copy_str(root, "name", sc->name, sizeof sc->name, &e, "scenario")) goto out;
    const fb_json_node_t *l = fb_json_get(root, "launch");
    if (l) {
        fb_sc_launch_t o;
        launch_unset(&o);
        if (l->type != FB_JSON_OBJECT) { fail(&e, "launch", "need an object"); goto out; }
        if (parse_launch(l, &o, &e, "launch")) goto out;
        fb_scenario_launch_merge(NULL, &o, &sc->launch);
    }
    const fb_json_node_t *st = fb_json_get(root, "steps");
    if (!st || st->type != FB_JSON_ARRAY) { fail(&e, "steps", "need an array"); goto out; }
    if (st->count > FB_SCENARIO_MAX_STEPS) { fail(&e, "steps", "at most 32"); goto out; }
    double last = 0;
    for (int i = 0; i < st->count; i++) {
        if (parse_step(fb_json_at(st, i), &sc->steps[i], &e, i)) goto out;
        if (!isnan(sc->steps[i].at_s)) {
            if (sc->steps[i].at_s < last) {
                char w[32];
                snprintf(w, sizeof w, "steps[%d]", i);
                fail(&e, w, "at_s must not go back in time");
                goto out;
            }
            last = sc->steps[i].at_s;
        }
    }
    sc->n = st->count;
    rc = 0;
out:
    free(nodes);
    return rc;
}

/* ---- runner ------------------------------------------------------------------------------- */

static void emit(fb_scenario_t *sc, const char *name, const char *label, int index, int step,
                 double t)
{
    int i = (sc->ev_head + sc->ev_count) % 16;
    if (sc->ev_count == 16) { sc->ev_head = (sc->ev_head + 1) % 16; sc->ev_count--; }
    snprintf(sc->ev[i].name, sizeof sc->ev[i].name, "%s", name);
    snprintf(sc->ev[i].label, sizeof sc->ev[i].label, "%s", label);
    sc->ev[i].index = index; sc->ev[i].step = step; sc->ev[i].t = t;
    sc->ev_count++;
}

bool fb_scenario_poll_event(fb_scenario_t *sc, fb_sc_event_t *ev)
{
    if (!sc->ev_count) return false;
    *ev = sc->ev[sc->ev_head];
    sc->ev_head = (sc->ev_head + 1) % 16;
    sc->ev_count--;
    return true;
}

static void fire(fb_scenario_t *sc, fb_fdm_t *p, int i, const fb_sc_launch_t *o, double t)
{
    const fb_sc_step_t *s = &sc->steps[i];
    fb_sc_launch_t base, l;
    switch (s->action) {
    case FB_SC_PAD: fb_fdm_return_to_pad(p); break;
    case FB_SC_HOLD:
        if (s->has_quat) fb_fdm_hold_quat(p, s->quat, s->rotate_s);
        else fb_fdm_hold(p, s->euler_deg[0] * DEG, s->euler_deg[1] * DEG,
                           isnan(s->euler_deg[2]) ? NAN : s->euler_deg[2] * DEG, s->rotate_s);
        break;
    case FB_SC_RELEASE: fb_fdm_release(p); break;
    case FB_SC_LAUNCH:
        fb_scenario_launch_merge(&sc->launch, &s->launch, &base);
        fb_scenario_launch_merge(&base, o, &l);
        fb_scenario_do_launch(p, &l);
        break;
    case FB_SC_MARK: break;
    }
    if (s->action == FB_SC_HOLD) sc->pending = i;           /* its event when the pose is reached */
    else if (s->event[0]) emit(sc, s->event, s->label, s->index, i, t);
    sc->next = i + 1;
}

static void check_pending(fb_scenario_t *sc, fb_fdm_t *p, double t)
{
    if (sc->pending < 0) return;
    fb_fdm_state_t st;
    fb_fdm_state(p, &st);
    if (!st.held) { sc->pending = -1; return; }            /* released before it got there */
    if (st.posed) {
        const fb_sc_step_t *s = &sc->steps[sc->pending];
        if (s->event[0]) emit(sc, s->event, s->label, s->index, sc->pending, t);
        sc->pending = -1;
    }
}

static void run_due(fb_scenario_t *sc, fb_fdm_t *p)
{
    fb_fdm_state_t st;
    fb_fdm_state(p, &st);
    double t = st.t - sc->t0;
    while (sc->running && sc->next < sc->n) {
        const fb_sc_step_t *s = &sc->steps[sc->next];
        if (isnan(s->at_s) || s->at_s > t + 1e-9) break;
        check_pending(sc, p, st.t);   /* a hold superseded at the same instant reports first */
        fire(sc, p, sc->next, NULL, st.t);
    }
    check_pending(sc, p, st.t);
    if (sc->running && sc->next >= sc->n && sc->pending < 0 && !sc->ended) {
        sc->ended = true;
        emit(sc, "scenario_end", sc->name, -1, sc->n, st.t);
    }
}

void fb_scenario_start(fb_scenario_t *sc, fb_fdm_t *p)
{
    fb_fdm_state_t st;
    fb_fdm_state(p, &st);
    sc->t0 = st.t;
    sc->next = 0;
    sc->pending = -1;
    sc->ended = false;
    sc->running = true;
    sc->ev_head = sc->ev_count = 0;
    run_due(sc, p);
}

void fb_scenario_stop(fb_scenario_t *sc) { sc->running = false; }

void fb_scenario_tick(fb_scenario_t *sc, fb_fdm_t *p)
{
    if (sc->running) run_due(sc, p);
}

int fb_scenario_command(fb_scenario_t *sc, fb_fdm_t *p, fb_sc_action_t action,
                        const fb_sc_launch_t *o)
{
    if (!sc->running || sc->next >= sc->n) return 0;
    const fb_sc_step_t *s = &sc->steps[sc->next];
    if (!isnan(s->at_s) || s->action != action) return 0;
    fb_fdm_state_t st;
    fb_fdm_state(p, &st);
    check_pending(sc, p, st.t);
    fire(sc, p, sc->next, o, st.t);
    /* the steps behind it count from now on (at_s is relative to the scenario start; a later
     * step whose time has passed fires at the next tick) */
    return 1;
}
