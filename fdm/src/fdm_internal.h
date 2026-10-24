/* FlightBench FDM: the object behind fb_fdm_t and what a model type provides. Private to fdm/src. */
#ifndef FB_FDM_INTERNAL_H
#define FB_FDM_INTERNAL_H

#include "fb/body.h"
#include "fb/fdm.h"
#include "fb/json.h"

#define FB_FDM_MAX_EVENTS 8

typedef struct fb_fdm_ops {
    void (*reset)(fb_fdm_t *f);                       /* model state (the body is reset by the core) */
    void (*step)(fb_fdm_t *f);                        /* one dt */
    void (*set_motor_rpm)(fb_fdm_t *f, int motor, double rpm);
    void (*set_throttle)(fb_fdm_t *f, int motor, double thr);
    double (*load)(const fb_fdm_t *f, int motor);
    const void *(*ext)(const fb_fdm_t *f, size_t *size);
    int (*launch)(fb_fdm_t *f, double speed_mps, double duration_s, double heading, double pitch);
    void (*on_contact)(fb_fdm_t *f);                  /* a ground contact began (reset rotor states) */
    void (*destroy)(fb_fdm_t *f);
} fb_fdm_ops_t;

struct fb_fdm {
    fb_fdm_kind_t kind;
    double dt;
    const fb_fdm_ops_t *ops;
    void *model;
    fb_body_t body;
    int n_inputs;
    const char *input_names[FB_FDM_MAX_INPUTS];
    uint16_t input_us[FB_FDM_MAX_INPUTS];
    double rho;
    double wind[3];
    fb_fdm_event_t events[FB_FDM_MAX_EVENTS];
    int ev_head, ev_count;
};

void fb_fdm_push_event(fb_fdm_t *f, fb_fdm_event_kind_t kind, double a, double b, double c, double d);

/* ---- parameter loading: a table of paths ("rotor.radius_m") -> doubles, arrays and names ------ */
typedef enum { FB_P_NUM, FB_P_VEC, FB_P_NAMES, FB_P_ENUM } fb_param_kind_t;
typedef struct {
    const char *path;
    fb_param_kind_t kind;
    void *dst;                /* double*, double[n], const char *[n] (names), or int* (enum) */
    int n;                    /* vector / names length */
    double lo, hi;            /* numbers: inclusive range */
    const char *const *choices;   /* enum: NULL-terminated list; the index is stored */
} fb_param_t;
/* Walks the object text; every leaf must be in the table (nested objects build the path with '.').
 * Returns 0, or -1 with err = "path: what". names: each element is strdup'ed into dst. */
int fb_params_load(const fb_param_t *table, size_t count, const char *json, size_t len, char *err,
                   size_t errlen);

/* the types */
int fb_heli_create(fb_fdm_t *f, const char *json, size_t len, char *err, size_t errlen);
int fb_plane_create(fb_fdm_t *f, const char *json, size_t len, char *err, size_t errlen);

#endif
