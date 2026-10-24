/* The settings normaliser's helpers for typing a part of a script (fb/cli_norm.h): the lines of
 * the settings the firmware changed, each with the context lines (control_profile, ...) in force
 * at it, the script's last selection at the end. */
#include "fb/cli_norm.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *const SCRIPT[] = {
    "set nav_fw_bank_angle = 35",   /* 0 */
    "aux 2 36 2 1700 2100",         /* 1 */
    "serial 2 64 115200 57600 0 115200",
    "serial 2 +1",                  /* 3: a relative serial builds on line 2 */
    "control_profile 2",            /* 4 */
    "set roll_rate = 20",           /* 5 */
    "battery_profile 2",            /* 6 */
    "set bat_cells = 3",            /* 7 */
    "control_profile 1",            /* 8 */
    "battery_profile 1",            /* 9 */
    "set pitch_rate = 30",          /* 10 */
    "set nav_fw_bank_angle = 40",   /* 11: the later line wins */
};
#define N (int)(sizeof SCRIPT / sizeof *SCRIPT)

static void seq(const unsigned char *want, const int *expect, int ne, const char *what)
{
    int out[N * 6 + 6];
    int m = fb_cli_with_context(SCRIPT, N, want, out);
    int ok = m == ne;
    for (int i = 0; ok && i < m; i++) ok = out[i] == expect[i];
    char got[256] = "";
    for (int i = 0; i < m; i++) snprintf(got + strlen(got), sizeof got - strlen(got), " %d", out[i]);
    CHECK(ok, "%s: got%s", what, got);
}

int main(void)
{
    fb_cli_line_t l[N];
    for (int i = 0; i < N; i++) l[i] = (fb_cli_line_t) { SCRIPT[i], NULL, NULL };
    unsigned char want[N];

    /* the firmware as the script leaves it: nothing changed */
    const char *same = "set nav_fw_bank_angle = 40\naux 2 36 2 1700 2100\nserial 2 66 115200 57600 0 115200\n"
                       "control_profile 2\nset roll_rate = 20\nbattery_profile 2\nset bat_cells = 3\n"
                       "control_profile 1\nset pitch_rate = 30.0\n";
    CHECK(fb_cli_changed_lines(l, N, same, NULL, 0, want) == 0, "same: changed lines");

    /* the configurator moved aux 2, the profile-2 roll rate, serial 2's GPS bit and the bank angle */
    const char *moved = "set nav_fw_bank_angle = 45\naux 2 1 3 900 1300\nserial 2 64 115200 57600 0 115200\n"
                        "control_profile 2\nset roll_rate = 25\nbattery_profile 2\nset bat_cells = 3\n"
                        "control_profile 1\nset pitch_rate = 30\n";
    int m = fb_cli_changed_lines(l, N, moved, NULL, 0, want);
    static const unsigned char w_moved[N] = { 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1 };
    CHECK(m == 6 && !memcmp(want, w_moved, N), "moved: %d changed lines", m);
    /* the lines in order: roll_rate after control_profile 2 (in force at it), the bank angle
     * after control_profile 1 and battery_profile 1 (the ones in force at line 11) */
    static const int e_moved[] = { 0, 1, 2, 3, 4, 5, 8, 9, 11 };
    seq(want, e_moved, (int)(sizeof e_moved / sizeof *e_moved), "moved");

    /* the script had the bank angle at 35 before (line 11 is new): its 45 is the delta's, not changed */
    static const char *const had[] = { "set nav_fw_bank_angle = 35", "aux 2 36 2 1700 2100",
                                       "serial 2 64 115200 57600 0 115200", "serial 2 +1", "set roll_rate = 20" };
    m = fb_cli_changed_lines(l, N, moved, had, 5, want);
    static const unsigned char w_had[N] = { 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0 };
    CHECK(m == 4 && !memcmp(want, w_had, N), "had: %d changed lines", m);

    /* a line before any context line: no context typed */
    memset(want, 0, sizeof want);
    want[1] = 1;
    static const int e_first[] = { 1 };
    seq(want, e_first, 1, "first");

    /* a battery-profile line: both contexts in force, the last selection of both after it */
    memset(want, 0, sizeof want);
    want[7] = 1;
    static const int e_bat[] = { 4, 6, 7, 8, 9 };
    seq(want, e_bat, 5, "battery");

    /* a line after the script's last selection: the selection only, nothing after it */
    memset(want, 0, sizeof want);
    want[10] = 1;
    static const int e_last[] = { 8, 9, 10 };
    seq(want, e_last, 3, "last");

    /* a wanted context line counts as typed */
    memset(want, 0, sizeof want);
    want[4] = want[5] = 1;
    static const int e_ctx[] = { 4, 5, 8 };
    seq(want, e_ctx, 3, "context line");

    if (fails) {
        printf("test_cli_norm: %d failed\n", fails);
        return 1;
    }
    printf("test_cli_norm: all passed\n");
    return 0;
}
