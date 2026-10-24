/* FlightBench: the firmware CLI lines as settings (iNav / RotorFlight `diff all` and config scripts).
 * One line -> the setting it sets (its key) and the value, so two layers of a config script (or the
 * script and the firmware's `diff all`) are compared setting by setting, not text by text. The
 * Python reference is core/qemu/scripts/cli_norm.py (the same rules; test_session_equiv.sh compares
 * both on the conflicts of every bench and on tests/fixtures/cli_diff/).
 *
 *   set <name> = <value>          key "set <name>" (lower case)
 *   feature|beeper|blackbox [-]X  key "<cmd> X" (upper case), value "on" | "off"
 *   serial|aux|smix|mmix|servo|adjrange|rxrange|logic|gvar|pid|led|safehome|temp_sensor|color|timer|
 *   geozone|fwapproach <i> ...    key "<cmd> <i>", value the rest; mode_color|osd_layout|resource
 *                                 take two index words; "<cmd> reset" clears that command's keys
 *   map <letters>                 key "map"
 *   control_profile|profile|mixer_profile|battery_profile|rateprofile <n>: the context of the
 *                                 `set`, `mmix`, `smix` lines after it: n other than 1 appends
 *                                 " @<context> <n>" (`profile` is iNav's older name of control_profile
 *                                 and RotorFlight's PID profile, `rateprofile` RotorFlight's rates;
 *                                 RotorFlight counts from 0, so its profile 0 is " @control_profile 0")
 *   comments, blank, batch, defaults, save, exit: not settings; any other command: the key is the
 *                                 whole line (whitespace collapsed), value ""
 * Values compare token by token, case-insensitively, numbers by value (1.0 = 1.000); iNav's relative
 * `serial <i> +<bit>` / `-<bit>` compares against the firmware's function mask (bauds after it are
 * not compared). */
#ifndef FB_CLI_NORM_H
#define FB_CLI_NORM_H

#include <stddef.h>

typedef enum {
    FB_CLI_OTHER,       /* not a setting */
    FB_CLI_SET,         /* key, value */
    FB_CLI_CONTEXT,     /* control_profile N ...: the context changed */
    FB_CLI_RESET,       /* smix reset ...: key = "<cmd>" + the context suffix; clears "<cmd> *" */
} fb_cli_kind_t;

typedef struct {
    char name[24];      /* "" = the default */
    char idx[16];
} fb_cli_ctx_t;

/* line -> kind; key/value malloc'd (NULL for OTHER/CONTEXT); ctx follows the context lines */
fb_cli_kind_t fb_cli_norm(fb_cli_ctx_t *ctx, const char *line, char **key, char **value);

/* 1 when the script's value a and the firmware's b are the same setting value for key */
int fb_cli_value_eq(const char *key, const char *a, const char *b);

typedef struct {
    const char *text, *layer, *source;      /* layer/source may be NULL (no provenance) */
} fb_cli_line_t;

/* The same setting in two or more layers of a script with different values (the later line
 * wins, as the firmware applies them in order). Returns the number of conflicts; *out (malloc'd,
 * fb_cli_conflicts_free) each the key and the script's line indexes that set it, in order. */
typedef struct {
    char *key;
    int n, *lines;
} fb_cli_conflict_t;
int fb_cli_conflicts(const fb_cli_line_t *l, int n, fb_cli_conflict_t **out);
void fb_cli_conflicts_free(fb_cli_conflict_t *c, int n);

/* The script's settings against the firmware's `diff all` text: the JSON members (no braces)
 * "sent":[...],"current":[...],"changed":[...],"only_firmware":[...],"only_script":[...] (malloc'd).
 * sent: the script's settings as they end (last line wins, a reset clears) {setting,value,text,
 * layer,source}; current: the diff's {setting,value,text}; changed {setting,script,firmware,layer,
 * source}; only_firmware {setting,firmware}; only_script {setting,script,layer,source}. */
char *fb_cli_diff_json(const fb_cli_line_t *script, int n, const char *diff_text);

/* want[i] = 1 for the script's lines of a setting the firmware's `diff all` has with another value
 * (fb_cli_diff_json's "changed"; every line of that setting, a relative serial builds on the ones
 * before it); returns how many. had (nhad lines; NULL = no filter): only the settings whose last
 * script line is one of had (a line the firmware was given before: its value was changed since, not
 * the script's). The settings the diff does not show (the firmware's default) are not marked: the
 * diff cannot tell a default the script repeats from one the configurator set. */
int fb_cli_changed_lines(const fb_cli_line_t *script, int n, const char *diff_text, const char *const *had,
                         int nhad, unsigned char *want);

/* The lines to type for a part of a script (want[i] = 1): out[] = indexes in the script's order,
 * each wanted line preceded by the context lines (control_profile, mixer_profile, ...) in force at
 * it when they differ from the ones typed before, and at the end the script's last context lines
 * where they differ (the selection the whole script leaves). out needs room for n * 6 + 6. */
int fb_cli_with_context(const char *const *lines, int n, const unsigned char *want, int *out);

/* a JSON string of z (Python's json.dumps(ensure_ascii=False) escaping) appended to a malloc'd
 * buffer (buf, len, cap: a growable string); returns 0, -1 out of memory */
int fb_cli_json_str(char **buf, size_t *len, size_t *cap, const char *z);

#endif
