/*
 * FlightBench: the flight dynamics model in the STM32 machines (fdm/include/fb/fdm.h, docs/fdm.md).
 *
 * Included by nexus_xr.c after the control server (nexus_xr_ctl.c.inc) and the aircraft-file
 * helpers; the machine calls these through the prototypes next to its ctl hooks. One object for
 * every aircraft type (the aircraft file's "fdm": heli, plane): the environment (wind, air, the
 * scenarios, the holder, the launch) is the same code for all of them.
 *
 * Per physics tick (1 kHz, fdm_tick): the servo pulses by actuator name (the wiring's map and
 * `reversed`) -> the FDM's inputs; the motor speed the ESC model owns -> fb_fdm_step ->
 * fb_esc_set_load(the shaft load) -> the env scenario -> events -> the IMU motion source through
 * the board's IMU alignment (board->imu_align: the chip frame the firmware's alignment turns back
 * into its board frame X forward, Y left, Z up).
 *
 * Commands (docs/protocol.md): `launch` (control and pilot socket; a type with launches),
 * `constraint`, `env` (wind_ned, pressure_pa/temp_c -> air density, home, scenario), `reset aircraft`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* ---- the aircraft file's FDM part (aircraft_load) ----------------------------------------- */

/* "fdm_params" as JSON text for fb_fdm_new (NULL = absent: the type's defaults) */
static char *fdm_params_text(NexusXRState *s, QDict *a)
{
    QObject *o = qdict_get(a, "fdm_params");
    if (!o) {
        return NULL;
    }
    if (!qobject_to(QDict, o)) {
        aj_fail(s, "fdm_params", "an object");
    }
    g_autoptr(GString) j = qobject_to_json(o);
    return g_strdup(j->str);
}

static char *env_rel_path(NexusXRState *s, QDict *a, const char *key)
{
    QObject *o = qdict_get(a, key);
    if (!o || qobject_type(o) == QTYPE_QNULL) {
        return NULL;
    }
    QString *q = qobject_to(QString, o);
    if (!q) {
        aj_fail(s, key, "a path (relative to the aircraft's directory)");
    }
    return g_strdup(qstring_get_str(q));
}

/* A session-check reads the aircraft file and builds the FDM once, so a wrong parameter is
 * reported with the bench's check and not at the launch (fb_fdm_new names the parameter). */
static void fdm_params_check(NexusXRState *s, const char *aircraft_file)
{
    g_autofree char *text = NULL;
    g_autoptr(GError) gerr = NULL;
    Error *err = NULL;
    if (!aircraft_file || !*aircraft_file) {
        return;
    }
    if (!g_file_get_contents(aircraft_file, &text, NULL, &gerr)) {
        return;                     /* the resolver reports the file itself */
    }
    QObject *o = qobject_from_json(text, &err);
    QDict *a = o ? qobject_to(QDict, o) : NULL;
    if (!a) {
        qobject_unref(o);
        return;
    }
    const char *kind = qdict_get_try_str(a, "fdm");
    fb_fdm_kind_t k = kind && !strcmp(kind, "plane") ? FB_FDM_PLANE : FB_FDM_HELI;
    QObject *po = qdict_get(a, "fdm_params");
    g_autofree char *params = NULL;
    if (po && qobject_to(QDict, po)) {
        g_autoptr(GString) j = qobject_to_json(po);
        params = g_strdup(j->str);
    } else if (po) {
        fprintf(stderr, "session: aircraft %s: fdm_params: an object\n", aircraft_file);
        qobject_unref(o);
        exit(2);
    }
    char e[200];
    fb_fdm_t *f = fb_fdm_new(k, params, params ? strlen(params) : 0, FDM_DT_NS / 1e9, e, sizeof e);
    if (!f) {
        fprintf(stderr, "session: aircraft %s: fdm_params.%s\n", aircraft_file, e);
        qobject_unref(o);
        exit(2);
    }
    fb_fdm_free(f);
    qobject_unref(o);
}

/* ---- scenarios -------------------------------------------------------------------------- */

/* a scenario by name (env/<name>.json next to the aircraft; a name ending in .json is a path
 * relative to the aircraft's directory) into sc; false + errp */
static bool env_scenario_file(NexusXRState *s, const char *name, fb_scenario_t *sc,
                              char **text_out, Error **errp)
{
    FbEnv *ev = s->env;
    g_autofree char *rel = g_str_has_suffix(name, ".json") ? g_strdup(name)
                         : g_strdup_printf("env/%s.json", name);
    g_autofree char *path = g_path_is_absolute(rel) ? g_strdup(rel)
                          : g_build_filename(ev->dir, rel, NULL);
    g_autofree char *text = NULL;
    g_autoptr(GError) ge = NULL;
    gsize len;
    if (strstr(name, "..") || !g_file_get_contents(path, &text, &len, &ge)) {
        error_setg(errp, "env.scenario %s: %s", name, ge ? ge->message : "not a scenario name");
        return false;
    }
    char e[160];
    if (fb_scenario_parse(sc, text, len, e, sizeof e)) {
        error_setg(errp, "env.scenario %s: %s", rel, e);
        return false;
    }
    if (text_out) {
        *text_out = g_steal_pointer(&text);
    }
    return true;
}

static void env_scenario_run(NexusXRState *s, const fb_scenario_t *sc)
{
    FbEnv *ev = s->env;
    ev->sc = *sc;
    ev->sc_gen++;
    fb_scenario_start(&ev->sc, s->fdm);
    ctl_log(s, "core", "env: scenario %s started (%d steps)", ev->sc.name, ev->sc.n);
}

/* the aircraft's env_default (after start and `reset aircraft`); a bad file is only logged */
static void env_default_run(NexusXRState *s)
{
    FbEnv *ev = s->env;
    Error *err = NULL;
    fb_scenario_t sc;
    fb_scenario_stop(&ev->sc);
    if (!ev->env_default) {
        return;
    }
    if (env_scenario_file(s, ev->env_default, &sc, NULL, &err)) {
        env_scenario_run(s, &sc);
    } else {
        ctl_log(s, "core", "env_default: %s", error_get_pretty(err));
        error_free(err);
    }
}

/* ---- init / reset ----------------------------------------------------------------------- */

static void env_init(NexusXRState *s)
{
    FbEnv *ev = g_new0(FbEnv, 1);
    s->env = ev;
    ev->dir = s->aircraft_file ? g_path_get_dirname(s->aircraft_file) : g_strdup(".");
    ev->env_default = s->env_scenario && *s->env_scenario ? g_strdup(s->env_scenario)
                    : g_steal_pointer(&s->env_default_path);
    /* the functions' scenarios (the ACC calibration's poses): parsed now, the text is part of
     * the first-start snapshot's key */
    for (int i = 0; i < s->nfn; i++) {
        FbFunction *f = &s->fn[i];
        fb_scenario_t sc;
        Error *err = NULL;
        if (f->scenario && !env_scenario_file(s, f->scenario, &sc, &f->scenario_text, &err)) {
            error_report("%s: aircraft-file %s: functions %s: %s", fb_mname, s->aircraft_file,
                         f->name, error_get_pretty(err));
            exit(1);
        }
        if (f->scenario && !strcmp(f->name, "calibrate_acc")) {
            ev->acc_cal = g_strdup(f->scenario);
        }
    }
    fb_scenario_stop(&ev->sc);
}

/* `reset aircraft`: back on the pad (fw: at once, the MCU restarts too; else a rotation the IMU
 * follows), then the env_default again */
static void env_reset(NexusXRState *s, bool fw)
{
    FbEnv *ev = s->env;
    if (fw) {
        fb_fdm_reset(s->fdm);
    } else {
        fb_fdm_return_to_pad(s->fdm);
    }
    ev->cmd_hold = false;
    env_default_run(s);
}

/* ---- the physics tick ------------------------------------------------------------------- */

/* FRD -> the firmware's board frame (x, -y, -z) -> the chip, undoing the board's IMU alignment
 * (iNav/Betaflight sensor_align_e, boardalignment.c applySensorAlignment: board = align(chip)) */
static void frd_to_chip(int align, const float in[3], float out[3])
{
    const float bx = in[0], by = -in[1], bz = -in[2];
    switch (align) {
    case FB_ALIGN_CW270:            /* board = (-cy, cx, cz) */
        out[0] = by, out[1] = -bx, out[2] = bz;
        break;
    case FB_ALIGN_CW180:            /* board = (-cx, -cy, cz) */
        out[0] = -bx, out[1] = -by, out[2] = bz;
        break;
    case FB_ALIGN_CW90:             /* board = (cy, -cx, cz) */
        out[0] = -by, out[1] = bx, out[2] = bz;
        break;
    default:                        /* CW0 */
        out[0] = bx, out[1] = by, out[2] = bz;
        break;
    }
}

static void env_events(NexusXRState *s)
{
    FbEnv *ev = s->env;
    fb_fdm_event_t e;
    fb_sc_event_t se;
    while (fb_fdm_poll_event(s->fdm, &e)) {
        g_autoptr(GString) d = g_string_new(NULL);
        const char *name = NULL;
        switch (e.kind) {
        case FB_FDM_EV_LAUNCH_START:
            name = "launch";
            g_string_append(d, ",\"phase\":\"start\"");
            js_kv_num(d, "a_mss", round(e.v[0] * 100) / 100);
            js_kv_num(d, "speed_mps", round(e.v[1] * 100) / 100);
            js_kv_num(d, "heading_deg", round(e.v[2] * 10) / 10);
            js_kv_num(d, "pitch_deg", round(e.v[3] * 10) / 10);
            ev->launches++;
            ctl_fn_launch(s, false, e.v[1], 0);
            break;
        case FB_FDM_EV_LAUNCH_RELEASE:
            name = "launch";
            g_string_append(d, ",\"phase\":\"release\"");
            js_kv_num(d, "speed_mps", round(e.v[0] * 100) / 100);
            js_kv_num(d, "height_m", round(e.v[1] * 100) / 100);
            ctl_fn_launch(s, true, e.v[0], e.v[1]);
            break;
        case FB_FDM_EV_POSE:
            if (!ev->cmd_hold) {
                continue;           /* a scenario's hold: the step reports its own event */
            }
            ev->cmd_hold = false;
            name = "pose";
            js_kv_num(d, "roll_deg", round(e.v[0] * 10) / 10);
            js_kv_num(d, "pitch_deg", round(e.v[1] * 10) / 10);
            js_kv_num(d, "yaw_deg", round(e.v[2] * 10) / 10);
            break;
        case FB_FDM_EV_CRASH:
            name = "crash";
            js_kv_num(d, "roll_deg", round(e.v[0] * 10) / 10);
            js_kv_num(d, "pitch_deg", round(e.v[1] * 10) / 10);
            js_kv_num(d, "speed_mps", round(e.v[2] * 100) / 100);
            break;
        case FB_FDM_EV_MIS_MIX:
            name = "mix_warning";
            js_kv_num(d, "left", round(e.v[0] * 1000) / 1000);
            js_kv_num(d, "right", round(e.v[1] * 1000) / 1000);
            break;
        }
        if (name) {
            ctl_event(s, name, d->len ? d->str + 1 : "");
        }
    }
    while (fb_scenario_poll_event(&ev->sc, &se)) {
        g_autoptr(GString) d = g_string_new(NULL);
        if (!strcmp(se.name, "scenario_end")) {
            g_string_append(d, ",\"scenario\":");
            js_str(d, ev->sc.name);
        } else {
            g_string_append(d, ",\"label\":");
            js_str(d, se.label);
            if (se.index >= 0) {
                g_string_append_printf(d, ",\"index\":%d", se.index);
            }
            g_string_append_printf(d, ",\"step\":%d,\"scenario\":", se.step);
            js_str(d, ev->sc.name);
        }
        ctl_event(s, se.name, d->str + 1);
        ctl_scenario_event(s, &se);
    }
}

static void fdm_log_header(NexusXRState *s)
{
    if (s->fdm_kind == FB_FDM_PLANE) {
        fprintf(s->fdm_log, "t,alt,vz,airspeed,alpha,beta,roll,pitch,yaw,p,q,r,pos_n,pos_e,vel_n,vel_e,"
                "ax,ay,az,motor_rpm,throttle,esc_value,motor_pulse_us,thrust,in1,in2,in3,in4,"
                "surf1,surf2,surf3,surf4,on_ground,held,launch,fc_armed,fc_mode,fc_flags,fc_roll,"
                "fc_pitch,fc_yaw,batt_v,batt_a,batt_mah,fc_gps_fix,fc_gps_sats,fc_lat,fc_lon,fc_baro_alt\n");
    } else {
        fprintf(s->fdm_log, "t,alt,vz,head_rpm,motor_rpm,throttle,dshot,roll,pitch,"
                "yaw,servo1,servo2,servo3,servo4,coll,on_ground,in1,in2,in3,in4,baro_reg_pa,"
                "fc_baro_alt\n");
    }
}

static void fdm_log_line(NexusXRState *s, const fb_fdm_state_t *st)
{
    NxCtl *ctl = s->ctl;
    double roll, pitch, yaw;
    fb_fdm_euler(s->fdm, &roll, &pitch, &yaw);
    if (s->fdm_kind != FB_FDM_PLANE) {
        const fb_fdm_heli_ext_t *h = fb_fdm_state_ext(s->fdm, NULL);
        fprintf(s->fdm_log, "%.3f,%.3f,%.3f,%.1f,%.0f,%.3f,%u,%.2f,%.2f,%.2f,"
                "%u,%u,%u,%u,%.2f,%d,%u,%u,%u,%u,%.2f,%.2f\n",
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, -st->pos_ned[2],
                -st->vel_ned[2], h->rotor_rpm, fb_esc_rpm(s->esc), fb_esc_throttle(s->esc),
                s->dshot_value, roll * 180 / G_PI, pitch * 180 / G_PI, yaw * 180 / G_PI,
                s->servo_us[0], s->servo_us[1], s->servo_us[2], s->servo_us[3],
                h->collective_deg, st->on_ground,
                s->fdm_in_us[0], s->fdm_in_us[1], s->fdm_in_us[2], s->fdm_in_us[3],
                s->baro_present ? fb_dps310_expected_pressure_pa(s->baro) : NAN, ctl_fc_baro_alt(s));
        return;
    }
    const fb_fdm_plane_ext_t *pe = fb_fdm_state_ext(s->fdm, NULL);
    double defl = fb_fdm_plane_params(s->fdm)->surface_deflection_deg;
    char mode[256] = "";
    if (ctl) {
        ctl_mode_str(ctl, mode, sizeof(mode));
    }
    fprintf(s->fdm_log, "%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f,%.1f,%.3f,%.3f,"
            "%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.3f,%u,%.1f,%.2f,%u,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,"
            "%d,%d,%d,%d,%s,%08x,",
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, -st->pos_ned[2], -st->vel_ned[2],
            pe->airspeed, pe->alpha * 180 / G_PI, pe->beta * 180 / G_PI,
            roll * 180 / G_PI, pitch * 180 / G_PI, yaw * 180 / G_PI,
            st->rates[0] * 180 / G_PI, st->rates[1] * 180 / G_PI, st->rates[2] * 180 / G_PI,
            st->pos_ned[0], st->pos_ned[1], st->vel_ned[0], st->vel_ned[1],
            st->accel_body[0], st->accel_body[1], st->accel_body[2],
            fb_esc_rpm(s->esc), fb_esc_throttle(s->esc), s->dshot_value,
            s->motor_pulse_ns / 1e3, pe->thrust_n,
            s->fdm_in_us[0], s->fdm_in_us[1], s->fdm_in_us[2], s->fdm_in_us[3],
            pe->surface[0] * defl, pe->surface[1] * defl, pe->surface[2] * defl, pe->surface[3] * defl,
            st->on_ground, st->held, pe->launch, ctl && ctl->armed, mode[0] ? mode : "-",
            ctl ? ctl->arm_flags : 0);
    if (ctl && ctl->have_att) {
        fprintf(s->fdm_log, "%.1f,%.1f,%d,", ctl->att[0] / 10.0, ctl->att[1] / 10.0, ctl->att[2]);
    } else {
        fprintf(s->fdm_log, ",,,");
    }
    /* the battery (the pack's truth), what the firmware says of its GPS and baro (nan: none) */
    bool b = s->battery_present, g = ctl && ctl->have_gps, a = ctl && ctl->have_alt;
    fprintf(s->fdm_log, "%.3f,%.3f,%.2f,%.0f,%.0f,%.7f,%.7f,%.2f\n",
            b ? fb_battery_voltage(s->batt) : NAN, b ? fb_battery_current_a(s->batt) : NAN,
            b ? fb_battery_mah_used(s->batt) : NAN, g ? ctl->gps_fix : NAN, g ? ctl->gps_sats : NAN,
            g ? ctl->gps_lat / 1e7 : NAN, g ? ctl->gps_lon / 1e7 : NAN, a ? ctl->alt_cm / 100.0 : NAN);
}

static void fdm_model_tick(NexusXRState *s)
{
    float g[3], a[3];
    int n = fb_fdm_input_count(s->fdm);

    PROF_ENTER(pa.fdm_step);
    /* the FDM's inputs by actuator: the output wired to it, mirrored around the input's centre
     * for a reversed linkage; an input nothing is wired to holds 1500 */
    for (int k = 0; k < n && k < 4; k++) {
        int i = s->fdm_src[k];
        uint16_t us = 1500;
        if (i >= 0) {
            us = s->servo_rev >> i & 1 ? 2 * s->fdm_centre_us[k] - s->servo_us[i] : s->servo_us[i];
        }
        s->fdm_in_us[k] = us;
        fb_fdm_set_input_us(s->fdm, k, us);
    }
    double rpm = fb_esc_rpm(s->esc);
    fb_fdm_set_motor_rpm(s->fdm, 0, s->fdm_kind == FB_FDM_PLANE ? rpm : rpm / s->main_gear);
    fb_fdm_step(s->fdm);
    fb_esc_set_load(s->esc, fb_fdm_load(s->fdm, 0));
    fb_scenario_tick(&s->env->sc, s->fdm);
    env_events(s);

    fb_fdm_imu(s->fdm, g, a);
    frd_to_chip(s->board->imu_align, g, s->motion.gyro_dps);
    frd_to_chip(s->board->imu_align, a, s->motion.accel_g);

    /* the baro's air and the GPS's position (the compass reads the attitude at its transfers) */
    fb_fdm_state_t st;
    fb_fdm_state(s->fdm, &st);
    baro_environment(s, -st.pos_ned[2]);
    gps_motion(s, st.pos_ned, st.vel_ned);

    s->fdm_steps++;
    if (s->fdm_log && s->fdm_steps % s->fdm_log_ms == 0) {
        fdm_log_line(s, &st);
    }
    PROF_LEAVE(pa.fdm_step);
}

/* ---- state for the control server --------------------------------------------------------- */

static void fdm_snapshot(NexusXRState *s, double *v)
{
    fb_fdm_state_t st;
    double r, p, y;
    fb_fdm_state(s->fdm, &st);
    fb_fdm_euler(s->fdm, &r, &p, &y);
    v[SIG_FDM_POS_N] = st.pos_ned[0];
    v[SIG_FDM_POS_E] = st.pos_ned[1];
    v[SIG_FDM_POS_D] = st.pos_ned[2];
    v[SIG_FDM_ALT] = -st.pos_ned[2];
    for (int i = 0; i < 3; i++) {
        v[SIG_FDM_VEL_N + i] = st.vel_ned[i];
        v[SIG_FDM_P + i] = st.rates[i] * 180 / G_PI;
        v[SIG_FDM_AX + i] = st.accel_body[i];
    }
    for (int i = 0; i < 4; i++) {
        v[SIG_FDM_QW + i] = st.quat[i];
    }
    v[SIG_FDM_ROLL] = r * 180 / G_PI;
    v[SIG_FDM_PITCH] = p * 180 / G_PI;
    v[SIG_FDM_YAW] = y * 180 / G_PI;
    v[SIG_FDM_ON_GROUND] = st.on_ground;
    v[SIG_ESC_LOAD] = fb_fdm_load(s->fdm, 0);
    if (s->fdm_kind == FB_FDM_PLANE) {
        const fb_fdm_plane_ext_t *pe = fb_fdm_state_ext(s->fdm, NULL);
        v[SIG_FDM_COLL] = NAN;
        v[SIG_FDM_THRUST] = pe->thrust_n;
        v[SIG_HEAD_RPM] = pe->motor_rpm;                /* the prop */
        v[SIG_FDM_AIRSPEED] = pe->airspeed;
        v[SIG_FDM_ALPHA] = pe->alpha * 180 / G_PI;
        v[SIG_FDM_BETA] = pe->beta * 180 / G_PI;
    } else {
        const fb_fdm_heli_ext_t *h = fb_fdm_state_ext(s->fdm, NULL);
        v[SIG_FDM_COLL] = h->collective_deg;
        v[SIG_FDM_THRUST] = h->thrust_n;
        v[SIG_HEAD_RPM] = fb_esc_rpm(s->esc) / s->main_gear;
        v[SIG_FDM_AIRSPEED] = NAN;
        v[SIG_FDM_ALPHA] = NAN;
        v[SIG_FDM_BETA] = NAN;
    }
}

/* the state message's "plane" / "heli" object */
static void fdm_state_json(NexusXRState *s, GString *g)
{
    FbEnv *ev = s->env;
    if (s->fdm_kind != FB_FDM_PLANE) {
        /* the helicopter FDM's own numbers (docs/protocol.md "state"): the blade collective pitch
         * it flies with (from the swash servos through the plate) and the rotor thrust */
        const fb_fdm_heli_ext_t *h = fb_fdm_state_ext(s->fdm, NULL);
        g_string_append(g, ",\"heli\":{\"coll_deg\":");
        js_num(g, round(h->collective_deg * 100) / 100);
        g_string_append(g, ",\"thrust_n\":");
        js_num(g, round(h->thrust_n * 10) / 10);
        g_string_append(g, ",\"torque_nm\":");
        js_num(g, round(h->torque_nm * 100) / 100);
        g_string_append(g, ",\"tail_pitch_deg\":");
        js_num(g, round(h->tail_pitch_deg * 10) / 10);
        g_string_append(g, ",\"wind_ned\":");
        js_vec(g, ev->wind, 3);
        g_string_append_c(g, '}');
        return;
    }
    static const char *const launch[] = { "none", "align", "ramp" };
    fb_fdm_state_t st;
    const fb_fdm_plane_ext_t *pe = fb_fdm_state_ext(s->fdm, NULL);
    double defl = fb_fdm_plane_params(s->fdm)->surface_deflection_deg;
    fb_fdm_state(s->fdm, &st);
    g_string_append(g, ",\"plane\":{\"airspeed_mps\":");
    js_num(g, round(pe->airspeed * 100) / 100);
    g_string_append(g, ",\"alpha_deg\":");
    js_num(g, round(pe->alpha * 1800 / G_PI) / 10);
    g_string_append(g, ",\"beta_deg\":");
    js_num(g, round(pe->beta * 1800 / G_PI) / 10);
    g_string_append(g, ",\"thrust_n\":");
    js_num(g, round(pe->thrust_n * 100) / 100);
    g_string_append(g, ",\"surfaces_deg\":{");
    for (int k = 0; k < 4; k++) {
        if (k) {
            g_string_append_c(g, ',');
        }
        js_str(g, s->fdm_in_names[k]);
        g_string_append_c(g, ':');
        js_num(g, round(pe->surface[k] * defl * 10) / 10);
    }
    g_string_append_printf(g, "},\"launch\":\"%s\",\"held\":%s,\"posed\":%s,\"scenario\":",
                           launch[pe->launch >= 0 && pe->launch <= 2 ? pe->launch : 0],
                           st.held ? "true" : "false", st.posed ? "true" : "false");
    if (ev->sc.running && !ev->sc.ended) {
        js_str(g, ev->sc.name);
    } else {
        g_string_append(g, "null");
    }
    g_string_append(g, ",\"wind_ned\":");
    js_vec(g, ev->wind, 3);
    g_string_append_c(g, '}');
}

/* ---- commands --------------------------------------------------------------------------- */

/* a `launch` object's fields (control or pilot socket) as overrides; ranges as docs/protocol.md */
static bool env_launch_parse(QDict *d, fb_sc_launch_t *o, Error **errp)
{
    static const struct { const char *k; double lo, hi; bool open_lo; } f[3] = {
        { "speed_kmh", 0, 360, true }, { "duration_s", 0, 10, true }, { "pitch_deg", -60, 60, false },
    };
    double *out[3] = { &o->speed_kmh, &o->duration_s, &o->pitch_deg };
    o->speed_kmh = o->duration_s = o->pitch_deg = o->heading_deg = NAN;
    o->heading = FB_SC_HEADING_UNSET;
    for (int i = 0; i < 3; i++) {
        QObject *x = qdict_get(d, f[i].k);
        if (!x || qobject_type(x) == QTYPE_QNULL) {
            continue;
        }
        QNum *n = qobject_to(QNum, x);
        double v = n ? qnum_get_double(n) : NAN;
        if (!isfinite(v) || v < f[i].lo || v > f[i].hi || (f[i].open_lo && v == f[i].lo)) {
            error_setg(errp, "launch: %s must be a number in %s%g..%g", f[i].k,
                       f[i].open_lo ? "(" : "", f[i].lo, f[i].hi);
            return false;
        }
        *out[i] = v;
    }
    QObject *h = qdict_get(d, "heading_deg");
    if (h && qobject_type(h) == QTYPE_QNULL) {
        o->heading = FB_SC_HEADING_CURRENT;
    } else if (h) {
        QNum *n = qobject_to(QNum, h);
        double v = n ? qnum_get_double(n) : NAN;
        if (!isfinite(v)) {
            error_setg(errp, "launch: heading_deg must be a number or null (the current heading)");
            return false;
        }
        o->heading = FB_SC_HEADING_VALUE;
        o->heading_deg = v;
    }
    return true;
}

/* apply a launch: a scenario step waiting for it takes it (with its own parameters under the
 * command's), else the running scenario's `launch` object, else the defaults, under o */
static void env_launch(NexusXRState *s, const fb_sc_launch_t *o)
{
    FbEnv *ev = s->env;
    fb_scenario_t *sc = &ev->sc;
    fb_sc_launch_t base, l;
    const fb_sc_step_t *w = sc->running && sc->next < sc->n ? &sc->steps[sc->next] : NULL;
    bool step = w && isnan(w->at_s) && w->action == FB_SC_LAUNCH;
    double a, imu_x;

    fb_scenario_launch_merge(sc->running ? &sc->launch : NULL, step ? &w->launch : NULL, &base);
    fb_scenario_launch_merge(&base, o, &l);
    int rc = step ? (fb_scenario_command(sc, s->fdm, FB_SC_LAUNCH, o) ? 0 : -1)
                  : fb_scenario_do_launch(s->fdm, &l);
    if (rc) {
        ctl_log(s, "core", "launch: the model does not take this launch (%g km/h in %g s at %g deg)",
                l.speed_kmh, l.duration_s, l.pitch_deg);
        return;
    }
    bool weak = fb_launch_weak(l.speed_kmh, l.duration_s, l.pitch_deg, &a, &imu_x);
    ctl_log(s, "core", "launch: %g km/h in %g s at %g deg%s (a %.1f m/s^2, IMU X %.1f m/s^2)",
            l.speed_kmh, l.duration_s, l.pitch_deg, step ? ", the scenario's step" : "", a, imu_x);
    if (s->ctl) {
        g_autofree char *d = g_strdup_printf("%g km/h in %g s at %g deg", l.speed_kmh, l.duration_s,
                                             l.pitch_deg);
        fn_msg(s, "launch", "start", 0, 1, d, NULL);
    }
    if (weak) {
        ctl_log(s, "core", "launch: a %.1f m/s^2 < %.1f: iNav's launch detector may not fire",
                a, FB_LAUNCH_ACCEL_MARGIN * FB_INAV_LAUNCH_ACCEL_MSS);
    }
}

/* `constraint`: {"mode":"hold","euler_deg":[r,p,y|null]|"quat":[w,x,y,z],"rotate_s":1} | "free" */
static bool env_constraint(NexusXRState *s, QDict *d, Error **errp)
{
    FbEnv *ev = s->env;
    const char *mode = qdict_get_try_str(d, "mode");
    if (mode && !strcmp(mode, "free")) {
        fb_fdm_release(s->fdm);
        ev->cmd_hold = false;
        ctl_log(s, "core", "constraint: free");
        return true;
    }
    if (!mode || strcmp(mode, "hold")) {
        error_setg(errp, "constraint: mode must be hold|free");
        return false;
    }
    double rot = 1, x[4];
    QObject *ro = qdict_get(d, "rotate_s");
    if (ro) {
        QNum *n = qobject_to(QNum, ro);
        rot = n ? qnum_get_double(n) : NAN;
        if (!(rot >= 0 && rot <= 60)) {
            error_setg(errp, "constraint: rotate_s must be a number 0..60");
            return false;
        }
    }
    QList *eu = qdict_haskey(d, "euler_deg") ? qobject_to(QList, qdict_get(d, "euler_deg")) : NULL;
    QList *qu = qdict_haskey(d, "quat") ? qobject_to(QList, qdict_get(d, "quat")) : NULL;
    if (qdict_haskey(d, "euler_deg") == qdict_haskey(d, "quat")) {
        error_setg(errp, "constraint: hold needs exactly one of euler_deg or quat");
        return false;
    }
    QList *l = eu ? eu : qu;
    int want = eu ? 3 : 4, i = 0;
    if (!l || (int)qlist_size(l) != want) {
        error_setg(errp, eu || qdict_haskey(d, "euler_deg")
                   ? "constraint: euler_deg must be [roll, pitch, yaw|null] in degrees"
                   : "constraint: quat must be [w, x, y, z]");
        return false;
    }
    for (const QListEntry *e = qlist_first(l); e; e = qlist_next(e), i++) {
        QObject *xo = qlist_entry_obj(e);
        QNum *n = qobject_to(QNum, xo);
        if (eu && i == 2 && qobject_type(xo) == QTYPE_QNULL) {
            x[i] = NAN;
        } else if (!n || !isfinite(qnum_get_double(n))) {
            error_setg(errp, eu ? "constraint: euler_deg must be [roll, pitch, yaw|null] in degrees"
                                : "constraint: quat must be [w, x, y, z]");
            return false;
        } else {
            x[i] = qnum_get_double(n);
        }
    }
    if (eu) {
        fb_fdm_hold(s->fdm, x[0] * G_PI / 180, x[1] * G_PI / 180,
                    isnan(x[2]) ? NAN : x[2] * G_PI / 180, rot);
        char yaw[32] = "kept";
        if (!isnan(x[2])) {
            snprintf(yaw, sizeof(yaw), "%g", x[2]);
        }
        ctl_log(s, "core", "constraint: hold roll %g pitch %g yaw %s in %g s", x[0], x[1], yaw, rot);
    } else {
        double nq = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2] + x[3] * x[3]);
        if (nq < 1e-9) {
            error_setg(errp, "constraint: quat must not be zero");
            return false;
        }
        for (i = 0; i < 4; i++) {
            x[i] /= nq;
        }
        fb_fdm_hold_quat(s->fdm, x, rot);
        ctl_log(s, "core", "constraint: hold quat [%.4f, %.4f, %.4f, %.4f] in %g s", x[0], x[1],
                x[2], x[3], rot);
    }
    ev->cmd_hold = true;
    return true;
}

/* `env`: wind_ned, pressure_pa + temp_c (air density), home (the GPS's origin), scenario (name,
 * object or null); every field is checked before any is applied */
static bool env_command(NexusXRState *s, QDict *d, Error **errp)
{
    FbEnv *ev = s->env;
    double w[3] = { ev->wind[0], ev->wind[1], ev->wind[2] }, pa = NAN, tc = 15;
    bool have_w = false, have_air = false, have_sc = false, have_home = false;
    double home[3] = { s->home[0], s->home[1], s->home[2] };
    fb_scenario_t sc = { 0 };
    QObject *o = qdict_get(d, "wind_ned");
    if (o) {
        QList *l = qobject_to(QList, o);
        int i = 0;
        if (!l || qlist_size(l) != 3) {
            error_setg(errp, "env: wind_ned must be [n, e, d] in m/s");
            return false;
        }
        for (const QListEntry *e = qlist_first(l); e; e = qlist_next(e), i++) {
            QNum *n = qobject_to(QNum, qlist_entry_obj(e));
            if (!n || !(fabs(qnum_get_double(n)) <= 100)) {
                error_setg(errp, "env: wind_ned must be [n, e, d] in m/s, each within +-100");
                return false;
            }
            w[i] = qnum_get_double(n);
        }
        have_w = true;
    }
    if (qdict_haskey(d, "pressure_pa") || qdict_haskey(d, "temp_c")) {
        QNum *p = qobject_to(QNum, qdict_get(d, "pressure_pa"));
        QNum *t = qobject_to(QNum, qdict_get(d, "temp_c"));
        pa = p ? qnum_get_double(p) : 101325;
        tc = t ? qnum_get_double(t) : 15;
        if ((qdict_haskey(d, "pressure_pa") && !p) || (qdict_haskey(d, "temp_c") && !t) ||
            !(pa >= 30000 && pa <= 110000) || !(tc >= -60 && tc <= 60)) {
            error_setg(errp, "env: pressure_pa must be 30000..110000, temp_c -60..60");
            return false;
        }
        have_air = true;
    }
    o = qdict_get(d, "home");
    if (o) {
        QDict *h = qobject_to(QDict, o);
        static const char *const k[3] = { "lat", "lon", "alt_m" };
        static const double lim[3] = { 90, 180, 9000 };
        for (int i = 0; h && i < 3; i++) {
            QObject *x = qdict_get(h, k[i]);
            QNum *n = x ? qobject_to(QNum, x) : NULL;
            if (x && (!n || !(fabs(qnum_get_double(n)) <= lim[i]))) {
                h = NULL;
            } else if (n) {
                home[i] = qnum_get_double(n);
            }
        }
        if (!h) {
            error_setg(errp, "env: home must be {lat -90..90, lon -180..180, alt_m -9000..9000} "
                       "(each optional)");
            return false;
        }
        have_home = true;
    }
    o = qdict_get(d, "scenario");
    if (o) {
        have_sc = true;
        if (qobject_type(o) == QTYPE_QSTRING) {
            if (!env_scenario_file(s, qstring_get_str(qobject_to(QString, o)), &sc, NULL, errp)) {
                return false;
            }
        } else if (qobject_type(o) == QTYPE_QDICT) {
            g_autoptr(GString) j = qobject_to_json(o);
            char e[160];
            if (fb_scenario_parse(&sc, j->str, j->len, e, sizeof e)) {
                error_setg(errp, "env.scenario: %s", e);
                return false;
            }
        } else if (qobject_type(o) != QTYPE_QNULL) {
            error_setg(errp, "env: scenario must be a name, an object or null");
            return false;
        }
    }
    if (have_w) {
        memcpy(ev->wind, w, sizeof(w));
        fb_fdm_set_wind(s->fdm, w);
        ctl_log(s, "core", "env: wind %.1f %.1f %.1f m/s (NED, towards)", w[0], w[1], w[2]);
    }
    if (have_air) {
        double rho = fb_fdm_air_density(pa, tc);
        fb_fdm_set_air(s->fdm, rho);
        ctl_log(s, "core", "env: air %.0f Pa %.1f C: density %.4f kg/m^3", pa, tc, rho);
    }
    if (have_home) {
        memcpy(s->home, home, sizeof(home));
        if (s->gps) {
            fb_ublox_set_home(s->gps, home[0], home[1], home[2]);
        }
        ctl_log(s, "core", "env: home %.7f %.7f, %.1f m MSL%s", home[0], home[1], home[2],
                s->gps ? " (the GPS's origin: the solution jumps there)" : " (no GPS wired)");
    }
    if (have_sc) {
        if (qobject_type(o) == QTYPE_QNULL) {
            fb_scenario_stop(&ev->sc);
            ctl_log(s, "core", "env: scenario stopped (the model stays where it is)");
        } else {
            env_scenario_run(s, &sc);
        }
    }
    for (const QDictEntry *e = qdict_first(d); e; e = qdict_next(d, e)) {
        const char *k = qdict_entry_key(e);
        if (strcmp(k, "type") && strcmp(k, "wind_ned") && strcmp(k, "pressure_pa") &&
            strcmp(k, "temp_c") && strcmp(k, "scenario") && strcmp(k, "home")) {
            ctl_log(s, "core", "env: %s not modelled, ignored", k);
        }
    }
    return true;
}
