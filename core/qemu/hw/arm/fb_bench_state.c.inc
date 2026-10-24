/*
 * FlightBench: the bench state's decisions (-M bench-state=<file>; nexus_xr_ctl.c.inc bs_*).
 * Plain C, no QEMU or glib: the control server and the host test core/qemu/tests/
 * test_bench_state.c include it. The files themselves (written through a temporary file and a
 * rename, the metadata's JSON) stay in the control server.
 *
 *   fbbs_json_name  X.state.bin -> X.state.json (the metadata next to the sector), else X.json
 *   fbbs_verdict    whether a sector + its metadata are restored at start, and why not (a newer
 *                   format: left alone, the bench state off for the run)
 *   fbbs_delta      the script's lines to send on the restored sector (added or changed: not in
 *                   the script the sector has) and the ones it had that are gone (reported only)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define FBBS_FORMAT     1

/* the metadata's name for the sector file bin into out; false when it does not fit */
static bool fbbs_json_name(const char *bin, char *out, size_t cap)
{
    size_t n = strlen(bin);
    int w = n > 4 && !strcasecmp(bin + n - 4, ".bin")
            ? snprintf(out, cap, "%.*s.json", (int)(n - 4), bin)
            : snprintf(out, cap, "%s.json", bin);
    return w >= 0 && (size_t)w < cap;
}

typedef enum {
    FBBS_RESTORE,           /* restore it */
    FBBS_RESTORE_OTHER_SHA, /* restore it; the metadata describes another sector (a write cut
                             * between the two renames): the metadata may be older */
    FBBS_SIZE,              /* the sector file is not the config sector's size */
    FBBS_UNREADABLE,        /* the metadata is not JSON, another format, machine, or has no script */
    FBBS_FIRMWARE,          /* another firmware's config */
    FBBS_NEWER,             /* written by a newer core (a later format): not restored and not
                             * overwritten by this one */
} FbbsVerdict;

typedef struct {
    bool parsed;            /* the metadata is a JSON object */
    int64_t format;
    const char *machine, *firmware, *script, *sector_sha256;   /* NULL = absent */
} FbbsMeta;

static int fbbs_streq(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

/* have: the sector file's size, sha: its sha256 (hex); why: a short reason (not for RESTORE) */
static FbbsVerdict fbbs_verdict(const FbbsMeta *m, size_t have, size_t want, const char *sha,
                                const char *machine, const char *firmware, const char **why)
{
    *why = NULL;
    if (have != want) {
        *why = "not the config sector's size";
        return FBBS_SIZE;
    }
    if (!m->parsed) {
        *why = "not a JSON object";
        return FBBS_UNREADABLE;
    }
    if (m->format > FBBS_FORMAT) {
        *why = "a newer core's format";
        return FBBS_NEWER;
    }
    if (m->format != FBBS_FORMAT) {
        *why = "not format 1";
        return FBBS_UNREADABLE;
    }
    if (!fbbs_streq(m->machine, machine)) {
        *why = "another machine's";
        return FBBS_UNREADABLE;
    }
    if (!m->script) {
        *why = "no script";
        return FBBS_UNREADABLE;
    }
    if (!fbbs_streq(m->firmware, firmware)) {
        *why = "another firmware's";
        return FBBS_FIRMWARE;
    }
    if (!fbbs_streq(m->sector_sha256, sha)) {
        *why = "the metadata describes another sector";
        return FBBS_RESTORE_OTHER_SHA;
    }
    return FBBS_RESTORE;
}

static bool fbbs_has(const char *const *lines, int n, const char *l)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(lines[i], l)) {
            return true;
        }
    }
    return false;
}

/* cur: the script's lines now, had: the lines of the script the sector has (both without
 * comments, blank lines, save and exit). added[]: indexes into cur of the lines had does not have,
 * in the script's order (a changed `set x = 2` over `set x = 1` is one added + one removed);
 * removed[] (may be NULL): indexes into had of the lines cur no longer has. Returns the number
 * added; *nremoved the number removed. The scripts are ~150 lines: quadratic is fine. */
static int fbbs_delta(const char *const *cur, int ncur, const char *const *had, int nhad,
                      int *added, int *removed, int *nremoved)
{
    int na = 0, nr = 0;
    for (int i = 0; i < ncur; i++) {
        if (!fbbs_has(had, nhad, cur[i])) {
            added[na++] = i;
        }
    }
    for (int i = 0; i < nhad; i++) {
        if (!fbbs_has(cur, ncur, had[i])) {
            if (removed) {
                removed[nr] = i;
            }
            nr++;
        }
    }
    *nremoved = nr;
    return na;
}
