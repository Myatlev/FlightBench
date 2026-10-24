/* FlightBench environment scenarios: a timed list of constraint/launch steps, as data.
 *
 * A scenario is the `scenario` object of the `env` command, or a file models/<id>/env/<name>.json
 * (docs/protocol.md "env.scenario"). The runner drives an fb_plane through its constraint API:
 *
 *   {"name":"acc-calibration", "description":"...",
 *    "launch":{"speed_kmh":50,"duration_s":0.5,"pitch_deg":10,"heading_deg":null},
 *    "steps":[{"at_s":0, "action":"pad"},
 *             {"at_s":0.5, "action":"hold", "euler_deg":[0,0,null], "rotate_s":1,
 *              "event":"pose", "label":"level", "index":0},
 *             {"at_s":null, "action":"launch"}, ...]}
 *
 * Actions: "pad" (fb_fdm_return_to_pad), "hold" (fb_fdm_hold: euler_deg [roll, pitch, yaw]
 * in degrees, yaw null = keep the heading, or "quat" [w,x,y,z]; rotate_s default 1), "release",
 * "launch" (the scenario's "launch" object, overridden by the step's own fields), "mark" (only the
 * event). Steps fire in order: a step fires once the previous one fired and t - start >= at_s;
 * "at_s": null waits for a command (fb_scenario_command, e.g. the client's `launch`). "event"
 * (a name) is reported when the step completes: a hold when the pose is reached (the IMU then
 * reads it), the others when they fire. After the last step: event "scenario_end".
 */
#ifndef FB_PLANE_SCENARIO_H
#define FB_PLANE_SCENARIO_H

#include <stdbool.h>
#include <stddef.h>

#include "fb/fdm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FB_SCENARIO_MAX_STEPS 32
#define FB_SCENARIO_NAME 48

typedef enum { FB_SC_PAD, FB_SC_HOLD, FB_SC_RELEASE, FB_SC_LAUNCH, FB_SC_MARK } fb_sc_action_t;

/* Launch parameters; NAN = not given (take the default). heading: FB_SC_HEADING_* */
enum { FB_SC_HEADING_UNSET, FB_SC_HEADING_CURRENT, FB_SC_HEADING_VALUE };
typedef struct {
    double speed_kmh, duration_s, pitch_deg, heading_deg;
    int heading;
} fb_sc_launch_t;

typedef struct {
    double at_s;                        /* NAN = on command */
    fb_sc_action_t action;
    double euler_deg[3];                /* hold; euler_deg[2] NAN = keep heading */
    double quat[4];
    bool has_quat;
    double rotate_s;
    fb_sc_launch_t launch;
    char event[FB_SCENARIO_NAME];       /* "" = none */
    char label[FB_SCENARIO_NAME];
    int index;                          /* -1 = none */
} fb_sc_step_t;

typedef struct {
    char name[FB_SCENARIO_NAME];        /* the step's event name, or "scenario_end" */
    char label[FB_SCENARIO_NAME];
    int index;                          /* the step's "index", -1 when none */
    int step;
    double t;
} fb_sc_event_t;

typedef struct {
    char name[FB_SCENARIO_NAME];
    fb_sc_launch_t launch;              /* defaults for launch steps and commands */
    fb_sc_step_t steps[FB_SCENARIO_MAX_STEPS];
    int n;
    /* runner */
    bool running;
    double t0;
    int next;                           /* next step to fire */
    int pending;                        /* a fired hold waiting for its pose, -1 = none */
    bool ended;
    fb_sc_event_t ev[16];
    int ev_head, ev_count;
} fb_scenario_t;

/* Parse a scenario object (JSON text). 0 on success; -1 with a message ("steps[2].action: ...").
 * Needs fb_json_parse (devices/src/json.c; the core links both libraries). */
int fb_scenario_parse(fb_scenario_t *sc, const char *json, size_t len, char *err, size_t errlen);
/* The launch defaults (50 km/h, 0.5 s, pitch 10 deg, current heading) filled into l. */
void fb_scenario_launch_defaults(fb_sc_launch_t *l);
/* Resolve a launch: overrides o on top of the defaults d (either may be NULL). */
void fb_scenario_launch_merge(const fb_sc_launch_t *d, const fb_sc_launch_t *o, fb_sc_launch_t *out);
/* Apply a resolved launch to the plane (fb_fdm_launch); returns its result. */
int fb_scenario_do_launch(fb_fdm_t *p, const fb_sc_launch_t *l);

void fb_scenario_start(fb_scenario_t *sc, fb_fdm_t *p);   /* t0 = the plane's time; fires due steps */
void fb_scenario_stop(fb_scenario_t *sc);
/* Call after every fb_fdm_step. */
void fb_scenario_tick(fb_scenario_t *sc, fb_fdm_t *p);
/* A client command for the step waiting on it (at_s null, same action). For "launch" the
 * overrides o apply on top of the step's and the scenario's parameters. Returns 1 when a waiting
 * step consumed it, 0 when no step waits for it (the core then applies the command itself). */
int fb_scenario_command(fb_scenario_t *sc, fb_fdm_t *p, fb_sc_action_t action,
                        const fb_sc_launch_t *o);
bool fb_scenario_poll_event(fb_scenario_t *sc, fb_sc_event_t *ev);

#ifdef __cplusplus
}
#endif
#endif
