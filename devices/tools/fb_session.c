/* Resolves a bench on the host, printing what -M nexus-xr,session=<bench>,session-check=on prints
 * (src/session.c; include/fb/session.h): warnings and errors on stderr with "session: ", the
 * report on stdout; exit 2 when the bench is rejected.
 *   make tools && build/fb_session [--root <dir>] [--board-json <file>] <bench.json>
 *   build/fb_session [--root <dir>] --manifest       the manifest only (default board config)
 *   build/fb_session --cli-diff <script.txt> <diff.txt>
 *                                  the script (one CLI line each) against a `diff all` text: the JSON
 *                                  object of fb_cli_diff_json (include/fb/cli_norm.h), one line; the
 *                                  Python reference: core/qemu/scripts/cli_norm.py --diff */
#include "fb/cli_norm.h"
#include "fb/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    size_t n = 0, cap = 4096;
    char *b = malloc(cap);
    for (size_t k; b && (k = fread(b + n, 1, cap - n - 1, f)) > 0;) {
        n += k;
        if (cap - n < 2) b = realloc(b, cap *= 2);
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

static int cli_diff(const char *script, const char *diff)
{
    char *st = slurp(script), *dt = slurp(diff);
    if (!st || !dt) {
        fprintf(stderr, "fb_session: cannot read %s\n", !st ? script : diff);
        return 2;
    }
    /* the script's lines, whitespace stripped, blank ones dropped (cli_norm.py --diff on a text file) */
    fb_cli_line_t *l = NULL;
    int n = 0;
    for (char *p = st; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *a = p;
        while (*a && strchr(" \t\r\v\f", *a)) a++;
        size_t k = strlen(a);
        while (k && strchr(" \t\r\v\f", a[k - 1])) a[--k] = 0;
        if (*a) {
            l = realloc(l, sizeof *l * (size_t)(n + 1));
            l[n++] = (fb_cli_line_t) { a, NULL, NULL };
        }
        if (!e) break;
        p = e + 1;
    }
    char *j = fb_cli_diff_json(l, n, dt);
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    printf("{%s}\n", j);
    free(j);
    free(l);
    free(st);
    free(dt);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--cli-diff")) return cli_diff(argv[2], argv[3]);
    fb_session_opts_t o = { 0 };
    const char *bench = NULL;
    int manifest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) o.root = argv[++i];
        else if (!strcmp(argv[i], "--board-json") && i + 1 < argc) o.board_json = argv[++i];
        else if (!strcmp(argv[i], "--manifest")) manifest = 1;
        else if (argv[i][0] != '-' && !bench) bench = argv[i];
        else {
            fprintf(stderr, "usage: fb_session [--root dir] [--board-json file] <bench.json> | --manifest\n");
            return 2;
        }
    }
    if (!bench && !manifest) {
        fprintf(stderr, "usage: fb_session [--root dir] [--board-json file] <bench.json> | --manifest\n");
        return 2;
    }
    fb_session_t *s = bench ? fb_session_load(bench, &o) : fb_session_manifest_only(&o);
    for (int i = 0; i < fb_session_nwarnings(s); i++) fprintf(stderr, "session: %s\n", fb_session_warning(s, i));
    for (int i = 0; i < fb_session_nerrors(s); i++) fprintf(stderr, "session: %s\n", fb_session_error(s, i));
    int rc = fb_session_ok(s) ? 0 : 2;
    if (rc == 0) {
        size_t n;
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);  /* the report's blob is N exact bytes, no CRLF */
#endif
        const char *r = bench ? fb_session_report(s, &n) : fb_session_manifest(s);
        if (!bench) n = strlen(r);
        fwrite(r, 1, n, stdout);
    }
    fb_session_free(s);
    return rc;
}
