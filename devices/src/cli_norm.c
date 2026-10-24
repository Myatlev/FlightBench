/* The firmware CLI lines as settings: include/fb/cli_norm.h. Python reference:
 * core/qemu/scripts/cli_norm.py (the same rules, line for line). */
#include "fb/cli_norm.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t len, cap;
} buf_t;

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "cli_norm: out of memory\n");
        abort();
    }
    return q;
}

static void b_add(buf_t *b, const char *z, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, z, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void b_puts(buf_t *b, const char *z) { b_add(b, z, strlen(z)); }

static char *b_take(buf_t *b)
{
    if (!b->p) b_add(b, "", 0);
    return b->p;
}

int fb_cli_json_str(char **buf, size_t *len, size_t *cap, const char *z)
{
    buf_t b = { *buf, *len, *cap };
    b_add(&b, "\"", 1);
    for (; *z; z++) {
        unsigned char c = (unsigned char)*z;
        char e[8];
        if (c == '"') b_puts(&b, "\\\"");
        else if (c == '\\') b_puts(&b, "\\\\");
        else if (c == '\n') b_puts(&b, "\\n");
        else if (c == '\r') b_puts(&b, "\\r");
        else if (c == '\t') b_puts(&b, "\\t");
        else if (c == '\b') b_puts(&b, "\\b");
        else if (c == '\f') b_puts(&b, "\\f");
        else if (c < 0x20) {
            snprintf(e, sizeof e, "\\u%04x", c);
            b_puts(&b, e);
        } else {
            b_add(&b, (const char *)&c, 1);
        }
    }
    b_add(&b, "\"", 1);
    *buf = b.p;
    *len = b.len;
    *cap = b.cap;
    return 0;
}

static void b_json(buf_t *b, const char *z)
{
    if (!z) {
        b_puts(b, "null");
        return;
    }
    fb_cli_json_str(&b->p, &b->len, &b->cap, z);
}

/* ---- one line ------------------------------------------------------------------------------ */

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

/* the line's words (ASCII whitespace): *n of them in a malloc'd array of malloc'd strings */
static char **words(const char *z, int *n)
{
    char **w = NULL;
    int k = 0;
    while (*z) {
        while (*z && is_ws(*z)) z++;
        if (!*z) break;
        const char *a = z;
        while (*z && !is_ws(*z)) z++;
        w = xrealloc(w, sizeof *w * (size_t)(k + 1));
        w[k] = xrealloc(NULL, (size_t)(z - a) + 1);
        memcpy(w[k], a, (size_t)(z - a));
        w[k][z - a] = 0;
        k++;
    }
    *n = k;
    return w;
}

static void words_free(char **w, int n)
{
    for (int i = 0; i < n; i++) free(w[i]);
    free(w);
}

/* words[from..] joined by one space */
static char *join(char **w, int from, int n)
{
    buf_t b = { 0 };
    for (int i = from; i < n; i++) {
        if (i > from) b_add(&b, " ", 1);
        b_puts(&b, w[i]);
    }
    return b_take(&b);
}

static char *dup_case(const char *z, int up)
{
    size_t n = strlen(z);
    char *d = xrealloc(NULL, n + 1);
    for (size_t i = 0; i <= n; i++) {
        unsigned char c = (unsigned char)z[i];
        d[i] = (char)(c < 0x80 ? (up ? toupper(c) : tolower(c)) : c);
    }
    return d;
}

static char *fmt2(const char *a, const char *b, const char *c)
{
    buf_t x = { 0 };
    b_puts(&x, a);
    b_puts(&x, b);
    b_puts(&x, c);
    return b_take(&x);
}

static const struct { const char *cmd; int nidx; } INDEXED[] = {
    { "serial", 1 }, { "aux", 1 }, { "smix", 1 }, { "mmix", 1 }, { "servo", 1 }, { "adjrange", 1 },
    { "rxrange", 1 }, { "logic", 1 }, { "gvar", 1 }, { "pid", 1 }, { "led", 1 }, { "safehome", 1 },
    { "temp_sensor", 1 }, { "color", 1 }, { "timer", 1 }, { "geozone", 1 }, { "fwapproach", 1 },
    { "mode_color", 2 }, { "osd_layout", 2 }, { "resource", 2 },
};
static const char *const FLAGS[] = { "feature", "beeper", "blackbox" };
static const char *const IGNORED[] = { "batch", "defaults", "save", "exit" };
static const char *const CONTEXTS[] = { "control_profile", "profile", "mixer_profile", "battery_profile",
                                         "rateprofile" };

static const char *ctx_suffix(const fb_cli_ctx_t *c, char *out, size_t cap)
{
    if (!c->name[0] || !strcmp(c->idx, "1")) return "";
    snprintf(out, cap, " @%s %s", c->name, c->idx);
    return out;
}

fb_cli_kind_t fb_cli_norm(fb_cli_ctx_t *ctx, const char *line, char **key, char **value)
{
    *key = *value = NULL;
    int n;
    char **w = words(line, &n);
    fb_cli_kind_t k = FB_CLI_OTHER;
    char sfx[64];
    if (!n || w[0][0] == '#') goto out;
    char *cmd = dup_case(w[0], 0);
    for (size_t i = 0; i < sizeof IGNORED / sizeof *IGNORED; i++) {
        if (!strcmp(cmd, IGNORED[i])) {
            free(cmd);
            goto out;
        }
    }
    for (size_t i = 0; i < sizeof CONTEXTS / sizeof *CONTEXTS; i++) {
        if (!strcmp(cmd, CONTEXTS[i]) && n == 2) {
            snprintf(ctx->name, sizeof ctx->name, "%s", i == 1 ? "control_profile" : cmd);
            snprintf(ctx->idx, sizeof ctx->idx, "%s", w[1]);
            k = FB_CLI_CONTEXT;
            free(cmd);
            goto out;
        }
    }
    if (!strcmp(cmd, "set")) {
        /* set <name> = <value>: the '=' may touch either word */
        char *rest = join(w, 1, n);
        char *eq = strchr(rest, '=');
        if (eq) {
            char *a = rest, *e = eq;
            while (e > a && is_ws(e[-1])) e--;
            *e = 0;
            char *v = eq + 1;
            while (*v && is_ws(*v)) v++;
            if (*a) {
                char *name = dup_case(a, 0);
                *key = fmt2("set ", name, ctx_suffix(ctx, sfx, sizeof sfx));
                int nv;
                char **vw = words(v, &nv);
                *value = join(vw, 0, nv);
                words_free(vw, nv);
                free(name);
                k = FB_CLI_SET;
            }
        }
        free(rest);
        free(cmd);
        goto out;
    }
    for (size_t i = 0; i < sizeof FLAGS / sizeof *FLAGS; i++) {
        if (!strcmp(cmd, FLAGS[i])) {
            if (n >= 2 && w[1][0] && strcmp(w[1], "-")) {
                int off = w[1][0] == '-';
                char *name = dup_case(w[1] + off, 1);
                *key = fmt2(cmd, " ", name);
                *value = dup_case(off ? "off" : "on", 0);
                free(name);
                k = FB_CLI_SET;
            }
            free(cmd);
            goto out;
        }
    }
    if (!strcmp(cmd, "map")) {
        if (n >= 2) {
            *key = dup_case("map", 0);
            *value = join(w, 1, n);
            k = FB_CLI_SET;
        }
        free(cmd);
        goto out;
    }
    for (size_t i = 0; i < sizeof INDEXED / sizeof *INDEXED; i++) {
        if (strcmp(cmd, INDEXED[i].cmd)) continue;
        int mix = !strcmp(cmd, "smix") || !strcmp(cmd, "mmix");
        const char *sf = mix ? ctx_suffix(ctx, sfx, sizeof sfx) : "";
        if (n >= 2 && !strcmp(w[1], "reset")) {
            *key = fmt2(cmd, "", sf);
            k = FB_CLI_RESET;
        } else if (n > INDEXED[i].nidx) {
            char *idx = join(w, 1, 1 + INDEXED[i].nidx);
            char *head = fmt2(cmd, " ", idx);
            *key = fmt2(head, sf, "");
            *value = join(w, 1 + INDEXED[i].nidx, n);
            free(idx);
            free(head);
            k = FB_CLI_SET;
        }
        free(cmd);
        goto out;
    }
    /* any other command: the line itself */
    free(cmd);
    *key = join(w, 0, n);
    *value = dup_case("", 0);
    k = FB_CLI_SET;
out:
    words_free(w, n);
    return k;
}

/* ---- values -------------------------------------------------------------------------------- */

/* a decimal number ([+-]digits[.digits][e[+-]digits] or [+-].digits...): no hex, inf or nan */
static int num(const char *z, double *d)
{
    const char *p = z;
    int digits = 0;
    if (*p == '+' || *p == '-') p++;
    while (*p >= '0' && *p <= '9') p++, digits++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++, digits++;
    }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return 0;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p) return 0;
    *d = strtod(z, NULL);
    return 1;
}

static int ascii_caseeq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x < 0x80) x = (unsigned char)tolower(x);
        if (y < 0x80) y = (unsigned char)tolower(y);
        if (x != y) return 0;
    }
    return *a == *b;
}

static int tok_eq(const char *a, const char *b)
{
    double x, y;
    if (ascii_caseeq(a, b)) return 1;
    return num(a, &x) && num(b, &y) && x == y;
}

static int is_serial(const char *key) { return !strncmp(key, "serial ", 7); }
static int is_rel(const char *v) { return v[0] == '+' || v[0] == '-'; }

/* the bit of a relative serial word after its sign: 0..31, else -1 (strtol clamps what does not fit
 * a long: atoi's int conversion would wrap +4294967297 to bit 1) */
static int rel_bit(const char *z)
{
    long b = strtol(z, NULL, 10);
    return b >= 0 && b <= 31 ? (int)b : -1;
}

int fb_cli_value_eq(const char *key, const char *a, const char *b)
{
    int na, nb, r = 1;
    char **wa = words(a, &na), **wb = words(b, &nb);
    if (is_serial(key) && na && is_rel(wa[0]) && !(nb && is_rel(wb[0]))) {
        /* iNav's relative form: the bits against the firmware's function mask */
        unsigned long m = nb ? strtoul(wb[0], NULL, 10) : 0;
        for (int i = 0; i < na && r; i++) {
            if (!is_rel(wa[i])) break;      /* bauds after a relative form: not compared */
            int bit = rel_bit(wa[i] + 1);
            int set = bit >= 0 && (m >> bit & 1);
            r = wa[i][0] == '+' ? set : !set;
        }
    } else if (is_serial(key) && nb && is_rel(wb[0]) && !(na && is_rel(wa[0]))) {
        r = fb_cli_value_eq(key, b, a);
    } else if (na != nb) {
        r = 0;
    } else {
        for (int i = 0; i < na && r; i++) r = tok_eq(wa[i], wb[i]);
    }
    words_free(wa, na);
    words_free(wb, nb);
    return r;
}

/* ---- a script's settings as they end ------------------------------------------------------- */

typedef struct {
    char *key, *value;
    int line;           /* the line that set it last */
} eff_t;

typedef struct {
    eff_t *v;
    int n;
} effs_t;

static int eff_find(const effs_t *e, const char *key)
{
    for (int i = 0; i < e->n; i++) {
        if (!strcmp(e->v[i].key, key)) return i;
    }
    return -1;
}

/* key "<cmd> <idx>[ @ctx n]" is cleared by a reset "<cmd>[ @ctx n]" */
static int reset_hits(const char *reset, const char *key)
{
    const char *rs = strstr(reset, " @");
    size_t cl = rs ? (size_t)(rs - reset) : strlen(reset);
    if (strncmp(key, reset, cl) || key[cl] != ' ') return 0;
    const char *ks = strstr(key, " @");
    return !strcmp(ks ? ks : "", rs ? rs : "");
}

/* old (absolute or relative) + a relative serial value */
static char *serial_combine(const char *old, const char *rel)
{
    int no, nr;
    char **wo = words(old, &no), **wr = words(rel, &nr);
    char *r;
    if (no && !is_rel(wo[0])) {
        unsigned long m = strtoul(wo[0], NULL, 10);
        for (int i = 0; i < nr && is_rel(wr[i]); i++) {
            int bit = rel_bit(wr[i] + 1);
            if (bit < 0) continue;
            if (wr[i][0] == '+') m |= 1ul << bit;
            else m &= ~(1ul << bit);
        }
        char mb[24];
        snprintf(mb, sizeof mb, "%lu", m);
        char *rest = join(wo, 1, no);
        r = fmt2(mb, *rest ? " " : "", rest);
        free(rest);
    } else {
        r = fmt2(old, " ", rel);
    }
    words_free(wo, no);
    words_free(wr, nr);
    return r;
}

static void effective(const fb_cli_line_t *l, int n, effs_t *e)
{
    fb_cli_ctx_t ctx = { { 0 }, { 0 } };
    e->v = NULL;
    e->n = 0;
    for (int i = 0; i < n; i++) {
        char *k, *v;
        fb_cli_kind_t kind = fb_cli_norm(&ctx, l[i].text, &k, &v);
        if (kind == FB_CLI_RESET) {
            int o = 0;
            for (int j = 0; j < e->n; j++) {
                if (reset_hits(k, e->v[j].key)) {
                    free(e->v[j].key);
                    free(e->v[j].value);
                } else {
                    e->v[o++] = e->v[j];
                }
            }
            e->n = o;
        } else if (kind == FB_CLI_SET) {
            int j = eff_find(e, k);
            if (j >= 0) {
                char *nv = is_serial(k) && is_rel(v) ? serial_combine(e->v[j].value, v) : NULL;
                free(e->v[j].value);
                e->v[j].value = nv ? nv : v;
                if (nv) free(v);
                e->v[j].line = i;
                free(k);
                continue;
            }
            e->v = xrealloc(e->v, sizeof *e->v * (size_t)(e->n + 1));
            e->v[e->n++] = (eff_t) { k, v, i };
            continue;
        }
        free(k);
        free(v);
    }
}

static void effs_free(effs_t *e)
{
    for (int i = 0; i < e->n; i++) {
        free(e->v[i].key);
        free(e->v[i].value);
    }
    free(e->v);
}

static int by_line(const void *a, const void *b)
{
    return ((const eff_t *)a)->line - ((const eff_t *)b)->line;
}

/* ---- conflicts ----------------------------------------------------------------------------- */

int fb_cli_conflicts(const fb_cli_line_t *l, int n, fb_cli_conflict_t **out)
{
    /* every SET line's key and value, in order */
    char **keys = xrealloc(NULL, sizeof *keys * (size_t)(n + 1));
    char **vals = xrealloc(NULL, sizeof *vals * (size_t)(n + 1));
    fb_cli_ctx_t ctx = { { 0 }, { 0 } };
    for (int i = 0; i < n; i++) {
        if (fb_cli_norm(&ctx, l[i].text, &keys[i], &vals[i]) != FB_CLI_SET) {
            free(keys[i]);
            free(vals[i]);
            keys[i] = vals[i] = NULL;
        }
    }
    fb_cli_conflict_t *c = NULL;
    int nc = 0;
    char *done = xrealloc(NULL, (size_t)n + 1);
    memset(done, 0, (size_t)n + 1);
    for (int i = 0; i < n; i++) {
        if (!keys[i] || done[i]) continue;
        int *idx = xrealloc(NULL, sizeof *idx * (size_t)n);
        int m = 0, layers = 0, differ = 0, last = i;
        for (int j = i; j < n; j++) {
            if (keys[j] && !strcmp(keys[j], keys[i])) {
                done[j] = 1;
                idx[m++] = j;
                last = j;
            }
        }
        for (int a = 0; a < m; a++) {
            const char *la = l[idx[a]].layer ? l[idx[a]].layer : "";
            int seen = 0;
            for (int b = 0; b < a; b++) seen |= !strcmp(la, l[idx[b]].layer ? l[idx[b]].layer : "");
            layers += !seen;
            differ |= !fb_cli_value_eq(keys[i], vals[idx[a]], vals[last]);
        }
        if (layers >= 2 && differ) {
            c = xrealloc(c, sizeof *c * (size_t)(nc + 1));
            c[nc].key = keys[i];
            keys[i] = NULL;
            c[nc].n = m;
            c[nc].lines = idx;
            nc++;
        } else {
            free(idx);
        }
    }
    for (int i = 0; i < n; i++) {
        free(keys[i]);
        free(vals[i]);
    }
    free(keys);
    free(vals);
    free(done);
    *out = c;
    return nc;
}

void fb_cli_conflicts_free(fb_cli_conflict_t *c, int n)
{
    for (int i = 0; i < n; i++) {
        free(c[i].key);
        free(c[i].lines);
    }
    free(c);
}

/* ---- the script against the firmware's diff ------------------------------------------------ */

static void kv(buf_t *b, const char *k, const char *v, int first)
{
    if (!first) b_add(b, ",", 1);
    b_json(b, k);
    b_add(b, ":", 1);
    b_json(b, v);
}

char *fb_cli_diff_json(const fb_cli_line_t *script, int n, const char *diff_text)
{
    /* the diff's lines */
    fb_cli_line_t *fl = NULL;
    int nf = 0;
    char *copy = fmt2(diff_text ? diff_text : "", "", "");
    for (char *p = copy; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        char *a = p;
        while (*a && is_ws(*a)) a++;
        size_t len = strlen(a);
        while (len && is_ws(a[len - 1])) a[--len] = 0;
        fl = xrealloc(fl, sizeof *fl * (size_t)(nf + 1));
        fl[nf++] = (fb_cli_line_t) { a, NULL, NULL };
        if (!e) break;
        p = e + 1;
    }
    effs_t es, ef;
    effective(script, n, &es);
    effective(fl, nf, &ef);
    qsort(es.v, (size_t)es.n, sizeof *es.v, by_line);
    qsort(ef.v, (size_t)ef.n, sizeof *ef.v, by_line);
    buf_t b = { 0 }, ch = { 0 }, of = { 0 }, os = { 0 };
    b_puts(&b, "\"sent\":[");
    for (int i = 0; i < es.n; i++) {
        const fb_cli_line_t *L = &script[es.v[i].line];
        b_puts(&b, i ? ",{" : "{");
        kv(&b, "setting", es.v[i].key, 1);
        kv(&b, "value", es.v[i].value, 0);
        kv(&b, "text", L->text, 0);
        kv(&b, "layer", L->layer, 0);
        kv(&b, "source", L->source, 0);
        b_puts(&b, "}");
        int j = eff_find(&ef, es.v[i].key);
        buf_t *t = j < 0 ? &os : !fb_cli_value_eq(es.v[i].key, es.v[i].value, ef.v[j].value) ? &ch : NULL;
        if (!t) continue;
        b_puts(t, t->len ? ",{" : "{");
        kv(t, "setting", es.v[i].key, 1);
        kv(t, "script", L->text, 0);
        if (j >= 0) kv(t, "firmware", fl[ef.v[j].line].text, 0);
        kv(t, "layer", L->layer, 0);
        kv(t, "source", L->source, 0);
        b_puts(t, "}");
    }
    b_puts(&b, "],\"current\":[");
    for (int i = 0; i < ef.n; i++) {
        b_puts(&b, i ? ",{" : "{");
        kv(&b, "setting", ef.v[i].key, 1);
        kv(&b, "value", ef.v[i].value, 0);
        kv(&b, "text", fl[ef.v[i].line].text, 0);
        b_puts(&b, "}");
        if (eff_find(&es, ef.v[i].key) < 0) {
            b_puts(&of, of.len ? ",{" : "{");
            kv(&of, "setting", ef.v[i].key, 1);
            kv(&of, "firmware", fl[ef.v[i].line].text, 0);
            b_puts(&of, "}");
        }
    }
    b_puts(&b, "],\"changed\":[");
    if (ch.len) b_add(&b, ch.p, ch.len);
    b_puts(&b, "],\"only_firmware\":[");
    if (of.len) b_add(&b, of.p, of.len);
    b_puts(&b, "],\"only_script\":[");
    if (os.len) b_add(&b, os.p, os.len);
    b_puts(&b, "]");
    free(ch.p);
    free(of.p);
    free(os.p);
    effs_free(&es);
    effs_free(&ef);
    free(fl);
    free(copy);
    return b_take(&b);
}

/* ---- the lines to type for a part of a script ---------------------------------------------- */

static int in_list(const char *const *v, int n, const char *z)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(v[i], z)) return 1;
    }
    return 0;
}

int fb_cli_changed_lines(const fb_cli_line_t *script, int n, const char *diff_text, const char *const *had,
                         int nhad, unsigned char *want)
{
    fb_cli_line_t *fl = NULL;
    int nf = 0;
    char *copy = fmt2(diff_text ? diff_text : "", "", "");
    for (char *p = copy; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        fl = xrealloc(fl, sizeof *fl * (size_t)(nf + 1));
        fl[nf++] = (fb_cli_line_t) { p, NULL, NULL };
        if (!e) break;
        p = e + 1;
    }
    effs_t es, ef;
    effective(script, n, &es);
    effective(fl, nf, &ef);
    int m = 0;
    fb_cli_ctx_t ctx = { { 0 }, { 0 } };
    for (int i = 0; i < n; i++) {
        char *k, *v;
        want[i] = 0;
        if (fb_cli_norm(&ctx, script[i].text, &k, &v) == FB_CLI_SET) {
            int a = eff_find(&es, k), b = eff_find(&ef, k);
            if (a >= 0 && b >= 0 && !fb_cli_value_eq(k, es.v[a].value, ef.v[b].value) &&
                (!had || in_list(had, nhad, script[es.v[a].line].text))) {
                want[i] = 1;
                m++;
            }
        }
        free(k);
        free(v);
    }
    effs_free(&es);
    effs_free(&ef);
    free(fl);
    free(copy);
    return m;
}

#define NCTX (int)(sizeof CONTEXTS / sizeof *CONTEXTS)

/* the context kind of a context line (profile = control_profile), -1 for any other line */
static int ctx_kind(const char *line)
{
    fb_cli_ctx_t c = { { 0 }, { 0 } };
    char *k, *v;
    fb_cli_kind_t kind = fb_cli_norm(&c, line, &k, &v);
    free(k);
    free(v);
    if (kind != FB_CLI_CONTEXT) return -1;
    for (int i = 0; i < NCTX; i++) {
        if (!strcmp(c.name, CONTEXTS[i])) return i;
    }
    return -1;
}

int fb_cli_with_context(const char *const *lines, int n, const unsigned char *want, int *out)
{
    int at[NCTX], typed[NCTX], m = 0, any = 0;
    for (int k = 0; k < NCTX; k++) at[k] = typed[k] = -1;
    for (int i = 0; i < n; i++) {
        int kind = ctx_kind(lines[i]);
        if (want[i]) {
            for (int k = 0; kind < 0 && k < NCTX; k++) {
                if (at[k] >= 0 && (typed[k] < 0 || strcmp(lines[typed[k]], lines[at[k]]))) {
                    out[m++] = at[k];
                    typed[k] = at[k];
                }
            }
            out[m++] = i;
            any = 1;
            if (kind >= 0) typed[kind] = i;
        }
        if (kind >= 0) at[kind] = i;
    }
    /* the script's own last selection of each context, as the whole script leaves it */
    for (int k = 0; any && k < NCTX; k++) {
        if (at[k] >= 0 && typed[k] >= 0 && strcmp(lines[typed[k]], lines[at[k]])) out[m++] = at[k];
    }
    return m;
}
