/* Host unit tests of the bench resolver (devices/src/session.c, what -M nexus-xr,session= runs):
 * the cases of core/qemu/scripts/test_wiring.py (every WiringError and note, both wiring forms,
 * the old form's migration rules, aircraft checks, OSD, the speedybee-f405-wing: output pads, its
 * machine's options, the CLI script, board.json checks), plus what only the C side has: "~"
 * expansion, a relative data root, the default root, the blob limit, the manifest.
 *   make -C core/qemu/tests test      (runs from core/qemu/tests; REPO_ROOT = the repo) */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* setenv */
#endif
#include "fb/session.h"
#include "fb/json.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef REPO_ROOT
#error "REPO_ROOT (the repo's absolute path) is set by the Makefile"
#endif
#define TMP "build/session-tmp"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("FAIL cannot write %s\n", path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    b[fread(b, 1, (size_t)n, f)] = 0;
    fclose(f);
    return b;
}

/* a string with one occurrence of `from` replaced (the caller frees) */
static char *replace(const char *s, const char *from, const char *to)
{
    const char *p = strstr(s, from);
    if (!p) {
        printf("FAIL replace: '%s' not found\n", from);
        exit(1);
    }
    size_t n = strlen(s) - strlen(from) + strlen(to) + 1;
    char *r = malloc(n);
    snprintf(r, n, "%.*s%s%s", (int)(p - s), s, to, p + strlen(from));
    return r;
}

/* ---- benches ---------------------------------------------------------------------------- */

#define NEW_WIRING \
    "{\"device\":\"esc\",\"port\":\"signal\",\"pad\":\"motor1\",\"telemetry\":\"bidir\"}," \
    "{\"device\":\"esc\",\"port\":\"tlm\",\"pad\":\"TELEM\"}," \
    "{\"device\":\"esc\",\"port\":\"rpm\",\"pad\":\"RPM\"}," \
    "{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"servo1\",\"actuator\":\"swash_a\"}," \
    "{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"servo2\",\"actuator\":\"swash_b\"}," \
    "{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"servo3\",\"actuator\":\"swash_c\"}," \
    "{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"TAIL\",\"actuator\":\"tail\",\"reversed\":false}"
#define OLD_WIRING \
    "{\"device\":\"icm42688p\",\"slot\":\"gyro1\"},{\"device\":\"dps310\",\"slot\":\"i2c3\"}," \
    "{\"device\":\"crsf\",\"slot\":\"uart5\"},{\"device\":\"esc\",\"slot\":\"motor1\",\"telemetry\":\"bidir\"}," \
    "{\"device\":\"servo\",\"slot\":\"servo1\",\"actuator\":\"swash_a\"}," \
    "{\"device\":\"servo\",\"slot\":\"servo2\",\"actuator\":\"swash_b\"}," \
    "{\"device\":\"servo\",\"slot\":\"servo3\",\"actuator\":\"swash_c\"}," \
    "{\"device\":\"servo\",\"slot\":\"servo4\",\"actuator\":\"tail\"}"

static const char *board_json;      /* NULL: the repo's */

static fb_session_t *resolve_text(const char *text)
{
    fb_session_opts_t o = { .root = REPO_ROOT, .board_json = board_json };
    return fb_session_resolve(text, strlen(text), &o);
}

/* a bench with this wiring (JSON list body) and extra top-level members (",\"k\":v" or "") */
static fb_session_t *bench(const char *wiring, const char *extra)
{
    size_t n = strlen(wiring) + strlen(extra) + 512;
    char *t = malloc(n);
    snprintf(t, n, "{\"board\":\"nexus-xr\",\"firmware\":\"fw/rf.elf\",\"profile\":\"models/rs7/qemu-profile.txt\","
             "\"aircraft\":\"models/rs7/aircraft.json\",\"wiring\":[%s]%s,"
             "\"time\":{\"icount_shift\":2,\"realtime\":false,\"idle_skip\":true}}", wiring, extra);
    fb_session_t *s = resolve_text(t);
    free(t);
    return s;
}

static fb_session_t *new_plus(const char *more) /* the rs7-pads wiring + entries */
{
    char w[4096];
    snprintf(w, sizeof w, "%s%s%s", NEW_WIRING, *more ? "," : "", more);
    return bench(w, "");
}

static fb_session_t *old_plus(const char *more)
{
    char w[4096];
    snprintf(w, sizeof w, "%s%s%s", OLD_WIRING, *more ? "," : "", more);
    return bench(w, "");
}

static const char *opt(fb_session_t *s, const char *k)
{
    for (int i = 0; i < fb_session_noptions(s); i++) {
        if (!strcmp(fb_session_option_key(s, i), k)) return fb_session_option_value(s, i);
    }
    return NULL;
}

static int has_error(fb_session_t *s, const char *sub)
{
    for (int i = 0; i < fb_session_nerrors(s); i++) {
        if (strstr(fb_session_error(s, i), sub)) return 1;
    }
    return 0;
}

static int count_notes(fb_session_t *s, const char *sub)
{
    int n = 0;
    for (int i = 0; i < fb_session_nwarnings(s); i++) n += strstr(fb_session_warning(s, i), sub) != NULL;
    return n;
}

static void dump(fb_session_t *s)
{
    for (int i = 0; i < fb_session_nerrors(s); i++) printf("    error: %s\n", fb_session_error(s, i));
    for (int i = 0; i < fb_session_nwarnings(s); i++) printf("    note: %s\n", fb_session_warning(s, i));
}

#define REJECT(s_, sub) do { fb_session_t *r_ = (s_); \
    CHECK(!fb_session_ok(r_) && has_error(r_, sub), "want error '%s'", sub); \
    if (!has_error(r_, sub)) { dump(r_); } fb_session_free(r_); } while (0)

static const char *const LEGACY[][2] = {
    { "imu", "on" }, { "crsf", "uart5" }, { "esc", "on" }, { "baro", "on" }, { "servos", "15" },
    { "actuators", "swash_a:swash_b:swash_c:tail:motor" }, { NULL, NULL },
};

/* the device options are LEGACY with these changes ({k, v}; v NULL = absent) */
static int legacy_but(fb_session_t *s, const char *const ch[][2])
{
    if (!fb_session_ok(s)) {
        dump(s);
        return 0;
    }
    int ok = 1;
    static const char *const keys[] = { "imu", "imu-noise", "crsf", "esc", "esc-timeout-ms", "baro", "servos",
                                        "osd", "actuators", "servo-rev", NULL };
    for (int k = 0; keys[k]; k++) {
        const char *want = NULL;
        for (int i = 0; LEGACY[i][0]; i++) {
            if (!strcmp(LEGACY[i][0], keys[k])) want = LEGACY[i][1];
        }
        for (int i = 0; ch && ch[i][0]; i++) {
            if (!strcmp(ch[i][0], keys[k])) want = ch[i][1];
        }
        const char *got = opt(s, keys[k]);
        if ((want == NULL) != (got == NULL) || (want && strcmp(want, got))) {
            printf("    %s: got %s, want %s\n", keys[k], got ? got : "(none)", want ? want : "(none)");
            ok = 0;
        }
    }
    return ok;
}

#define LEGACY_BUT(s_, ...) do { fb_session_t *l_ = (s_); static const char *const ch_[][2] = { __VA_ARGS__ }; \
    CHECK(legacy_but(l_, ch_), "machine options"); fb_session_free(l_); } while (0)
#define END { NULL, NULL }

/* a copy of the repo's board.json with one text replaced, as the board_json override */
static const char *board_with(const char *name, const char *from, const char *to)
{
    static char path[256];
    char *bj = read_all(REPO_ROOT "/boards/nexus-xr/board.json");
    char *mod = replace(bj, from, to);
    snprintf(path, sizeof path, TMP "/%s.json", name);
    write_file(path, mod);
    free(bj);
    free(mod);
    return path;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void forms_and_migration(void)
{
    /* the old form (what the app writes) and the new form give the same machine */
    LEGACY_BUT(bench(OLD_WIRING, ""), END);
    fb_session_t *s = bench(NEW_WIRING, "");
    CHECK(count_notes(s, "esc.tlm on TELEM (uart2)") == 1, "tlm note");
    LEGACY_BUT(s, END);
    /* old form: the list only adds; receiver and ESC stay (the machine before the wiring model) */
    s = bench("{\"device\":\"icm42688p\",\"slot\":\"gyro1\"},{\"device\":\"dps310\",\"slot\":\"i2c3\"},"
              "{\"device\":\"servo\",\"slot\":\"servo1\",\"actuator\":\"swash_a\"},"
              "{\"device\":\"servo\",\"slot\":\"servo2\",\"actuator\":\"swash_b\"},"
              "{\"device\":\"servo\",\"slot\":\"servo3\",\"actuator\":\"swash_c\"},"
              "{\"device\":\"servo\",\"slot\":\"servo4\",\"actuator\":\"tail\"}", "");
    CHECK(count_notes(s, "old-form session: receiver/ESC/servos assumed present") == 1, "assumed note");
    LEGACY_BUT(s, END);
    /* new form: complete, what is not wired is not there */
    s = bench("{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"servo1\",\"actuator\":\"swash_a\"},"
              "{\"device\":\"crsf\",\"detached\":true}", "");
    CHECK(fb_session_ok(s) && !strcmp(opt(s, "crsf"), "off") && !strcmp(opt(s, "esc"), "off"), "new form: no rx, no esc");
    fb_session_free(s);
    /* old form: crsf on uart3, the (detachable) baro omitted -> detached */
    s = bench("{\"device\":\"icm42688p\",\"slot\":\"gyro1\"},{\"device\":\"crsf\",\"slot\":\"uart3\"}", "");
    CHECK(count_notes(s, "assumed present (not in the list: ESC on motor1; servos on servo1, servo2, servo3, servo4)") == 1,
          "old uart3 assumed");
    LEGACY_BUT(s, { "crsf", "uart3" }, { "baro", "off" }, END);
    /* the app's smoke bench: gyro only, old form, the app's empty props */
    s = bench("{\"device\":\"icm42688p\",\"slot\":\"gyro1\",\"actuator\":\"\",\"reversed\":false,\"option\":\"\"}", "");
    CHECK(count_notes(s, "assumed present (not in the list: receiver on Int.Rx (uart5); ESC on motor1; "
                         "servos on servo1, servo2, servo3, servo4)") == 1, "smoke assumed");
    LEGACY_BUT(s, { "baro", "off" }, END);
    /* old form, unplaceable entries: warnings, left out */
    s = old_plus("{\"device\":\"nope\",\"slot\":\"uart3\"},{\"device\":\"servo\",\"slot\":\"servo9\"},"
                 "{\"device\":\"servo\",\"slot\":\"PORT-A\"},{\"device\":\"servo\",\"slot\":\"servo1\",\"actuator\":\"swash_b\"}");
    CHECK(count_notes(s, "left out (old-form session)") == 4, "4 left out");
    LEGACY_BUT(s, END);
    /* old form omitting a soldered baro: kept; the real (detachable) one: detached */
    char w[4096];
    snprintf(w, sizeof w, "%s", OLD_WIRING);
    char *no_baro = replace(w, "{\"device\":\"dps310\",\"slot\":\"i2c3\"},", "");
    board_json = board_with("soldered-baro", "\"device\": \"dps310\", \"detachable\": true",
                            "\"device\": \"dps310\", \"detachable\": false");
    s = bench(no_baro, "");
    CHECK(count_notes(s, "onboard dps310 on i2c3 not in the (old-form) wiring: it is soldered, kept") == 1, "soldered kept");
    LEGACY_BUT(s, END);
    board_json = NULL;
    s = bench(no_baro, "");
    CHECK(count_notes(s, "onboard dps310 on i2c3 not in the (old-form) wiring: detached") == 1, "detached note");
    LEGACY_BUT(s, { "baro", "off" }, END);
    free(no_baro);
    /* mixing the forms is rejected */
    REJECT(bench("{\"device\":\"servo\",\"slot\":\"servo1\"},{\"device\":\"servo\",\"pad\":\"servo2\"}", ""), "mixes the old form");
    REJECT(bench("{\"device\":\"servo\",\"slot\":\"servo1\",\"pad\":\"servo1\"}", ""), "mixes the old form");
    REJECT(bench("{\"device\":\"icm42688p\",\"slot\":\"gyro1\"},{\"device\":\"crsf\",\"detached\":true}", ""), "mixes the old form");
    /* the app's bench: every entry with actuator/reversed/option, the aircraft as its own path */
    char *app = replace(OLD_WIRING "", "\"telemetry\":\"bidir\"", "\"option\":\"bidir\",\"actuator\":\"\",\"reversed\":false");
    s = bench(app, ",\"aircraft\":\"C:/Users/you/flightbench/models/rs7/aircraft.json\"");
    LEGACY_BUT(s, END);
    free(app);
}

static void wiring_errors(void)
{
    REJECT(bench("{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"S9\",\"actuator\":\"tail\"}", ""), "unknown pad 'S9'");
    REJECT(bench("{\"device\":\"servo\",\"port\":\"pwm\",\"pad\":\"PORT-A\"}", ""),
           "servo.pwm is a servo port, pad PORT-A (uart4) is uart");
    board_json = board_with("soldered-baro", "\"device\": \"dps310\", \"detachable\": true",
                            "\"device\": \"dps310\", \"detachable\": false");
    REJECT(bench("{\"device\":\"dps310\",\"detached\":true}", ""), "is soldered (detachable: false)");
    board_json = board_with("gyro-no-detachable", "\"device\": \"icm42688p\", \"detachable\": true,",
                            "\"device\": \"icm42688p\",");
    REJECT(bench("{\"device\":\"icm42688p\",\"detached\":true}", ""),
           "onboard icm42688p on gyro1 is soldered (detachable: false)");
    board_json = NULL;
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash_a\"},"
                 "{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash_b\"}", ""), "pad S1 (servo1) used by");
    REJECT(bench("{\"device\":\"gps\",\"pad\":\"uart5\"}", ""), "pad Int.Rx (uart5) used by gps.uart");
    REJECT(bench("{\"device\":\"nope\",\"pad\":\"uart3\"}", ""), "unknown device 'nope'");
    REJECT(bench("{\"device\":\"crsf\",\"pad\":\"TELEM\"}", ""), "pad TELEM (uart2) has no TX");
    REJECT(bench("{\"device\":\"servo\",\"detached\":true}", ""), "servo is not an onboard device of nexus-xr, nothing to detach");
    REJECT(bench("{\"device\":\"crsf\",\"pad\":\"PORT-A\",\"detached\":true}", ""), "nothing to detach");
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"wiring\":{\"device\":\"esc\"}}"), "wiring: need a list");
    REJECT(bench("{\"device\":\"icm42688p\",\"port\":\"int\",\"pad\":\"gyro1\"}", ""), "comes with the spi pad");
    REJECT(bench("{\"device\":\"esc\",\"port\":\"nope\",\"pad\":\"motor1\"}", ""), "has no port 'nope'");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"flap\"}", ""), "not one of the aircraft's");
    REJECT(bench("{\"device\":\"icm42688p\",\"detached\":true},{\"device\":\"icm42688p\",\"pad\":\"gyro1\"}", ""),
           "is both wired and detached");
    REJECT(bench("{\"device\":\"icm42688p\",\"detached\":\"yes\"}", ""), "detached must be true|false");
    REJECT(bench("{\"device\":\"icm42688p\",\"detached\":1}", ""), "detached must be true|false");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash_a\",\"reversed\":\"true\"}", ""),
           "servo reversed 'true': need true|false");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash_a\",\"reversed\":1}", ""), "need true|false");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"tail\"},"
                 "{\"device\":\"servo\",\"pad\":\"servo4\",\"actuator\":\"tail\"}", ""), "actuator tail is already driven by");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash a\"}", ""), "letters, digits and _ only");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":3}", ""), "letters, digits and _ only");
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"motor\"}", ""),
           "servo actuator 'motor' is not one of the aircraft's FDM inputs");
    REJECT(bench("{\"device\":\"esc\",\"pad\":\"motor1\",\"actuator\":\"tail\"}", ""),
           "esc actuator 'tail': the ESC's signal drives the aircraft's throttle 'motor'");
    REJECT(bench("{\"device\":\"gps\",\"pad\":\"PORT-A\",\"actuator\":\"tail\"}", ""), "gps has no prop 'actuator'");
    REJECT(bench("{\"device\":\"servo\",\"slot\":\"servo2\",\"actuator\":\"motor\"}", ""), "not one of the aircraft's FDM inputs");
    char *esc_motor = replace(NEW_WIRING, "\"telemetry\":\"bidir\"", "\"telemetry\":\"bidir\",\"actuator\":\"motor\"");
    LEGACY_BUT(bench(esc_motor, ""), END);          /* the ESC keeps the throttle's name */
    free(esc_motor);
    REJECT(bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"swash_a\",\"trim\":5}", ""),
           "wiring[0]: servo has no prop 'trim'");
    REJECT(bench("{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"gyro_noise\":0.1}", ""), "icm42688p has no prop 'gyro_noise'");
    REJECT(bench("{\"device\":\"esc\",\"slot\":\"motor1\",\"timeout\":5}", ""), "esc has no prop 'timeout'");
    REJECT(bench("{\"device\":\"esc\",\"pad\":\"motor1\",\"telemetry\":\"pwm\"}", ""), "need one of bidir, serial");
    REJECT(bench("{\"device\":\"esc\",\"pad\":\"motor1\",\"signal_timeout_ms\":2001}", ""),
           "esc signal_timeout_ms 2001: need an integer 0..2000");
    REJECT(bench("{\"device\":\"esc\",\"pad\":\"motor1\",\"signal_timeout_ms\":50.5}", ""), "need an integer 0..2000");
    REJECT(bench("{\"device\":\"esc\",\"slot\":\"motor1\",\"signal_timeout_ms\":-1}", ""), "need an integer 0..2000");
    REJECT(bench("{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"gyro_noise_dps\":51}", ""),
           "icm42688p gyro_noise_dps 51: need a number 0..50");
    REJECT(bench("{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"accel_noise_g\":\"0.1\"}", ""), "need a number 0..2");
    REJECT(bench("{\"device\":\"compass\",\"pad\":\"i2c3\"},{\"device\":\"compass\",\"pad\":\"i2c3\"}", ""),
           "bus i2c3: two compass (same I2C address)");
    REJECT(bench("{\"device\":\"dps310\",\"pad\":\"i2c3\"},{\"device\":\"dps310\",\"pad\":\"i2c3\"}", ""), "two dps310");
    REJECT(bench("{\"device\":\"esc\",\"pad\":\"motor1\",\"telemetry\":\"bidir\",\"option\":\"serial\"}", ""),
           "option 'serial' and telemetry 'bidir' disagree");
    /* labels must be unique, case-insensitively and against other pads' ids */
    board_json = board_with("label-clash", "{\"id\": \"uart6\", \"label\": \"PORT-B\"",
                            "{\"id\": \"uart6\", \"label\": \"port-a\"");
    REJECT(bench(NEW_WIRING, ""), "label 'port-a' is not unique");
    board_json = board_with("label-id", "{\"id\": \"uart6\", \"label\": \"PORT-B\"",
                            "{\"id\": \"uart6\", \"label\": \"uart3\"");
    REJECT(bench(NEW_WIRING, ""), "is not unique");
    board_json = NULL;
}

static void wiring_accepted(void)
{
    fb_session_t *s;
    LEGACY_BUT(new_plus("{\"device\":\"icm42688p\",\"detached\":true}"), { "imu", "off" }, END);
    LEGACY_BUT(new_plus("{\"device\":\"dps310\",\"detached\":true}"), { "baro", "off" }, END);
    s = new_plus("{\"device\":\"compass\",\"pad\":\"i2c3\"}");
    CHECK(count_notes(s, "compass.i2c on i2c3 (wired): not modelled on this machine, left out") == 1, "compass note");
    CHECK(count_notes(s, "dps310") == 0, "no baro note");
    LEGACY_BUT(s, END);
    /* the app writes actuator "", reversed false, option "" on every entry */
    char *app = replace(NEW_WIRING, "\"pad\":\"RPM\"", "\"pad\":\"RPM\",\"actuator\":\"\",\"reversed\":false,\"option\":\"\"");
    LEGACY_BUT(bench(app, ""), END);
    free(app);
    /* noise and timeout props -> options */
    char *to0 = replace(NEW_WIRING, "\"telemetry\":\"bidir\"", "\"telemetry\":\"bidir\",\"signal_timeout_ms\":0");
    char w[4096];
    snprintf(w, sizeof w, "%s,{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"gyro_noise_dps\":0.5}", to0);
    LEGACY_BUT(bench(w, ""), { "imu-noise", "0.5:0.002" }, { "esc-timeout-ms", "0" }, END);
    free(to0);
    /* an external receiver, a reversed servo */
    char *rev = replace(NEW_WIRING, "\"pad\":\"servo2\",\"actuator\":\"swash_b\"",
                        "\"pad\":\"servo2\",\"actuator\":\"swash_b\",\"reversed\":true");
    snprintf(w, sizeof w, "%s,{\"device\":\"crsf\",\"detached\":true},{\"device\":\"crsf\",\"pad\":\"PORT-C\"}", rev);
    LEGACY_BUT(bench(w, ""), { "crsf", "uart3" }, { "servo-rev", "2" }, END);
    free(rev);
    /* no wiring: no device options at all -> the machine's defaults (the baro stays on) */
    s = bench("", "");
    CHECK(fb_session_ok(s) && !opt(s, "imu") && !opt(s, "baro") && opt(s, "fdm"), "no wiring");
    CHECK(count_notes(s, "no wiring: the machine's defaults") == 1, "no wiring note");
    fb_session_free(s);
}

static void osd(void)
{
    fb_session_t *s = new_plus("{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"PORT-A\"}");
    const char *r = fb_session_report(s, NULL);
    CHECK(opt(s, "osd") && !strcmp(opt(s, "osd"), "uart4"), "osd=uart4");
    CHECK(r && strstr(r, "config: serial 3 1 115200 57600 0 115200\n") && strstr(r, "config: set displayport_msp_serial = 3\n")
          && strstr(r, "config: set osd_crosshairs_pos") && strstr(r, "config: set osd_displayport_device = AUTO\n"),
          "osd config lines");
    size_t n;
    const char *blob = fb_session_defaults(s, &n);
    CHECK(blob && strstr(blob, "\nset displayport_msp_serial = 3\n") && !strstr(blob, "(device config)"),
          "device config in the blob, its comment dropped");
    fb_session_free(s);
    s = old_plus("{\"device\":\"msp_osd\",\"slot\":\"uart6\"}");
    r = fb_session_report(s, NULL);
    CHECK(opt(s, "osd") && !strcmp(opt(s, "osd"), "uart6") && strstr(r, "config: set displayport_msp_serial = 5\n"), "osd uart6");
    fb_session_free(s);
    s = bench(OLD_WIRING, "");
    CHECK(fb_session_ok(s) && !opt(s, "osd") && !strstr(fb_session_report(s, NULL), "\nconfig: "), "no osd");
    fb_session_free(s);
    REJECT(new_plus("{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"TELEM\"}"), "has no TX");
    REJECT(new_plus("{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"Int.Rx\"}"), "pad Int.Rx (uart5) used by");
    REJECT(new_plus("{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"S1\"}"), "is a uart port, pad S1 (servo1) is servo");
    REJECT(new_plus("{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"PORT-A\"},{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"PORT-B\"}"),
           "one OSD per session: msp_osd on PORT-A (uart4) (wiring[7]), PORT-B (uart6) (wiring[8])");
    REJECT(old_plus("{\"device\":\"msp_osd\",\"slot\":\"uart4\"},{\"device\":\"msp_osd\",\"slot\":\"uart6\"}"), "one OSD per session");
    s = new_plus("{\"device\":\"crsf\",\"detached\":true},{\"device\":\"crsf\",\"port\":\"uart\",\"pad\":\"PORT-C\"},"
                 "{\"device\":\"msp_osd\",\"port\":\"uart\",\"pad\":\"uart5\"}");
    r = fb_session_report(s, NULL);
    CHECK(fb_session_ok(s) && !strcmp(opt(s, "crsf"), "uart3") && !strcmp(opt(s, "osd"), "uart5") &&
          strstr(r, "config: set displayport_msp_serial = 4\n") && strstr(r, "config: serial 4 1 115200 57600 0 115200\n"),
          "osd on uart5, receiver elsewhere");
    fb_session_free(s);
}

static void aircraft(void)
{
    static const struct { const char *from, *to, *want; } cases[] = {
        { "\"transmission\": {\"main_rotor_gear_ratio\": [13, 116]},", "", "transmission.main_rotor_gear_ratio: missing" },
        { "[13, 116]", "[0, 116]", "need [pinion, main], 2 numbers > 0" },
        { "[13, 116]", "[13]", "need [pinion, main]" },
        { "\"kv\": 520", "\"kv\": 0", "esc.kv 0: need a number > 0" },
        { "\"kv\": 520", "\"kv\": \"520\"", "esc.kv '520'" },
        { "\"poles\": 10", "\"poles\": 7", "esc.poles 7: need an even integer 2..64" },
        { "\"poles\": 10", "\"poles\": 66", "esc.poles 66: need a number 2..64" },
        { "\"kv\": 520", "\"kvv\": 1", "esc.kvv: unknown key" },
        { "\"fdm_params\": {", "\"fdm_params\": 5, \"x\": {", "fdm_params: need an object" },
        { "\"tail\": 1500}", "\"tail\": 2500}", "centre_us.tail 2500: need an integer 1000..2000" },
        { "\"centre_us\": {\"swash_a\": 1500, \"swash_b\": 1500, \"swash_c\": 1500, \"tail\": 1500}",
          "\"centre_us\": {\"motor\": 1500}", "centre_us.motor: not one of the fdm_inputs" },
        { "\"throttle\": \"motor\"", "\"throttle\": \"tail\"", "throttle 'tail' is also an fdm_input" },
        { "\"kv\": 520", "\"kv\": 520,", "aircraft core/qemu/tests/" TMP "/ac/aircraft.json: line 7 column 20: expecting a property name" },
    };
    char *base = read_all(REPO_ROOT "/models/rs7/aircraft.json");
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *a = replace(base, cases[i].from, cases[i].to);
        write_file(TMP "/ac/aircraft.json", a);
        free(a);
        REJECT(bench(NEW_WIRING, ",\"aircraft\":\"" REPO_ROOT "/core/qemu/tests/" TMP "/ac/aircraft.json\""), cases[i].want);
    }
    free(base);
    REJECT(bench(NEW_WIRING, ",\"aircraft\":\"models/nope/aircraft.json\""), "aircraft 'models/nope/aircraft.json': no ");
    fb_session_t *s = bench(NEW_WIRING, ",\"aircraft\":\"rs7\"");      /* a model id */
    CHECK(fb_session_ok(s) && !strcmp(opt(s, "aircraft-file"), REPO_ROOT "/models/rs7/aircraft.json"), "aircraft by id");
    fb_session_free(s);
}

static void bench_blocks(void)
{
    fb_session_t *s = bench(NEW_WIRING, ",\"environment\":{\"sea_level_pa\":100800,\"field_elevation_m\":990}");
    CHECK(opt(s, "baro-env") && !strcmp(opt(s, "baro-env"), "100800.0:990.0:22.0"), "baro-env");
    fb_session_free(s);
    s = bench(NEW_WIRING, ",\"environment\":{\"sea_level_pa\":101325.123456789,\"field_elevation_m\":989.987654321,"
              "\"temp_c\":-12.3456789}");
    CHECK(opt(s, "baro-env") && !strcmp(opt(s, "baro-env"), "101325.123456789:989.987654321:-12.3456789"), "baro-env precision");
    fb_session_free(s);
    s = bench(NEW_WIRING, ",\"environment\":{}");
    CHECK(opt(s, "baro-env") && !strcmp(opt(s, "baro-env"), "101325.0:0.0:22.0"), "baro-env defaults");
    fb_session_free(s);
    REJECT(bench(NEW_WIRING, ",\"environment\":{\"sea_level_pa\":5}"), "environment.sea_level_pa 5: need a number 30000..120000");
    REJECT(bench(NEW_WIRING, ",\"environment\":{\"qnh\":1}"), "environment.qnh: unknown key");
    REJECT(bench(NEW_WIRING, ",\"environment\":{\"temp_c\":\"20\"}"), "environment.temp_c '20'");
    REJECT(bench(NEW_WIRING, ",\"environment\":[]"), "environment: need an object");
    /* time (the bench's own "time" comes last in bench(): it wins over these) */
    s = resolve_text("{\"firmware\":\"fw.elf\",\"wiring\":[" NEW_WIRING "],\"time\":{\"icount_shift\":3,\"realtime\":true},"
                     "\"ports\":{\"control\":31280,\"msp\":31281}}");
    CHECK(fb_session_ok(s) && fb_session_icount_shift(s) == 3 && fb_session_realtime(s) && opt(s, "realtime") &&
          !opt(s, "idle-skip") && fb_session_port_control(s) == 31280 && fb_session_port_msp(s) == 31281, "time, ports");
    fb_session_free(s);
    s = resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"icount_shift\":null}}");
    CHECK(fb_session_ok(s) && fb_session_icount_shift(s) == -1 && strstr(fb_session_report(s, NULL), "\nicount: none\n"),
          "no icount");
    fb_session_free(s);
    s = resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"idle_skip\":true,\"idle_skip_n\":32,\"idle_skip_gran_us\":200}}");
    const char *os = fb_session_options_str(s), *tail = ",idle-skip=on,idle-skip-n=32,idle-skip-gran-us=200";
    CHECK(fb_session_ok(s) && strlen(os) > strlen(tail) && !strcmp(os + strlen(os) - strlen(tail), tail),
          "idle skip options: %s", os);
    fb_session_free(s);
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"icount_shift\":11}}"), "time.icount_shift 11: need an integer 0..10 or null");
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"icount_shift\":null,\"idle_skip\":true}}"),
           "time.idle_skip needs time.icount_shift");
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"idle_skip\":\"yes\"}}"), "time.idle_skip 'yes': need true|false");
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"ports\":{\"control\":70000}}"), "ports.control 70000: need a TCP port 1..65535");
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"board\":\"matek\"}"), "board 'matek': not emulated (boards: nexus-xr, speedybee-f405-wing)");
    REJECT(resolve_text("{\"wiring\":[]}"), "no firmware");
    REJECT(resolve_text("{\"firmware\": }"), "line 1 column 14: expecting a value");
    /* a comma in a value is doubled in the -M string, not in the option */
    s = resolve_text("{\"firmware\":\"fw.elf\",\"aircraft\":\"" REPO_ROOT "/core/qemu/tests/" TMP "/a,b/aircraft.json\"}");
    CHECK(fb_session_ok(s) && strstr(opt(s, "aircraft-file"), "/a,b/aircraft.json") &&
          strstr(fb_session_options_str(s), "/a,,b/aircraft.json"), "comma");
    fb_session_free(s);
}

static void blob_and_manifest(void)
{
    fb_session_t *s = bench(NEW_WIRING, ",\"profile_text\":\"# c\\r\\nresource BEEPER 1 C13\\r\\n\\r\\n  set beeper_inversion = ON\"");
    size_t n;
    const char *blob = fb_session_defaults(s, &n);
    char *cfg = read_all(REPO_ROOT "/boards/nexus-xr/NEXUS_XR.config");
    CHECK(blob && !strncmp(blob, "## Rotorflight Custom Defaults\n", 31) && !strncmp(blob + 31, cfg, strlen(cfg)),
          "blob = header + board config");
    CHECK(blob && n >= 50 && !memcmp(blob + n - 50, "resource BEEPER 1 C13\n  set beeper_inversion = ON\n", 50),
          "profile_text lines last, comments/blank/CR dropped");
    CHECK(opt(s, "beeper") && !strcmp(opt(s, "beeper"), "C13:inv"), "beeper from profile_text");
    CHECK(n < FB_SESSION_DEFAULTS_MAX && !memchr(blob, '\r', n), "size, no CR");
    free(cfg);
    fb_session_free(s);
    /* too big: an error, as mkdefaults.sh */
    char *big = malloc(9000);
    strcpy(big, ",\"profile_text\":\"");
    for (int i = 0; i < 700; i++) strcat(big, "set x = 1\\n");
    strcat(big, "\"");
    REJECT(bench(NEW_WIRING, big), "defaults too big: ");
    free(big);
    /* boot bare: no blob */
    s = bench(NEW_WIRING, ",\"board_config\":\"\"");
    CHECK(fb_session_ok(s) && !fb_session_defaults(s, &n) && strstr(fb_session_report(s, NULL), "defaults: none\n"), "bare");
    fb_session_free(s);
    REJECT(bench(NEW_WIRING, ",\"profile\":\"models/rs7/nope.txt\""), "profile " REPO_ROOT "/models/rs7/nope.txt: ");
    /* the manifest: one line, parses, the machine's modelled devices marked */
    s = bench(NEW_WIRING, "");
    const char *m = fb_session_manifest(s);
    int nodes = m ? fb_json_parse(m, strlen(m), NULL, 0, NULL, 0) : -1;
    CHECK(m && !strncmp(m, "{\"type\":\"manifest\",\"core\":\"flightbench-qemu\"", 44) && strchr(m, '\n') == m + strlen(m) - 1
          && nodes > 100, "manifest");
    CHECK(m && strstr(m, "\"id\":\"msp_osd\"") && strstr(m, "\"implemented\":true}") && strstr(m, "\"id\":\"gps\""),
          "manifest devices");
    fb_session_t *mo = fb_session_manifest_only(&(fb_session_opts_t) { .root = REPO_ROOT });
    CHECK(fb_session_ok(mo) && !strcmp(fb_session_manifest(mo), m), "manifest_only = the bench's (default board config)");
    fb_session_free(mo);
    fb_session_free(s);
}

static void paths_and_root(void)
{
    /* "~" and "~/..." -> the home directory (HOME; Windows: USERPROFILE, then HOME) */
    const char *home = REPO_ROOT "/core/qemu/tests/" TMP "/home";
    set_env("HOME", home);
    set_env("USERPROFILE", home);
    write_file(TMP "/home/aircraft.json", "{\"id\": \"toy\", \"actuators\": [\"a\"]}");
    fb_session_t *s = resolve_text("{\"firmware\":\"~/fw/rf.elf\",\"profile\":\"~\\\\p.txt\",\"aircraft\":\"~/aircraft.json\","
                                   "\"board_config\":\"\"}");
    CHECK(fb_session_ok(s) && !strcmp(fb_session_firmware(s), REPO_ROOT "/core/qemu/tests/" TMP "/home/fw/rf.elf"),
          "~ firmware: %s", fb_session_firmware(s));
    CHECK(strstr(fb_session_report(s, NULL), "\nprofile: " REPO_ROOT "/core/qemu/tests/" TMP "/home/p.txt\n") != NULL,
          "~\\ profile (backslash)");
    CHECK(opt(s, "aircraft") && !strcmp(opt(s, "aircraft"), "toy") && !opt(s, "fdm"), "~ aircraft, no FDM");
    CHECK(count_notes(s, "aircraft 'toy': no FDM for it") == 1, "no FDM note");
    fb_session_free(s);
    s = resolve_text("{\"firmware\":\"~user/fw.elf\",\"board_config\":\"\"}");
    CHECK(fb_session_ok(s) && !strcmp(fb_session_firmware(s), REPO_ROOT "/~user/fw.elf"), "~user is not expanded");
    fb_session_free(s);
    /* a relative data root is taken from the current directory (make runs in core/qemu/tests) */
    fb_session_opts_t o = { .root = "../../../core/./qemu/../.." };
    s = fb_session_resolve("{\"firmware\":\"fw.elf\"}", 21, &o);
    CHECK(fb_session_ok(s) && !strcmp(fb_session_root(s), REPO_ROOT), "relative root: %s", fb_session_root(s));
    CHECK(!strcmp(fb_session_firmware(s), REPO_ROOT "/fw.elf"), "relative against the root");
    fb_session_free(s);
    /* no root: $FB_ROOT, else the nearest ancestor of the bench's directory with boards/ */
    set_env("FB_ROOT", NULL);
    s = fb_session_load("../sessions/rs7.json", NULL);
    CHECK(fb_session_ok(s) && !strcmp(fb_session_root(s), REPO_ROOT), "root found from the bench: %s", fb_session_root(s));
    fb_session_free(s);
    set_env("FB_ROOT", "../../..");
    s = fb_session_load(TMP "/home/aircraft.json", NULL);      /* not a bench: its error, with FB_ROOT's root */
    CHECK(!strcmp(fb_session_root(s), REPO_ROOT) && has_error(s, "no firmware"), "FB_ROOT: %s", fb_session_root(s));
    fb_session_free(s);
    set_env("FB_ROOT", NULL);
    s = fb_session_load(TMP "/nope.json", NULL);
    CHECK(has_error(s, "bench " TMP "/nope.json: "), "missing bench file");
    fb_session_free(s);
}

/* the old form's assumed receiver/ESC/servos only on pads the board config has (a config without
 * motor1/servo1..4 used to put entries on missing pads: a NULL pad in the report) */
static void old_form_sparse_config(void)
{
    write_file(TMP "/sparse.config", "resource GYRO_CS 1 A04\nresource SERIAL_TX 5 C12\nresource SERIAL_RX 5 D02\n");
    fb_session_t *s = resolve_text("{\"firmware\":\"fw.elf\",\"board_config\":\"core/qemu/tests/" TMP "/sparse.config\","
                                   "\"wiring\":[{\"device\":\"icm42688p\",\"slot\":\"gyro1\"}]}");
    CHECK(fb_session_ok(s) && fb_session_report(s, NULL) && !strcmp(opt(s, "esc"), "off") &&
          !strcmp(opt(s, "servos"), "0") && !strcmp(opt(s, "crsf"), "uart5"), "old form on a sparse config");
    if (!fb_session_ok(s)) dump(s);
    fb_session_free(s);
}

/* the launcher's line (docs/protocol.md "Launching the core") */
static void launch_line(void)
{
    fb_session_t *s = resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"icount_shift\":3,\"realtime\":true},"
                                   "\"ports\":{\"control\":31280,\"msp\":31281}}");
#define LINE "\nlaunch: icount_shift=3 realtime=on control=31280 msp=31281 machine=nexus-xr\n"
    const char *r = fb_session_report(s, NULL), *l = r ? strstr(r, "\nlaunch: ") : NULL, *m = r ? strstr(r, "\nmanifest: ") : NULL;
    CHECK(l && m && l < m && !strncmp(l, LINE, strlen(LINE)), "launch line");
#undef LINE
    fb_session_free(s);
    s = resolve_text("{\"firmware\":\"fw.elf\",\"time\":{\"icount_shift\":null}}");
    r = fb_session_report(s, NULL);
    CHECK(r && strstr(r, "\nlaunch: icount_shift=none realtime=off control=31250 msp=31235 machine=nexus-xr\n"), "launch line, no icount");
    fb_session_free(s);
}

/* ---- the speedybee-f405-wing (iNav, pads from board.json, a CLI script) ------------------- */

#define F405_RX "{\"device\":\"crsf\",\"port\":\"uart\",\"pad\":\"uart1\"}"

static fb_session_t *f405(const char *wiring, const char *extra)
{
    size_t n = strlen(wiring) + strlen(extra) + 256;
    char *t = malloc(n);
    snprintf(t, n, "{\"board\":\"speedybee-f405-wing\",\"firmware\":\"fw/inav.hex\",\"aircraft\":\"rs7\",\"wiring\":[%s]%s}",
             wiring, extra);
    fb_session_t *s = resolve_text(t);
    free(t);
    return s;
}

/* a copy of the speedybee's board.json with one text replaced, as the board_json override */
static const char *f405_board_with(const char *name, const char *from, const char *to)
{
    static char path[256];
    char *bj = read_all(REPO_ROOT "/boards/speedybee-f405-wing/board.json");
    char *mod = replace(bj, from, to);
    snprintf(path, sizeof path, TMP "/%s.json", name);
    write_file(path, mod);
    free(bj);
    free(mod);
    return path;
}

static int opt_is(fb_session_t *s, const char *k, const char *v)
{
    const char *o = opt(s, k);
    return v ? o && !strcmp(o, v) : !o;
}

static void speedybee(void)
{
    /* the boot bench: onboard devices only (the SPL06 modelled), the base script (comments only: no
     * script), no blob, no FDM on this board */
    fb_session_t *s = f405("", "");
    size_t n;
    const char *cli = fb_session_config_cli(s, &n), *r = fb_session_report(s, NULL);
    CHECK(fb_session_ok(s) && !strcmp(fb_session_machine(s), "speedybee-f405-wing") &&
          !strcmp(fb_session_msp_uart(s), "uart6"), "machine, msp uart");
    CHECK(!cli && n == 0 && !fb_session_defaults(s, NULL), "config_cli, no blob");
    CHECK(r && strstr(r, "\nboard: speedybee-f405-wing\nboard_machine: speedybee-f405-wing\n") &&
          strstr(r, " msp=31235 machine=speedybee-f405-wing\nmanifest: ") &&
          strstr(r, "\nconfig_cli: none\nconfig_json: {\"kind\":\"cli\",") && strstr(r, "}\ndefaults: none\n"), "report lines");
    CHECK(opt_is(s, "imu", "on") && opt_is(s, "crsf", "off") && opt_is(s, "esc", "off") && opt_is(s, "servos", "0") &&
          opt_is(s, "baro", "on") && opt_is(s, "mag", "off") && opt_is(s, "gps", "off") && opt_is(s, "battery", "off") &&
          opt_is(s, "fdm", NULL) && opt_is(s, "aircraft-file", NULL) &&
          opt_is(s, "beeper", "C15:inv") && opt_is(s, "aircraft", "rs7"), "boot options: %s", fb_session_options_str(s));
    CHECK(count_notes(s, "no wiring: speedybee-f405-wing with its onboard devices only (no receiver, no ESC, no servos)") == 1 &&
          count_notes(s, "aircraft 'rs7': no FDM for it on speedybee-f405-wing, the IMU sees a board at rest") == 1 &&
          count_notes(s, "spl06.i2c on i2c1") == 0, "boot notes");
    const char *m = fb_session_manifest(s);
    CHECK(m && strstr(m, "{\"id\":\"nexus-xr\",\"name\":\"RadioMaster Nexus-XR\",\"mcu\":\"STM32F722\",\"machine\":\"nexus-xr\","
                         "\"config\":\"unified\",\"msp_uart\":\"uart1\",\"firmware_family\":\"rotorflight\",\"slots\":") &&
          strstr(m, ",{\"id\":\"speedybee-f405-wing\",\"name\":\"SpeedyBee F405 WING\",\"mcu\":\"STM32F405\","
                    "\"machine\":\"speedybee-f405-wing\",\"config\":\"cli\",\"msp_uart\":\"uart6\",\"firmware_family\":\"inav\",\"slots\":") &&
          strstr(m, "{\"id\":\"output1\",\"label\":\"S1\",\"kind\":\"output\",\"flags\":[],\"pins\":{\"signal\":\"B07\"},"
                    "\"timer\":\"TIM4_CH2\",\"onboard\":false,\"devices\":[\"esc\",\"servo\"]}") &&
          strstr(m, "{\"id\":\"spl06\",") && strstr(m, "\"category\":\"storage\",\"kind\":\"spi\""), "manifest");
    /* the same manifest from every bench with default board configs, and without a bench */
    fb_session_t *nx = bench(NEW_WIRING, "");
    fb_session_t *mo = fb_session_manifest_only(&(fb_session_opts_t) { .root = REPO_ROOT });
    CHECK(m && fb_session_manifest(nx) && !strcmp(m, fb_session_manifest(nx)) && !strcmp(m, fb_session_manifest(mo)),
          "one manifest");
    CHECK(!strcmp(fb_session_machine(mo), "nexus-xr") && !strcmp(fb_session_msp_uart(mo), "uart1"), "manifest_only machine");
    /* nexus-xr: its machine, uart1, a blob and no CLI script */
    CHECK(!strcmp(fb_session_machine(nx), "nexus-xr") && !strcmp(fb_session_msp_uart(nx), "uart1") &&
          !fb_session_config_cli(nx, &n) && n == 0 && fb_session_defaults(nx, NULL) &&
          strstr(fb_session_report(nx, NULL), "\nconfig_cli: none\nconfig_json: {\"kind\":\"blob\",") &&
          fb_session_config_nlines(nx) == 0 && !fb_session_config_line(nx, 0, NULL, NULL), "nexus-xr: blob, no CLI");
    fb_session_free(mo);
    fb_session_free(nx);
    fb_session_free(s);

    /* outputs: a motor or a servo port; the ESC on S1 only, servos on S1..S11 */
    s = f405(F405_RX ",{\"device\":\"esc\",\"port\":\"signal\",\"pad\":\"S1\",\"actuator\":\"motor\",\"signal_timeout_ms\":50},"
             "{\"device\":\"servo\",\"pad\":\"output3\",\"actuator\":\"swash_a\"},"
             "{\"device\":\"servo\",\"pad\":\"S4\",\"actuator\":\"swash_b\",\"reversed\":true},"
             "{\"device\":\"servo\",\"pad\":\"S11\",\"actuator\":\"tail\"}", "");
    CHECK(fb_session_ok(s) && opt_is(s, "crsf", "uart1") && opt_is(s, "esc", "on") && opt_is(s, "esc-timeout-ms", "50") &&
          opt_is(s, "servos", "1036") && opt_is(s, "servo-rev", "8") &&
          opt_is(s, "actuators", "servo1:servo2:swash_a:swash_b:servo5:servo6:servo7:servo8:servo9:servo10:tail:motor"),
          "outputs: %s", fb_session_options_str(s));
    CHECK(strstr(fb_session_options_str(s), "aircraft=rs7,imu=on,crsf=uart1,esc=on,esc-timeout-ms=50,baro=on,servos=1036,"
                                            "mag=off,gps=off,battery=off,actuators=")
          != NULL, "option order: %s", fb_session_options_str(s));
    fb_session_free(s);
    s = f405("{\"device\":\"esc\",\"pad\":\"S2\"},{\"device\":\"crsf\",\"pad\":\"uart6\"},{\"device\":\"servo\",\"pad\":\"S1\"}", "");
    CHECK(fb_session_ok(s) && opt_is(s, "esc", "off") && opt_is(s, "crsf", "off") && opt_is(s, "servos", "1") &&
          count_notes(s, "esc.signal on S2 (output2) (wired): not modelled on this machine, left out") == 1 &&
          count_notes(s, "crsf.uart on uart6 (wired): not modelled") == 1, "what is not modelled");
    fb_session_free(s);
    REJECT(f405("{\"device\":\"crsf\",\"port\":\"uart\",\"pad\":\"S3\"}", ""), "crsf.uart is a uart port, pad S3 (output3) is output");
    REJECT(f405("{\"device\":\"servo\",\"pad\":\"uart2\"}", ""), "servo.pwm is a servo port, pad uart2 is uart");
    REJECT(f405("{\"device\":\"esc\",\"pad\":\"S1\"},{\"device\":\"servo\",\"pad\":\"output1\"}", ""),
           "pad S1 (output1) used by esc.signal (wiring[0]), servo.pwm (wiring[1])");
    REJECT(f405("{\"device\":\"max7456\",\"detached\":true}", ""), "onboard max7456 on osd1 is soldered");
    REJECT(f405("{\"device\":\"crsf\",\"slot\":\"uart1\"}", ""),
           "wiring: the old form ({device, slot}) is only for nexus-xr benches; on speedybee-f405-wing use {device, port, pad}");
    REJECT(f405("{\"device\":\"crsf\",\"slot\":\"uart1\"},{\"device\":\"servo\",\"pad\":\"S3\"}", ""), "mixes the old form");
    s = f405(F405_RX ",{\"device\":\"icm42688p\",\"detached\":true},{\"device\":\"spl06\",\"detached\":true}", "");
    CHECK(fb_session_ok(s) && opt_is(s, "imu", "off") && opt_is(s, "baro", "off") && count_notes(s, "spl06") == 0,
          "detach gyro and baro");
    fb_session_free(s);

    /* the sensors: the GPS on a USART, the compass on I2C1 next to the baro, the battery on BAT */
#define GPS "{\"device\":\"ublox\",\"port\":\"uart\",\"pad\":\"uart3\""
#define MAG "{\"device\":\"qmc5883l\",\"port\":\"i2c\",\"pad\":\"i2c1\""
#define BAT "{\"device\":\"battery\",\"port\":\"power\",\"pad\":\"BAT\""
    s = f405(F405_RX "," GPS "}," MAG "}," BAT "}", "");
    cli = fb_session_config_cli(s, &n);
    CHECK(fb_session_ok(s) && opt_is(s, "baro", "on") && opt_is(s, "mag", "on") && opt_is(s, "gps", "uart3") &&
          opt_is(s, "battery", "on") && opt_is(s, "gps-props", NULL) && opt_is(s, "mag-props", NULL) &&
          opt_is(s, "battery-props", NULL), "sensors: %s", fb_session_options_str(s));
    CHECK(cli && !strcmp(cli, "feature GPS\nserial 2 +1\nset gps_provider = UBLOX\nset gps_ublox_nav_hz = 10\n"),
          "the GPS's lines: %s", cli ? cli : "(none)");
    fb_session_free(s);
    s = f405(F405_RX "," GPS ",\"seed\":3,\"generation\":\"m8\",\"fix_delay_s\":5}," MAG ",\"noise_mg\":0,\"mount\":\"CW90\"},"
             BAT ",\"capacity_mah\":2200,\"cells\":3},{\"device\":\"spl06\",\"pad\":\"i2c1\",\"noise_scale\":2}", "");
    CHECK(fb_session_ok(s) && opt_is(s, "gps-props", "generation=m8:fix_delay_s=5.0:seed=3") &&
          opt_is(s, "mag-props", "mount=CW90:noise_mg=0.0") && opt_is(s, "battery-props", "cells=3:capacity_mah=2200.0") &&
          opt_is(s, "baro-props", "noise_scale=2.0"), "sensor props: %s", fb_session_options_str(s));
    fb_session_free(s);
    REJECT(f405(GPS "}," GPS ",\"pad\":\"uart4\"}", ""), "one ublox per session: uart3 (wiring[0]), uart4 (wiring[1])");
    REJECT(f405(BAT "}," BAT "}", ""), "one battery per session: BAT (batt1) (wiring[0]), BAT (batt1) (wiring[1])");
    REJECT(f405(MAG "}," MAG "}", ""), "bus i2c1: two qmc5883l (same I2C address)");
    REJECT(f405(BAT ",\"pad\":\"uart2\"}", ""), "battery.power is a power port, pad uart2 is uart");
    REJECT(f405(MAG ",\"mount\":\"CW45\"}", ""), "qmc5883l mount 'CW45': need one of CW0");
    REJECT(f405(F405_RX "," GPS ",\"pad\":\"uart1\"}", ""), "pad uart1 used by crsf.uart (wiring[0]), ublox.uart (wiring[1])");
    s = f405(GPS ",\"pad\":\"uart6\"}", "");
    CHECK(fb_session_ok(s) && opt_is(s, "gps", "off") && count_notes(s, "ublox.uart on uart6 (wired): not modelled") == 1,
          "GPS on the MSP port");
    fb_session_free(s);
#undef GPS
#undef MAG
#undef BAT

    /* the CLI script: base + profiles + profile_text + devices; save/exit dropped, reset/wipe rejected */
    write_file(TMP "/base.cli", "# base\r\nfeature -GPS\r\n\r\n   set a = 1   \r\nSAVE\n");
    s = f405(F405_RX ",{\"device\":\"msp_osd\",\"pad\":\"uart4\"}",
             ",\"board_config\":\"core/qemu/tests/" TMP "/base.cli\",\"profile\":\"core/qemu/tests/" TMP "/base.cli\","
             "\"profile_text\":\"set b = 2\\r\\nexit\\n#c\"");
    cli = fb_session_config_cli(s, &n);
    /* the OSD's lines last, iNav's (the board's firmware_family), not RotorFlight's */
    CHECK(cli && !strncmp(cli, "feature -GPS\nset a = 1\nfeature -GPS\nset a = 1\nset b = 2\n"
                               "feature OSD\nserial 3 33554432 115200 115200 0 115200\nset osd_video_system = HDZERO\n"
                               "osd_layout 0 7 2 1 V\n", 160) && strstr(cli, "\nosd_layout 0 3 0 0 V\n") &&
          !strstr(cli, "displayport_msp_serial"), "script: %s", cli ? cli : "(none)");
    CHECK(count_notes(s, "base.cli line 5: 'save' dropped (the core saves at the end)") == 2 &&
          count_notes(s, "config_cli: profile_text line 2: 'exit' dropped") == 1, "save/exit notes");
    fb_session_free(s);
    s = f405("", ",\"profile_text\":\"save\\ndefaults\\n  Reboot now\\nset x = 1\\nblx = 1\\nmsc\"");
    CHECK(!fb_session_ok(s) && fb_session_nerrors(s) == 3 && !fb_session_config_cli(s, NULL) &&
          has_error(s, "config_cli: profile_text line 2: 'defaults' would reset or wipe the board mid-script, not allowed") &&
          has_error(s, "profile_text line 3: 'reboot' would") && has_error(s, "profile_text line 6: 'msc' would") &&
          count_notes(s, "profile_text line 1: 'save' dropped") == 1, "forbidden lines");
    fb_session_free(s);
    s = f405("", ",\"board_config\":\"\"");
    CHECK(fb_session_ok(s) && !fb_session_config_cli(s, &n) && n == 0 &&
          strstr(fb_session_report(s, NULL), "\nconfig_cli: none\nconfig_json: {\"kind\":\"cli\",\"layers\":[],\"lines\":[],"
                 "\"disabled\":[],\"conflicts\":[]}\ndefaults: none\n"), "no base script: no script");
    fb_session_free(s);
    REJECT(f405("", ",\"board_config\":\"core/qemu/tests/" TMP "/nope.cli\""), "board config " REPO_ROOT "/core/qemu/tests/" TMP "/nope.cli: ");
    /* the aircraft's firmware_cli (relative to the root), after the base script */
    write_file(TMP "/ac/plane.cli", "set platform_type = AIRPLANE\nsave\n");
    char *a = read_all(REPO_ROOT "/models/rs7/aircraft.json");
    char *ac = replace(a, "\"fdm\": \"heli\",", "\"fdm\": \"heli\", \"firmware_cli\": \"core/qemu/tests/" TMP "/ac/plane.cli\",");
    write_file(TMP "/ac/aircraft.json", ac);
    s = f405("", ",\"aircraft\":\"core/qemu/tests/" TMP "/ac/aircraft.json\"");
    cli = fb_session_config_cli(s, NULL);
    CHECK(cli && !strcmp(cli, "set platform_type = AIRPLANE\n") && count_notes(s, "plane.cli line 2: 'save'") == 1,
          "aircraft firmware_cli: %s", cli ? cli : "(none)");
    fb_session_free(s);
    /* ... ignored on a blob board */
    s = bench(NEW_WIRING, ",\"aircraft\":\"core/qemu/tests/" TMP "/ac/aircraft.json\"");
    CHECK(fb_session_ok(s) && !fb_session_config_cli(s, NULL) && count_notes(s, "plane.cli") == 0, "nexus ignores firmware_cli");
    fb_session_free(s);
    free(ac);
    ac = replace(a, "\"fdm\": \"heli\",", "\"fdm\": \"heli\", \"firmware_cli\": 5,");
    write_file(TMP "/ac/aircraft.json", ac);
    REJECT(f405("", ",\"aircraft\":\"core/qemu/tests/" TMP "/ac/aircraft.json\""),
           "aircraft core/qemu/tests/" TMP "/ac/aircraft.json: firmware_cli 5: need a path");
    free(ac);
    free(a);
    /* environment: the SPL06's air */
    s = f405("", ",\"environment\":{\"sea_level_pa\":100800}");
    CHECK(fb_session_ok(s) && opt_is(s, "baro-env", "100800.0:0.0:22.0") && count_notes(s, "no modelled baro") == 0,
          "environment");
    fb_session_free(s);
    REJECT(f405("", ",\"environment\":{\"qnh\":1}"), "environment.qnh: unknown key");

    /* board.json checks */
    board_json = f405_board_with("f405-pins", "\"pins\": {\"cs\": \"A04\"}", "\"pins\": [\"A04\"]");
    REJECT(f405("", ""), "board.json pad gyro1: bad pins ['A04']");
    board_json = f405_board_with("f405-msp", "\"msp_uart\": \"uart6\"", "\"msp_uart\": \"output1\"");
    REJECT(f405("", ""), "board.json: msp_uart 'output1' is not a uart pad");
    board_json = f405_board_with("f405-beeper", "\"beeper\": \"C15:inv\"", "\"beeper\": \"PC15\"");
    REJECT(f405("", ""), "board.json: beeper 'PC15': need a pin like 'C15' or 'C15:inv'");
    board_json = f405_board_with("f405-no-beeper", "\"beeper\": \"C15:inv\",", "");
    s = f405("", "");
    CHECK(fb_session_ok(s) && opt_is(s, "beeper", NULL) && count_notes(s, "no beeper in board.json") == 1, "no beeper");
    fb_session_free(s);
    board_json = NULL;
    REJECT(resolve_text("{\"firmware\":\"fw.elf\",\"board\":null}"), "board None: not emulated (boards: nexus-xr, speedybee-f405-wing)");
}

#define F405_TRAINER F405_RX ",{\"device\":\"esc\",\"pad\":\"S1\",\"actuator\":\"motor\"}," \
    "{\"device\":\"servo\",\"pad\":\"S3\",\"actuator\":\"aileron_l\"},{\"device\":\"servo\",\"pad\":\"S4\",\"actuator\":\"aileron_r\"}," \
    "{\"device\":\"servo\",\"pad\":\"S5\",\"actuator\":\"elevator\"},{\"device\":\"servo\",\"pad\":\"S6\",\"actuator\":\"rudder\"}"

/* the plane FDM (models/trainer): the inputs it needs, env; fdm_params are the model's (docs/fdm.md) */
static void plane(void)
{
    static const struct { const char *from, *to, *want; } cases[] = {
        { "\"fdm_inputs\": [\"aileron_l\", \"aileron_r\", \"elevator\", \"rudder\"]",
          "\"fdm_inputs\": [\"aileron_l\", \"aileron_r\", \"elevator\"]", "fdm_inputs: the plane FDM needs 4" },
        { "\"throttle\": \"motor\",", "", "throttle: the plane FDM needs the ESC's actuator name" },
        { "/models/trainer/env/launch.json\",", "/models/trainer/env/nope.json\",", "/env/nope.json': no such file next to the aircraft" },
        { "\"env_default\": \"" REPO_ROOT, "\"env_default\": 5, \"x\": \"", "env_default 5: need a path" },
        { "/env/acc-calibration.json\",", "/env/nope.json\",", "functions[0].scenario '" REPO_ROOT "/models/trainer/env/nope.json': no such file" },
        { "\"auto_first_start\": false,\n   \"params\": {\"speed_kmh\"", "\"auto_first_start\": true,\n   \"params\": {\"speed_kmh\"",
          "functions[3].auto_first_start: launch is never part of the first start" },
        { "\"hold_s\": 2}", "\"hold_s\": 90}", "functions[2].params.hold_s 90: need a number 0..60" },
    };
    /* the copy lives elsewhere: its env_default absolute */
    char *orig = read_all(REPO_ROOT "/models/trainer/aircraft.json");
    char *b1 = replace(orig, "\"env/launch.json\",", "\"" REPO_ROOT "/models/trainer/env/launch.json\",");
    char *base = replace(b1, "\"env/acc-calibration.json\", \"params\"",
                         "\"" REPO_ROOT "/models/trainer/env/acc-calibration.json\", \"params\"");
    free(orig);
    free(b1);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *a = replace(base, cases[i].from, cases[i].to);
        CHECK(strcmp(a, base), "plane case %u changes the file", i);
        write_file(TMP "/ac/aircraft.json", a);
        free(a);
        REJECT(f405(F405_TRAINER, ",\"aircraft\":\"" REPO_ROOT "/core/qemu/tests/" TMP "/ac/aircraft.json\""), cases[i].want);
    }
    free(base);
    /* the trainer on its board: fdm=on, the servos by name; on the nexus-xr: no plane FDM there */
    fb_session_t *s = f405(F405_TRAINER, ",\"aircraft\":\"trainer\"");
    CHECK(fb_session_ok(s) && opt_is(s, "fdm", "on") && opt_is(s, "aircraft", "trainer") &&
          opt_is(s, "aircraft-file", REPO_ROOT "/models/trainer/aircraft.json") && opt_is(s, "servos", "60") &&
          opt_is(s, "esc", "on") && opt_is(s, "crsf", "uart1"), "trainer: %s", fb_session_options_str(s));
    CHECK(strstr(fb_session_options_str(s), "actuators=servo1:servo2:aileron_l:aileron_r:elevator:rudder:"),
          "trainer actuators: %s", fb_session_options_str(s));
    CHECK(count_notes(s, "no FDM for it") == 0, "trainer: no FDM note");
    fb_session_free(s);
    s = f405(F405_TRAINER ",{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"accel_offset_mg\":20,\"accel_scale_pct\":0.5}",
             ",\"aircraft\":\"trainer\"");
    CHECK(fb_session_ok(s) && opt_is(s, "imu-accel-error", "20.0:0.5") && opt_is(s, "imu-noise", NULL),
          "accel error: %s", fb_session_options_str(s));
    REJECT(f405("{\"device\":\"icm42688p\",\"pad\":\"gyro1\",\"accel_scale_pct\":11}", ""),
           "accel_scale_pct 11: need a number -10..10");
    fb_session_free(s);
    s = bench("{\"device\":\"servo\",\"pad\":\"servo1\",\"actuator\":\"aileron_l\"}", ",\"aircraft\":\"trainer\"");
    CHECK(fb_session_ok(s) && !opt(s, "fdm") && count_notes(s, "aircraft 'trainer': no FDM for it on nexus-xr") == 1,
          "trainer on the nexus-xr");
    fb_session_free(s);
}

/* the layers: provenance per line, bench_cli after the device lines, disabled_lines, conflicts */
static void config_layers(void)
{
    write_file(TMP "/bench.cli", "# the bench's lines\nset gps_ublox_nav_hz = 5\nfeature GPS\n");
    fb_session_t *s = f405("{\"device\":\"ublox\",\"port\":\"uart\",\"pad\":\"uart3\"}",
                           ",\"bench_cli\":\"core/qemu/tests/" TMP "/bench.cli\",\"disabled_lines\":[\"device:2\",\"x\"]");
    const char *layer, *src, *t;
    int n = fb_session_config_nlines(s);
    CHECK(fb_session_ok(s) && n == 5, "5 lines sent: %d", n);
    t = fb_session_config_line(s, 0, &layer, &src);
    CHECK(t && !strcmp(t, "feature GPS") && !strcmp(layer, "device") && !strcmp(src, "device ublox on uart3"), "line 0");
    t = fb_session_config_line(s, 3, &layer, &src);
    CHECK(t && !strcmp(t, "set gps_ublox_nav_hz = 5") && !strcmp(layer, "bench") &&
          !strcmp(src, "core/qemu/tests/" TMP "/bench.cli:2"), "line 3: %s %s", t ? t : "-", t ? src : "-");
    CHECK(!fb_session_config_line(s, 5, NULL, NULL), "past the end");
    const char *cli = fb_session_config_cli(s, NULL);
    CHECK(cli && !strstr(cli, "serial 2 +1") && strstr(cli, "\nset gps_ublox_nav_hz = 5\nfeature GPS\n"), "script");
    const char *r = fb_session_report(s, NULL);
    CHECK(r && strstr(r, "\nconfig_disabled: [device:device ublox on uart3] serial 2 +1 (disabled_lines 'device:2')\n") &&
          strstr(r, "\nconfig_conflict: set gps_ublox_nav_hz: [device:device ublox on uart3] set gps_ublox_nav_hz = 10 -> "
                    "[bench:core/qemu/tests/" TMP "/bench.cli:2] set gps_ublox_nav_hz = 5 (the last wins)\n"), "report");
    const char *j = fb_session_config_json(s);
    CHECK(j && strstr(j, "\"conflicts\":[{\"setting\":\"set gps_ublox_nav_hz\",\"lines\":[") &&
          strstr(j, "\"wins\":\"bench:1\"}]}") &&
          strstr(j, "\"disabled\":[{\"text\":\"serial 2 +1\",\"layer\":\"device\",\"source\":\"device ublox on uart3\","
                    "\"ref\":\"device:2\",\"by\":\"device:2\"}]") &&
          strstr(j, "{\"layer\":\"bench\",\"file\":\"core/qemu/tests/" TMP "/bench.cli\",\"path\":\"" REPO_ROOT), "json");
    CHECK(count_notes(s, "config: disabled_lines 'x': matches no line of the script") == 1 &&
          count_notes(s, "config: conflict: set gps_ublox_nav_hz: ") == 1, "notes");
    /* feature GPS twice (device, bench), the same value: no conflict */
    CHECK(!strstr(r, "config_conflict: feature GPS"), "the same value is no conflict");
    fb_session_free(s);
    REJECT(f405("", ",\"bench_cli\":\"core/qemu/tests/" TMP "/nope.cli\""), "bench_cli " REPO_ROOT "/core/qemu/tests/" TMP "/nope.cli: ");
    REJECT(f405("", ",\"disabled_lines\":\"device:1\""), "disabled_lines 'device:1': need a list");
    REJECT(bench(NEW_WIRING, ",\"bench_cli\":\"x.cli\""), "bench_cli: only on a CLI board");
}

int main(void)
{
    /* the scratch files (the Makefile made TMP) */
    FILE *f = fopen(TMP "/ac/.keep", "w");
    if (!f) {
        printf("FAIL: make creates " TMP "/ac, " TMP "/home, " TMP "/a,b\n");
        return 1;
    }
    fclose(f);
    char *a = read_all(REPO_ROOT "/models/rs7/aircraft.json");
    write_file(TMP "/a,b/aircraft.json", a);
    free(a);

    forms_and_migration();
    wiring_errors();
    wiring_accepted();
    osd();
    aircraft();
    bench_blocks();
    blob_and_manifest();
    paths_and_root();
    old_form_sparse_config();
    launch_line();
    speedybee();
    config_layers();
    plane();
    if (fails) {
        printf("test_session: %d of %d check(s) failed\n", fails, checks);
        return 1;
    }
    printf("test_session: all %d checks passed\n", checks);
    return 0;
}
