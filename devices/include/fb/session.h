/* FlightBench bench resolver: a bench (docs/ui.md: plate + firmware + aircraft + wiring, the
 * JSON of core/qemu/sessions/ benches) -> what the board's machine builds. Plain C, no emulator
 * dependency: the machine (-M nexus-xr,session=<bench>) and the host tool/tests use the same code.
 *
 * Boards (the BOARDS table in src/session.c, wiring.py MACHINES): nexus-xr (RotorFlight, a unified
 * config, the custom-defaults blob; machine nexus-xr, MSP on uart1) and speedybee-f405-wing (iNav
 * 9.1.0 SPEEDYBEEF405WING, pads from board.json alone, configured by a CLI script sent over the MSP
 * UART; machine speedybee-f405-wing, MSP on uart6).
 *
 * It does what core/qemu/run.sh --session did through scripts/session.py, wiring.py,
 * mkdefaults.sh and manifest.py (kept as the reference implementation; the equivalence test
 * core/qemu/tests/test_session_equiv.sh compares both on every bench):
 *   - the wiring (both forms: old {device, slot} (nexus-xr only), new {device, port, pad} /
 *     detached) checked against the board (unified config + boards/<id>/board.json, or board.json
 *     alone) and the device descriptors (fb/device.h), with the same errors and notes as wiring.py
 *   - the machine options (-M imu/crsf/esc/baro/servos/osd/actuators/..., aircraft, fdm,
 *     beeper, baro-env, realtime, idle-skip) in session.py's order, values unquoted
 *   - the custom-defaults blob (mkdefaults.sh byte for byte: header, board config, profiles
 *     without comment/blank lines, the wired devices' CLI lines), at most FB_SESSION_DEFAULTS_MAX;
 *     on a CLI board instead the CLI script (session.py config_cli)
 *   - the core manifest (manifest.py byte for byte: one JSON line)
 *   - the time block (icount shift, realtime, idle skip) and the ports the launcher needs
 *
 * Paths in the bench resolve against the data root (where boards/ and models/ are); "~" and
 * "~/..." expand to the home directory (HOME; on Windows USERPROFILE, then HOME). All strings are
 * UTF-8 (on Windows too: files are opened through their UTF-16 names).
 *
 * The result owns all its strings; errors and warnings are lines without the "session: " prefix
 * (the caller prints them with it). A bench with errors has no options, blob or manifest. */
#ifndef FB_SESSION_H
#define FB_SESSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0x08004000 - 0x08002808: the F7 custom-defaults area; the blob must be smaller */
#define FB_SESSION_DEFAULTS_MAX 6136

typedef struct fb_session fb_session_t;

typedef struct {
    /* data root with boards/ and models/. NULL or "": $FB_ROOT, else the nearest ancestor of
     * the bench file's directory that has boards/<board>/board.json (the bench's two levels up
     * when it sits in <root>/<dir>/), else the current directory. A relative root is taken
     * from the current directory. */
    const char *root;
    const char *bench_path;     /* the bench file (for the default root); NULL: from text only */
    const char *board_json;     /* instead of <root>/boards/<id>/board.json (tests) */
} fb_session_opts_t;

/* Reads and resolves a bench file (bench_path is `path`). Never returns NULL. */
fb_session_t *fb_session_load(const char *path, const fb_session_opts_t *o);
/* Resolves bench JSON text. Never returns NULL. */
fb_session_t *fb_session_resolve(const char *text, size_t n, const fb_session_opts_t *o);
/* No bench: only the manifest, for the default board config (run.sh --manifest) */
fb_session_t *fb_session_manifest_only(const fb_session_opts_t *o);
void fb_session_free(fb_session_t *s);

int fb_session_ok(const fb_session_t *s);
int fb_session_nerrors(const fb_session_t *s);
const char *fb_session_error(const fb_session_t *s, int i);
int fb_session_nwarnings(const fb_session_t *s);
const char *fb_session_warning(const fb_session_t *s, int i);

const char *fb_session_root(const fb_session_t *s);
const char *fb_session_firmware(const fb_session_t *s);
/* Machine options in session.py's SESSION_OPTS order; values unquoted (a -M string needs
 * "," doubled, see fb_session_options_str) */
int fb_session_noptions(const fb_session_t *s);
const char *fb_session_option_key(const fb_session_t *s, int i);
const char *fb_session_option_value(const fb_session_t *s, int i);
/* SESSION_OPTS: key=value,... with a literal "," written ",," */
const char *fb_session_options_str(const fb_session_t *s);
int fb_session_icount_shift(const fb_session_t *s);    /* -1: time.icount_shift null */
int fb_session_realtime(const fb_session_t *s);
int fb_session_port_control(const fb_session_t *s);
int fb_session_port_msp(const fb_session_t *s);
/* The QEMU machine type of the bench's board ("nexus-xr", "speedybee-f405-wing"; board.json
 * `machine`, default the board id); fb_session_manifest_only: the default board's (nexus-xr).
 * NULL when the bench names no known board. */
const char *fb_session_machine(const fb_session_t *s);
/* The pad id of the board's MSP/CLI host UART (board.json `msp_uart`: "uart1" on nexus-xr,
 * "uart6" on speedybee-f405-wing); NULL until the board is loaded */
const char *fb_session_msp_uart(const fb_session_t *s);
/* The custom-defaults blob, NULL when the bench has board_config "" (boot bare) or its board is
 * configured by a CLI script */
const char *fb_session_defaults(const fb_session_t *s, size_t *len);
/* A CLI-configured board's script (board.json "config": "cli"): the lines the machine sends over
 * the MSP UART, each ending in "\n" (the board's base script = board_config, default board.json
 * `defaults_cli`; the aircraft's `firmware_cli`; the profiles and profile_text; the wired devices'
 * `config` lines; stripped, without blank/comment lines and without save/exit: the machine saves
 * at the end). NULL for a blob board, an empty script or a rejected bench. */
const char *fb_session_config_cli(const fb_session_t *s, size_t *len);
/* The manifest: one JSON line ending in "\n" */
/* A CLI board's script line by line with its provenance (the lines fb_session_config_cli has, in
 * order; disabled_lines left out): layer "board" | "aircraft" | "profile" | "device" | "bench",
 * source "boards/x/defaults.cli:12" | "device ublox on uart3" | "profile_text:3". 0 / NULL on a
 * blob board or a rejected bench. */
int fb_session_config_nlines(const fb_session_t *s);
const char *fb_session_config_line(const fb_session_t *s, int i, const char **layer, const char **source);
/* the layered assembly as one line of JSON (the report's config_json: kind, layers (files), lines,
 * disabled, conflicts); NULL for a rejected bench */
const char *fb_session_config_json(fb_session_t *s);
const char *fb_session_manifest(const fb_session_t *s);

/* What -M session-check=on prints (NUL-terminated, owned by s; NULL when !ok): the assembly as
 * `session.py --check` prints it, then key: value lines (root, firmware, board, board_machine,
 * board_config, aircraft, profile, options, icount, realtime, ports, launch (ending in
 * machine=<name>), manifest), then "config_cli: none" or "config_cli: N lines" followed by N lines
 * "config_cli| <line>", and last "defaults: N bytes (max M)" followed by exactly the N blob bytes
 * ("defaults: none" without a blob). */
const char *fb_session_report(const fb_session_t *s, size_t *len);

#ifdef __cplusplus
}
#endif
#endif
