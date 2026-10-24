/* Bench resolver: bench JSON -> the nexus-xr machine's assembly, custom-defaults blob and core
 * manifest. See include/fb/session.h.
 *
 * A port of core/qemu/scripts/wiring.py, session.py, mkdefaults.sh and manifest.py (the reference
 * implementation): the same rules, the same order of checks, the same error and note texts (the
 * Python value reprs included: 'rs7', None, True, 5.0), the same output bytes. The comments name
 * the Python function each part follows. */
#include "fb/session.h"
#include "fb/cli_norm.h"
#include "fb/device.h"
#include "fb/json.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif
/* mingw's <stdio.h> names the archetype of the printf in effect (see src/catalog.c) */
#if defined(__MINGW_PRINTF_FORMAT)
#define FB_PRINTF_ARCHETYPE __MINGW_PRINTF_FORMAT
#else
#define FB_PRINTF_ARCHETYPE printf
#endif
#if defined(__GNUC__)
#define FB_PRINTF(a, b) __attribute__((format(FB_PRINTF_ARCHETYPE, a, b)))
#else
#define FB_PRINTF(a, b)
#endif

/* ---- the machines: the emulated boards and what each models where (wiring.py MACHINES) ------ */

#define MAX_MPADS 12                        /* pads per modelled device port */
#define MAX_SERVOS 16

typedef struct {
    const char *dev, *port;
    const char *pads[MAX_MPADS];
} modelled_t;

typedef struct {
    const char *id, *mcu, *dir;
    const char *default_config;             /* a unified-config board's default board_config; NULL: CLI */
    const modelled_t *modelled;
    int nmodelled;
    const char *const *servo_pads;          /* -M servos bit order */
    int nservo;
    const char *const *fdm;                 /* the aircraft FDM types it runs, NULL-terminated */
    int legacy;                             /* old slot form + "no wiring = the machine's defaults" */
} board_spec_t;

/* hw/arm/nexus_xr.c: uart1 is the MSP host socket, uart2 is RX-only */
static const modelled_t NEXUS_MODELLED[] = {
    { "icm42688p", "spi", { "gyro1" } },
    { "dps310", "i2c", { "i2c3" } },
    { "crsf", "uart", { "uart3", "uart4", "uart5", "uart6" } },
    { "esc", "signal", { "motor1" } },
    { "servo", "pwm", { "servo1", "servo2", "servo3", "servo4" } },
    { "msp_osd", "uart", { "uart3", "uart4", "uart5", "uart6" } },
};
static const char *const NEXUS_SERVOS[] = { "servo1", "servo2", "servo3", "servo4" };
static const char *const NEXUS_FDM[] = { "heli", NULL };
/* the STM32F405 machine for iNav's SPEEDYBEEF405WING: uart6 is the MSP host socket; the MAX7456 is
 * modelled as far as iNav's driver needs to boot (its clear-display poll hangs on an empty bus);
 * the SPL06 and a QMC5883L on I2C1, a u-blox GPS on a USART, a battery on the VBAT/CURRENT pads;
 * the SD card is not modelled */
#define OUTPUTS "output1", "output2", "output3", "output4", "output5", "output6", "output7", "output8", \
    "output9", "output10", "output11"
static const modelled_t SPEEDYBEE_MODELLED[] = {
    { "icm42688p", "spi", { "gyro1" } },
    { "spl06", "i2c", { "i2c1" } },
    { "qmc5883l", "i2c", { "i2c1" } },
    { "crsf", "uart", { "uart1", "uart2", "uart3", "uart4", "uart5" } },
    { "ublox", "uart", { "uart1", "uart2", "uart3", "uart4", "uart5" } },
    { "esc", "signal", { "output1" } },
    { "servo", "pwm", { OUTPUTS } },
    { "max7456", "spi", { "osd1" } },
    { "battery", "power", { "batt1" } },
    { "msp_osd", "uart", { "uart1", "uart2", "uart3", "uart4", "uart5" } },
};
static const char *const SPEEDYBEE_SERVOS[] = { OUTPUTS };
static const char *const SPEEDYBEE_FDM[] = { "plane", NULL };
#undef OUTPUTS

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const board_spec_t BOARDS[] = {
    { "nexus-xr", "STM32F722", "boards/nexus-xr", "boards/nexus-xr/NEXUS_XR.config",
      NEXUS_MODELLED, NELEM(NEXUS_MODELLED), NEXUS_SERVOS, NELEM(NEXUS_SERVOS), NEXUS_FDM, 1 },
    { "speedybee-f405-wing", "STM32F405", "boards/speedybee-f405-wing", NULL,
      SPEEDYBEE_MODELLED, NELEM(SPEEDYBEE_MODELLED), SPEEDYBEE_SERVOS, NELEM(SPEEDYBEE_SERVOS), SPEEDYBEE_FDM, 0 },
};
#define NBOARDS NELEM(BOARDS)
#define DEFAULT_BOARD (&BOARDS[0])
#define MAX_PAD_DEVICES 8                   /* distinct modelled devices on one pad */

static const char *const DEFAULT_ACTUATORS[5] = { "swash_a", "swash_b", "swash_c", "tail", "motor" };
static const char *const ENTRY_KEYS[] = { "device", "port", "pad", "slot", "detached", NULL };
static const char *const SLOT_GROUPS[] = { "gyro", "i2c", "uart", "motor", "servo", NULL };
static const char *const CLI_DROP[] = { "save", "exit", NULL };
static const char *const CLI_FORBID[] = { "defaults", "reboot", "dfu", "bl", "bootloader", "msc", NULL };
#define OLD_RX_PAD "uart5"
#define OLD_ESC_PAD "motor1"
#define DEFAULTS_HEADER "## Rotorflight Custom Defaults\n"
#define MAX_PINS 3

static const board_spec_t *board_spec(const char *id)
{
    for (int i = 0; id && i < NBOARDS; i++) {
        if (!strcmp(BOARDS[i].id, id)) return &BOARDS[i];
    }
    return NULL;
}

static int modelled_on(const board_spec_t *b, const char *dev, const char *port, const char *pad)
{
    for (int i = 0; i < b->nmodelled; i++) {
        if (strcmp(b->modelled[i].dev, dev) || strcmp(b->modelled[i].port, port)) continue;
        for (int k = 0; k < MAX_MPADS && b->modelled[i].pads[k]; k++) {
            if (!strcmp(b->modelled[i].pads[k], pad)) return 1;
        }
    }
    return 0;
}

static int board_models(const board_spec_t *b, const char *dev)
{
    for (int i = 0; i < b->nmodelled; i++) {
        if (!strcmp(b->modelled[i].dev, dev)) return 1;
    }
    return 0;
}

/* manifest `implemented`: some machine models it */
static int device_modelled(const char *dev)
{
    for (int i = 0; i < NBOARDS; i++) {
        if (board_models(&BOARDS[i], dev)) return 1;
    }
    return 0;
}

/* wiring.kind_fits: an `output` pad takes a motor or a servo port */
static int kind_fits(const char *pad_kind, const char *port_kind)
{
    return !strcmp(pad_kind, port_kind) ||
           (!strcmp(pad_kind, "output") && (!strcmp(port_kind, "motor") || !strcmp(port_kind, "servo")));
}

/* ---- buffers ------------------------------------------------------------------------------ */

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("fb_session: out of memory\n", stderr);
        abort();
    }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fputs("fb_session: out of memory\n", stderr);
        abort();
    }
    return p;
}

typedef struct {
    char *p;
    size_t len, cap;
} sbuf_t;

static void sb_add(sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_puts(sbuf_t *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_putc(sbuf_t *b, char c) { sb_add(b, &c, 1); }

static void FB_PRINTF(2, 0) sb_vprintf(sbuf_t *b, const char *fmt, va_list ap)
{
    va_list aq;
    va_copy(aq, ap);
    int k = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (k <= 0) return;
    if (b->len + (size_t)k + 1 > b->cap) {
        b->cap = (b->len + (size_t)k + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    vsnprintf(b->p + b->len, (size_t)k + 1, fmt, ap);
    b->len += (size_t)k;
}

static void FB_PRINTF(2, 3) sb_printf(sbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}

typedef struct {
    const char **v;
    int n, cap;
} strlist_t;

static void sl_add(strlist_t *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc((void *)l->v, sizeof *l->v * (size_t)l->cap);
    }
    l->v[l->n++] = s;
}

static int sl_has(const strlist_t *l, const char *s)
{
    for (int i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i], s)) return 1;
    }
    return 0;
}

/* ---- the session ---------------------------------------------------------------------------- */

typedef struct {
    const char *k, *v;
} kv_t;

typedef struct {
    const char *id, *kind, *group, *label;
    kv_t pins[MAX_PINS];
    int npins;
    strlist_t flags;                        /* its v is a session alloc once the board is loaded */
    const char *bus, *timer, *dma;
    int serial;                             /* -1: none */
    const fb_json_node_t *hint, *note, *part;
    int onboard, detachable;
    const char *device;
    const char *devices[MAX_PAD_DEVICES];   /* what the machine models here, sorted */
    int ndevices;
} pad_t;

/* a loaded board (wiring.load_board) */
typedef struct {
    const board_spec_t *spec;
    pad_t *pads;
    int npads, cap;
    const fb_json_node_t *name, *mcu;
    const char *machine, *msp_uart, *beeper, *defaults_cli;     /* beeper, defaults_cli: NULL = none */
    const char *firmware_family;    /* board.json firmware_family, default "rotorflight" */
    int cli;                                /* board.json "config": "cli" */
} board_t;

typedef struct {
    const char *k;
    const fb_json_node_t *v;
} prop_t;

typedef struct {
    const char *device, *port, *pad, *src;
    prop_t *props;
    int nprops;
    int onboard, modelled;
} entry_t;

typedef struct {
    kv_t *v;
    int n, cap;
} kvlist_t;

/* one line of the config script: its text, layer (board|aircraft|profile|device|bench), source
 * ("boards/x/defaults.cli:12", "device ublox on uart3"), index in its layer (1-based: the ref
 * "layer:index"), and the disabled_lines entry that skips it (NULL: sent) */
typedef struct {
    const char *text, *layer, *source, *by;
    size_t n;
    int idx;
} cline_t;

struct fb_session {
    void **allocs;
    int nallocs, capallocs;
    strlist_t errs, warns;
    char *root;
    const char *board_json;                 /* tests: instead of <root>/boards/<id>/board.json */
    const char *board_id;
    board_t board;                          /* the bench's */
    /* bench */
    const fb_json_node_t *bench;
    const char *firmware, *board_config, *aircraft_path;
    strlist_t profiles;                     /* files, in blob order */
    const char *profile_text;               /* CRs removed, "\n" added; NULL = none */
    kvlist_t options;                       /* SESSION_OPTS */
    const char *options_str;
    int shift, realtime, port_ctl, port_msp;
    /* the bench board's pads (= board.pads, board.npads) */
    pad_t *pads;
    int npads;
    /* devices: the catalog read back from its JSON, as manifest.py/wiring.py read it */
    const fb_json_node_t *catalog;
    /* aircraft */
    const fb_json_node_t *ac;
    const char *ac_id;
    /* assembly */
    int have_wiring;
    entry_t *entries;
    int nentries;
    kvlist_t mopts;                         /* machine_options(), dict order */
    strlist_t devcfg;
    /* outputs */
    int have_blob;
    sbuf_t blob, manifest, report;
    sbuf_t config_cli;                      /* a CLI board's script; len 0 = none */
    char *config_json;                      /* fb_session_config_json, made on first use */
    /* the script's lines with their layer and source (a blob board: the device lines) */
    const char *bench_cli;                  /* the bench's own lines (layer "bench"): a file; "" = none */
    const fb_json_node_t *disabled;         /* disabled_lines; NULL = none */
    cline_t *cl;
    int ncl, capcl;
    fb_cli_conflict_t *conf;
    int nconf;
    strlist_t layer_files;                  /* "layer\tpath" of the files read, in order */
};

static void *s_alloc(fb_session_t *s, size_t n)
{
    void *p = xmalloc(n);
    if (s->nallocs == s->capallocs) {
        s->capallocs = s->capallocs ? s->capallocs * 2 : 64;
        s->allocs = xrealloc(s->allocs, sizeof *s->allocs * (size_t)s->capallocs);
    }
    s->allocs[s->nallocs++] = p;
    return p;
}

static char *s_dup(fb_session_t *s, const char *z, size_t n)
{
    char *p = s_alloc(s, n + 1);
    memcpy(p, z, n);
    p[n] = 0;
    return p;
}

static char *s_strdup(fb_session_t *s, const char *z) { return s_dup(s, z, strlen(z)); }

/* an sbuf's text as a session string; the sbuf is reset */
static char *s_take(fb_session_t *s, sbuf_t *b)
{
    char *p = s_dup(s, b->p ? b->p : "", b->len);
    free(b->p);
    *b = (sbuf_t) { 0 };
    return p;
}

static char *FB_PRINTF(2, 3) s_fmt(fb_session_t *s, const char *fmt, ...)
{
    sbuf_t b = { 0 };
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    return s_take(s, &b);
}

static void FB_PRINTF(2, 3) err(fb_session_t *s, const char *fmt, ...)
{
    sbuf_t b = { 0 };
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    sl_add(&s->errs, s_take(s, &b));
}

static void FB_PRINTF(2, 3) note(fb_session_t *s, const char *fmt, ...)
{
    sbuf_t b = { 0 };
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    sl_add(&s->warns, s_take(s, &b));
}

static void kv_set(fb_session_t *s, kvlist_t *l, const char *k, const char *v)
{
    for (int i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i].k, k)) {
            l->v[i].v = v;
            return;
        }
    }
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        kv_t *nv = s_alloc(s, sizeof *nv * (size_t)l->cap);
        if (l->n) memcpy(nv, l->v, sizeof *nv * (size_t)l->n);
        l->v = nv;
    }
    l->v[l->n++] = (kv_t) { k, v };
}

static const char *kv_get(const kvlist_t *l, const char *k)
{
    for (int i = 0; i < l->n; i++) {
        if (!strcmp(l->v[i].k, k)) return l->v[i].v;
    }
    return NULL;
}

/* ---- host: files, environment (UTF-8 everywhere; Windows through UTF-16 names) ------------- */

#ifdef _WIN32
static wchar_t *wide(const char *u)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    wchar_t *w = xmalloc(sizeof *w * (size_t)(n > 0 ? n : 1));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n)) w[0] = 0;
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = xmalloc((size_t)(n > 0 ? n : 1));
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL)) u[0] = 0;
    return u;
}

static FILE *u8open(const char *path)
{
    wchar_t *w = wide(path);
    FILE *f = _wfopen(w, L"rb");
    free(w);
    return f;
}

static char *u8getenv(fb_session_t *s, const char *name)
{
    wchar_t *w = wide(name);
    const wchar_t *v = _wgetenv(w);
    free(w);
    if (!v || !*v) return NULL;
    char *u = narrow(v), *r = s_strdup(s, u);
    free(u);
    return r;
}

static char *u8getcwd(fb_session_t *s)
{
    wchar_t *w = _wgetcwd(NULL, 0);
    if (!w) return s_strdup(s, ".");
    char *u = narrow(w), *r = s_strdup(s, u);
    free(u);
    free(w);
    for (char *p = r; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return r;
}
#else
static FILE *u8open(const char *path) { return fopen(path, "rb"); }

static char *u8getenv(fb_session_t *s, const char *name)
{
    const char *v = getenv(name);
    return v && *v ? s_strdup(s, v) : NULL;
}

static char *u8getcwd(fb_session_t *s)
{
    size_t n = 256;
    for (;;) {
        char *b = xmalloc(n);
        if (getcwd(b, n)) {
            char *r = s_strdup(s, b);
            free(b);
            return r;
        }
        free(b);
        if (errno != ERANGE || n > (1u << 20)) return s_strdup(s, ".");
        n *= 2;
    }
}
#endif

static int exists(const char *path)
{
    FILE *f = u8open(path);
    if (f) fclose(f);
    return f != NULL;
}

/* a whole file, NUL-terminated; NULL with the reason in why */
static char *read_file(fb_session_t *s, const char *path, size_t *len, const char **why)
{
    FILE *f = u8open(path);
    if (!f) {
        *why = s_strdup(s, strerror(errno));
        return NULL;
    }
    sbuf_t b = { 0 };
    char chunk[8192];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) sb_add(&b, chunk, k);
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        free(b.p);
        *why = s_strdup(s, "read error");
        return NULL;
    }
    *len = b.len;
    return s_take(s, &b);
}

/* the names in a directory, sorted, without the ones starting with '.' (glob's "*") */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void list_dir(fb_session_t *s, const char *dir, strlist_t *out)
{
#ifdef _WIN32
    char *pat = s_fmt(s, "%s/*", dir);
    wchar_t *w = wide(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *u = narrow(fd.cFileName);
            if (u[0] != '.') sl_add(out, s_strdup(s, u));
            free(u);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] != '.') sl_add(out, s_strdup(s, de->d_name));
        }
        closedir(d);
    }
#endif
    if (out->n > 1) qsort((void *)out->v, (size_t)out->n, sizeof *out->v, cmp_str);
}

/* ---- paths (os.path, posix; Windows: drive letters and // too) ------------------------------ */

static int is_abs(const char *p)
{
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
        (p[2] == '/' || p[2] == '\\')) {
        return 1;
    }
    return p[0] == '/' || p[0] == '\\';
#else
    return p[0] == '/';
#endif
}

static char *join(fb_session_t *s, const char *a, const char *b)
{
    size_t la = strlen(a);
    if (is_abs(b) || !la) return s_strdup(s, b);
    return s_fmt(s, "%s%s%s", a, a[la - 1] == '/' ? "" : "/", b);
}

static char *dirname_of(fb_session_t *s, const char *p)
{
    const char *sl = strrchr(p, '/');
    if (!sl) return s_strdup(s, "");
    size_t n = (size_t)(sl - p);
    while (n > 1 && p[n - 1] == '/') n--;          /* os.path.dirname keeps a lone "/" */
    return s_dup(s, p, n ? n : 1);
}

static const char *basename_of(const char *p)
{
    const char *sl = strrchr(p, '/');
    return sl ? sl + 1 : p;
}

static char *home_dir(fb_session_t *s)
{
#ifdef _WIN32
    char *h = u8getenv(s, "USERPROFILE");
    if (!h) h = u8getenv(s, "HOME");
    if (h) {
        for (char *p = h; *p; p++) {
            if (*p == '\\') *p = '/';
        }
    }
    return h;
#else
    return u8getenv(s, "HOME");
#endif
}

/* os.path.expanduser for "~" and "~/..." ("~user" is left as it is) */
static char *expand_user(fb_session_t *s, const char *p)
{
    if (p[0] != '~' || (p[1] && p[1] != '/')) return s_strdup(s, p);
    char *h = home_dir(s);
    if (!h) return s_strdup(s, p);
    size_t n = strlen(h);
    while (n > 0 && h[n - 1] == '/') n--;
    if (!n && !p[1]) return s_strdup(s, "/");
    return s_fmt(s, "%.*s%s", (int)n, h, p + 1);
}

/* os.path.normpath of an absolute path (the data root) */
static char *norm_path(fb_session_t *s, const char *p)
{
    size_t pre = 0;
#ifdef _WIN32
    if (p[0] && p[1] == ':') pre = 2;
#endif
    int abs = p[pre] == '/';
    const char **parts = s_alloc(s, sizeof *parts * (strlen(p) + 1));
    size_t *lens = s_alloc(s, sizeof *lens * (strlen(p) + 1));
    int n = 0;
    for (const char *q = p + pre; *q;) {
        while (*q == '/') q++;
        const char *e = q;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - q);
        if (l == 0 || (l == 1 && q[0] == '.')) {
            /* skip */
        } else if (l == 2 && q[0] == '.' && q[1] == '.') {
            if (n > 0 && !(lens[n - 1] == 2 && !memcmp(parts[n - 1], "..", 2))) {
                n--;
            } else if (!abs) {
                parts[n] = q;
                lens[n++] = l;
            }
        } else {
            parts[n] = q;
            lens[n++] = l;
        }
        q = e;
    }
    sbuf_t b = { 0 };
    sb_add(&b, p, pre);
    if (abs) sb_putc(&b, '/');
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(&b, '/');
        sb_add(&b, parts[i], lens[i]);
    }
    if (!b.len) sb_putc(&b, '.');
    return s_take(s, &b);
}

static char *abs_path(fb_session_t *s, const char *p)
{
    char *q = s_strdup(s, p);
#ifdef _WIN32
    for (char *c = q; *c; c++) {
        if (*c == '\\') *c = '/';
    }
#endif
    return norm_path(s, is_abs(q) ? q : join(s, u8getcwd(s), q));
}

/* session.py path(): "" stays "", backslashes -> "/", "~" expanded, relative -> the data root */
static char *bench_path(fb_session_t *s, const char *p)
{
    if (!p || !*p) return s_strdup(s, "");
    char *q = s_strdup(s, p);
    for (char *c = q; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    q = expand_user(s, q);
    return is_abs(q) ? q : join(s, s->root, q);
}

/* ---- Python formatting: repr(), str(), json.dumps() ------------------------------------------ */

/* float.__repr__: the shortest digits that read back, fixed notation for 1e-4 <= |v| < 1e16
 * (with ".0" when integral), else d.ddde+XX. json: inf/nan as json.dumps writes them. */
static void py_float(sbuf_t *b, double v, int json)
{
    if (isnan(v)) {
        sb_puts(b, json ? "NaN" : "nan");
        return;
    }
    if (isinf(v)) {
        sb_puts(b, v < 0 ? (json ? "-Infinity" : "-inf") : (json ? "Infinity" : "inf"));
        return;
    }
    char t[40];
    for (int p = 1; p <= 17; p++) {
        snprintf(t, sizeof t, "%.*e", p - 1, v);
        if (strtod(t, NULL) == v) break;
    }
    /* t = [-]d[.ddd]e[+-]xx */
    const char *q = t;
    int neg = 0;
    if (*q == '-') {
        neg = 1;
        q++;
    }
    char dig[24];
    int nd = 0;
    for (; *q && *q != 'e'; q++) {
        if (*q != '.') dig[nd++] = *q;
    }
    int ex = atoi(q + 1);
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    if (neg) sb_putc(b, '-');
    if (ex >= -4 && ex < 16) {
        if (ex < 0) {
            sb_puts(b, "0.");
            for (int i = 0; i < -ex - 1; i++) sb_putc(b, '0');
            sb_add(b, dig, (size_t)nd);
        } else {
            for (int i = 0; i <= ex; i++) sb_putc(b, i < nd ? dig[i] : '0');
            sb_putc(b, '.');
            if (nd > ex + 1) sb_add(b, dig + ex + 1, (size_t)(nd - ex - 1));
            else sb_putc(b, '0');
        }
    } else {
        sb_putc(b, dig[0]);
        if (nd > 1) {
            sb_putc(b, '.');
            sb_add(b, dig + 1, (size_t)(nd - 1));
        }
        sb_printf(b, "e%c%02d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
    }
}

/* an int token as Python's int prints it ("-0" -> "0") */
static void py_int(sbuf_t *b, const fb_json_node_t *v)
{
    if (v->len == 2 && v->text[0] == '-' && v->text[1] == '0') sb_putc(b, '0');
    else sb_add(b, v->text, v->len);
}

static void py_number(sbuf_t *b, const fb_json_node_t *v, int json)
{
    if (v->is_int) py_int(b, v);
    else py_float(b, fb_json_number(v), json);
}

/* one UTF-8 sequence at s -> code point, bytes used */
static unsigned long utf8_cp(const unsigned char *s, size_t n, int *used)
{
    unsigned c = s[0];
    int k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if ((size_t)k > n) k = 1;
    unsigned long cp = k == 1 ? c : c & (0x3F >> (k - 1));
    for (int i = 1; i < k; i++) cp = cp << 6 | (s[i] & 0x3F);
    *used = k;
    return cp;
}

static void py_repr_str(sbuf_t *b, const char *z, size_t n)
{
    char q = memchr(z, '\'', n) && !memchr(z, '"', n) ? '"' : '\'';
    sb_putc(b, q);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)z[i];
        if (c == (unsigned char)q || c == '\\') {
            sb_putc(b, '\\');
            sb_putc(b, (char)c);
        } else if (c == '\n') {
            sb_puts(b, "\\n");
        } else if (c == '\r') {
            sb_puts(b, "\\r");
        } else if (c == '\t') {
            sb_puts(b, "\\t");
        } else if (c < 0x20 || c == 0x7F) {
            sb_printf(b, "\\x%02x", c);
        } else {
            sb_putc(b, (char)c);
        }
    }
    sb_putc(b, q);
}

static void py_repr(fb_session_t *s, sbuf_t *b, const fb_json_node_t *v);

static void json_str_decoded(fb_session_t *s, const fb_json_node_t *v, const char **z, size_t *n)
{
    size_t k = fb_json_str(v, NULL, 0);
    char *p = s_alloc(s, k + 1);
    fb_json_str(v, p, k + 1);
    *z = p;
    *n = k;
}

static void py_repr(fb_session_t *s, sbuf_t *b, const fb_json_node_t *v)
{
    if (!v) {
        sb_puts(b, "None");
        return;
    }
    switch (v->type) {
    case FB_JSON_NULL: sb_puts(b, "None"); break;
    case FB_JSON_TRUE: sb_puts(b, "True"); break;
    case FB_JSON_FALSE: sb_puts(b, "False"); break;
    case FB_JSON_NUMBER: py_number(b, v, 0); break;
    case FB_JSON_STRING: {
        const char *z;
        size_t n;
        json_str_decoded(s, v, &z, &n);
        py_repr_str(b, z, n);
        break;
    }
    case FB_JSON_ARRAY: {
        sb_putc(b, '[');
        const fb_json_node_t *e = v + 1;
        for (int i = 0; i < v->count; i++, e += e->skip) {
            if (i) sb_puts(b, ", ");
            py_repr(s, b, e);
        }
        sb_putc(b, ']');
        break;
    }
    case FB_JSON_OBJECT: {
        sb_putc(b, '{');
        const fb_json_node_t *k = v + 1;
        for (int i = 0; i < v->count; i++, k = (k + 1) + k[1].skip) {
            if (i) sb_puts(b, ", ");
            py_repr(s, b, k);
            sb_puts(b, ": ");
            py_repr(s, b, k + 1);
        }
        sb_putc(b, '}');
        break;
    }
    }
}

/* repr(v) / str(v) as session strings */
static const char *R(fb_session_t *s, const fb_json_node_t *v)
{
    sbuf_t b = { 0 };
    py_repr(s, &b, v);
    return s_take(s, &b);
}

static const char *Rs(fb_session_t *s, const char *z)
{
    sbuf_t b = { 0 };
    py_repr_str(&b, z, strlen(z));
    return s_take(s, &b);
}

static const char *S(fb_session_t *s, const fb_json_node_t *v)
{
    if (v && v->type == FB_JSON_STRING) {
        const char *z;
        size_t n;
        json_str_decoded(s, v, &z, &n);
        return z;
    }
    return R(s, v);
}

/* json.dumps string: ascii = ensure_ascii (the default) */
static void py_json_str(sbuf_t *b, const char *z, size_t n, int ascii)
{
    sb_putc(b, '"');
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)z[i];
        if (c == '"') sb_puts(b, "\\\"");
        else if (c == '\\') sb_puts(b, "\\\\");
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c == '\b') sb_puts(b, "\\b");
        else if (c == '\f') sb_puts(b, "\\f");
        else if (c < 0x20 || (ascii && c == 0x7F)) sb_printf(b, "\\u%04x", c);
        else if (ascii && c >= 0x80) {
            int used;
            unsigned long cp = utf8_cp((const unsigned char *)z + i, n - i, &used);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                sb_printf(b, "\\u%04lx\\u%04lx", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
            } else {
                sb_printf(b, "\\u%04lx", cp);
            }
            i += (size_t)used;
            continue;
        } else {
            sb_putc(b, (char)c);
        }
        i++;
    }
    sb_putc(b, '"');
}

/* json.dumps(v): compact = separators (",", ":") and ensure_ascii=False (manifest.py), else the
 * defaults (", ", ": ", ensure_ascii) (session.py --check props) */
static void py_dump(fb_session_t *s, sbuf_t *b, const fb_json_node_t *v, int compact)
{
    switch (v->type) {
    case FB_JSON_NULL: sb_puts(b, "null"); break;
    case FB_JSON_TRUE: sb_puts(b, "true"); break;
    case FB_JSON_FALSE: sb_puts(b, "false"); break;
    case FB_JSON_NUMBER: py_number(b, v, 1); break;
    case FB_JSON_STRING: {
        const char *z;
        size_t n;
        json_str_decoded(s, v, &z, &n);
        py_json_str(b, z, n, !compact);
        break;
    }
    case FB_JSON_ARRAY: {
        sb_putc(b, '[');
        const fb_json_node_t *e = v + 1;
        for (int i = 0; i < v->count; i++, e += e->skip) {
            if (i) sb_puts(b, compact ? "," : ", ");
            py_dump(s, b, e, compact);
        }
        sb_putc(b, ']');
        break;
    }
    case FB_JSON_OBJECT: {
        sb_putc(b, '{');
        const fb_json_node_t *k = v + 1;
        for (int i = 0; i < v->count; i++, k = (k + 1) + k[1].skip) {
            if (i) sb_puts(b, compact ? "," : ", ");
            py_dump(s, b, k, compact);
            sb_puts(b, compact ? ":" : ": ");
            py_dump(s, b, k + 1, compact);
        }
        sb_putc(b, '}');
        break;
    }
    }
}

static void dump_zstr(sbuf_t *b, const char *z) { py_json_str(b, z, strlen(z), 0); }

/* ---- JSON documents ------------------------------------------------------------------------ */

static const fb_json_node_t *parse_doc(fb_session_t *s, const char *text, size_t n, char *why,
                                       size_t whylen)
{
    int need = fb_json_parse(text, n, NULL, 0, why, whylen);
    if (need < 0) return NULL;
    fb_json_node_t *nodes = s_alloc(s, sizeof *nodes * (size_t)need);
    fb_json_parse(text, n, nodes, need, why, whylen);
    return nodes;
}

static const char *jstr(fb_session_t *s, const fb_json_node_t *v)
{
    if (!v || v->type != FB_JSON_STRING) return NULL;
    const char *z;
    size_t n;
    json_str_decoded(s, v, &z, &n);
    return z;
}

static int is_int(const fb_json_node_t *v) { return v && v->type == FB_JSON_NUMBER && v->is_int; }
static int is_bool(const fb_json_node_t *v)
{
    return v && (v->type == FB_JSON_TRUE || v->type == FB_JSON_FALSE);
}
/* wiring._num: int or float, not bool, finite */
static int is_num(const fb_json_node_t *v)
{
    return v && v->type == FB_JSON_NUMBER && isfinite(fb_json_number(v));
}
static double num(const fb_json_node_t *v) { return fb_json_number(v); }

/* Python ==, for the JSON values the resolver compares */
static int py_eq(fb_session_t *s, const fb_json_node_t *a, const fb_json_node_t *b)
{
    if (!a || !b) return a == b;
    int an = a->type == FB_JSON_NUMBER || is_bool(a), bn = b->type == FB_JSON_NUMBER || is_bool(b);
    if (an && bn) {
        double x = a->type == FB_JSON_NUMBER ? num(a) : a->type == FB_JSON_TRUE;
        double y = b->type == FB_JSON_NUMBER ? num(b) : b->type == FB_JSON_TRUE;
        return x == y;
    }
    if (a->type != b->type) return 0;
    sbuf_t x = { 0 }, y = { 0 };
    py_dump(s, &x, a, 1);
    py_dump(s, &y, b, 1);
    int eq = x.len == y.len && !memcmp(x.p, y.p, x.len);
    free(x.p);
    free(y.p);
    return eq;
}

/* the object's members as (key, value); iteration helper */
#define FOR_MEMBERS(obj, k) \
    for (const fb_json_node_t *k = (obj) + 1; k < (obj) + (obj)->skip; k = (k + 1) + k[1].skip)
#define FOR_ITEMS(arr, e) \
    for (const fb_json_node_t *e = (arr) + 1; (e - (arr)) < (arr)->skip; e += e->skip)

static int in_strs(const char *const *list, const char *z)
{
    for (int i = 0; list[i]; i++) {
        if (!strcmp(list[i], z)) return 1;
    }
    return 0;
}

static int actuator_name(const char *z)
{
    if (!z || !*z) return 0;
    for (; *z; z++) {
        char c = *z;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

/* ---- board (wiring.py _parse_config, config_pads, load_board) ------------------------------- */

typedef struct {
    const char *name, *value;
    long n;
} res_t;

typedef struct {
    res_t *res;
    int nres;
    kvlist_t timers, dmas, sets;
} cfg_t;

static int is_w(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
static int is_d(char c) { return c >= '0' && c <= '9'; }
static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

/* matchers over [p, e): advance p, 0 when it does not match */
static int m_lit(const char **p, const char *e, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(e - *p) < n || memcmp(*p, lit, n)) return 0;
    *p += n;
    return 1;
}

static int m_run(const char **p, const char *e, int (*cls)(char), const char **s, size_t *n)
{
    const char *q = *p;
    while (q < e && cls(*q)) q++;
    if (q == *p) return 0;
    if (s) *s = *p;
    if (n) *n = (size_t)(q - *p);
    *p = q;
    return 1;
}

static int not_sp(char c) { return !is_sp(c); }

static char *upper(fb_session_t *s, const char *z, size_t n)
{
    char *u = s_dup(s, z, n);
    for (char *c = u; *c; c++) {
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
    }
    return u;
}

static char *lower(fb_session_t *s, const char *z, size_t n)
{
    char *u = s_dup(s, z, n);
    for (char *c = u; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    }
    return u;
}

typedef struct {
    const char *p;
    size_t n;
} line_t;

/* str.splitlines(): \n, \r\n, \r */
static line_t *split_lines(fb_session_t *s, const char *t, size_t n, int *count)
{
    int cap = 1;
    for (size_t i = 0; i < n; i++) cap += t[i] == '\n' || t[i] == '\r';
    line_t *l = s_alloc(s, sizeof *l * (size_t)cap);
    int k = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && t[j] != '\n' && t[j] != '\r') j++;
        l[k++] = (line_t) { t + i, j - i };
        if (j < n && t[j] == '\r' && j + 1 < n && t[j + 1] == '\n') j++;
        i = j + 1;
    }
    *count = k;
    return l;
}

static void parse_config(fb_session_t *s, const char *cfg, size_t n, cfg_t *c)
{
    int nl;
    line_t *lines = split_lines(s, cfg, n, &nl);
    c->res = s_alloc(s, sizeof *c->res * (size_t)(nl + 1));
    for (int i = 0; i < nl; i++) {
        const char *p = lines[i].p, *e = p + lines[i].n, *a, *b2, *d;
        size_t an, bn, dn;
        line_t nx = i + 1 < nl ? lines[i + 1] : (line_t) { "", 0 };
        const char *q = p;
        /* ^resource (\w+) (\d+) (\w+)\s*$ */
        if (m_lit(&q, e, "resource ") && m_run(&q, e, is_w, &a, &an) && m_lit(&q, e, " ") &&
            m_run(&q, e, is_d, &d, &dn) && m_lit(&q, e, " ") && m_run(&q, e, is_w, &b2, &bn)) {
            while (q < e && is_sp(*q)) q++;
            if (q == e) {
                char *v = upper(s, b2, bn);
                if (strcmp(v, "NONE")) {
                    c->res[c->nres++] = (res_t) { upper(s, a, an), v, strtol(s_dup(s, d, dn), NULL, 10) };
                }
            }
        }
        /* ^timer (\w+) AF\d+ ; next ^# pin \w+: (TIM\d+) CH(\d+) */
        q = p;
        if (m_lit(&q, e, "timer ") && m_run(&q, e, is_w, &a, &an) && m_lit(&q, e, " AF") &&
            m_run(&q, e, is_d, NULL, NULL)) {
            const char *r = nx.p, *re = nx.p + nx.n, *t0, *ch;
            size_t tn, chn;
            if (m_lit(&r, re, "# pin ") && m_run(&r, re, is_w, NULL, NULL) && m_lit(&r, re, ": ") &&
                (t0 = r, m_lit(&r, re, "TIM")) && m_run(&r, re, is_d, NULL, NULL) &&
                (tn = (size_t)(r - t0), m_lit(&r, re, " CH")) && m_run(&r, re, is_d, &ch, &chn)) {
                kv_set(s, &c->timers, upper(s, a, an), s_fmt(s, "%.*s_CH%.*s", (int)tn, t0, (int)chn, ch));
            }
        }
        /* ^dma pin (\w+) \d+ ; next ^# pin \w+: (DMA\d) Stream (\d+) Channel (\d+) */
        q = p;
        if (m_lit(&q, e, "dma pin ") && m_run(&q, e, is_w, &a, &an) && m_lit(&q, e, " ") &&
            m_run(&q, e, is_d, NULL, NULL)) {
            const char *r = nx.p, *re = nx.p + nx.n, *st, *chn_p;
            size_t stn, chn;
            if (m_lit(&r, re, "# pin ") && m_run(&r, re, is_w, NULL, NULL) && m_lit(&r, re, ": ") &&
                m_lit(&r, re, "DMA") && r < re && is_d(*r)) {
                char dn1 = *r++;
                if (m_lit(&r, re, " Stream ") && m_run(&r, re, is_d, &st, &stn) &&
                    m_lit(&r, re, " Channel ") && m_run(&r, re, is_d, &chn_p, &chn)) {
                    kv_set(s, &c->dmas, upper(s, a, an),
                           s_fmt(s, "DMA%c_S%.*s_CH%.*s", dn1, (int)stn, st, (int)chn, chn_p));
                }
            }
        }
        /* ^set (\w+) = (\S+) (re.M over the text: the same per line) */
        q = p;
        if (m_lit(&q, e, "set ") && m_run(&q, e, is_w, &a, &an) && m_lit(&q, e, " = ") &&
            m_run(&q, e, not_sp, &b2, &bn)) {
            kv_set(s, &c->sets, lower(s, a, an), s_dup(s, b2, bn));
        }
    }
}

static const char *res_get(const cfg_t *c, const char *name, long n)
{
    for (int i = c->nres - 1; i >= 0; i--) {
        if (c->res[i].n == n && !strcmp(c->res[i].name, name)) return c->res[i].value;
    }
    return NULL;
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

/* sorted unique numbers of the resources with one of the names */
static int res_nums(fb_session_t *s, const cfg_t *c, const char *n1, const char *n2, long **out)
{
    long *v = s_alloc(s, sizeof *v * (size_t)(c->nres + 1));
    int k = 0;
    for (int i = 0; i < c->nres; i++) {
        if (strcmp(c->res[i].name, n1) && (!n2 || strcmp(c->res[i].name, n2))) continue;
        int dup = 0;
        for (int j = 0; j < k; j++) dup |= v[j] == c->res[i].n;
        if (!dup) v[k++] = c->res[i].n;
    }
    qsort(v, (size_t)k, sizeof *v, cmp_long);
    *out = v;
    return k;
}

static pad_t *new_pad(fb_session_t *s, int *cap)
{
    if (s->npads == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        pad_t *np = s_alloc(s, sizeof *np * (size_t)*cap);
        if (s->npads) memcpy(np, s->pads, sizeof *np * (size_t)s->npads);
        s->pads = np;
    }
    pad_t *p = &s->pads[s->npads++];
    *p = (pad_t) { .serial = -1 };
    return p;
}

static void pin(pad_t *p, const char *k, const char *v)
{
    p->pins[p->npins++] = (kv_t) { k, v };
}

static const char *pin_get(const pad_t *p, const char *k)
{
    for (int i = 0; i < p->npins; i++) {
        if (!strcmp(p->pins[i].k, k)) return p->pins[i].v;
    }
    return NULL;
}

static void timed(const cfg_t *c, pad_t *p, const char *pinv)
{
    const char *t = kv_get(&c->timers, pinv), *d = kv_get(&c->dmas, pinv);
    if (t) p->timer = t;
    if (d) p->dma = d;
}

static void config_pads(fb_session_t *s, const cfg_t *c, int *cap)
{
    long *ns;
    int k = res_nums(s, c, "GYRO_CS", NULL, &ns);
    for (int i = 0; i < k; i++) {
        pad_t *p = new_pad(s, cap);
        p->id = s_fmt(s, "gyro%ld", ns[i]);
        p->kind = "spi";
        p->group = "gyro";
        pin(p, "cs", res_get(c, "GYRO_CS", ns[i]));
        if (res_get(c, "GYRO_EXTI", ns[i])) pin(p, "int", res_get(c, "GYRO_EXTI", ns[i]));
        const char *bus = kv_get(&c->sets, s_fmt(s, "gyro_%ld_spibus", ns[i]));
        if (bus) p->bus = s_fmt(s, "spi%s", bus);
    }
    k = res_nums(s, c, "I2C_SCL", NULL, &ns);
    for (int i = 0; i < k; i++) {
        pad_t *p = new_pad(s, cap);
        p->id = s_fmt(s, "i2c%ld", ns[i]);
        p->kind = "i2c";
        p->group = "i2c";
        pin(p, "scl", res_get(c, "I2C_SCL", ns[i]));
        if (res_get(c, "I2C_SDA", ns[i])) pin(p, "sda", res_get(c, "I2C_SDA", ns[i]));
        p->bus = s_fmt(s, "i2c%ld", ns[i]);
    }
    k = res_nums(s, c, "SERIAL_TX", "SERIAL_RX", &ns);
    for (int i = 0; i < k; i++) {
        pad_t *p = new_pad(s, cap);
        p->id = s_fmt(s, "uart%ld", ns[i]);
        p->kind = "uart";
        p->group = "uart";
        if (res_get(c, "SERIAL_TX", ns[i])) pin(p, "tx", res_get(c, "SERIAL_TX", ns[i]));
        if (res_get(c, "SERIAL_RX", ns[i])) pin(p, "rx", res_get(c, "SERIAL_RX", ns[i]));
        if (!pin_get(p, "tx")) sl_add(&p->flags, "rx_only");
        if (!pin_get(p, "rx")) sl_add(&p->flags, "tx_only");
        p->serial = (int)ns[i] - 1;          /* the CLI's `serial <n>` index */
    }
    static const struct { const char *res, *pfx, *kind, *group, *pinname; } simple[] = {
        { "MOTOR", "motor", "motor", "motor", "signal" },
        { "SERVO", "servo", "servo", "servo", "signal" },
        { "FREQ", "freq", "capture", NULL, "in" },
    };
    for (int t = 0; t < 3; t++) {
        k = res_nums(s, c, simple[t].res, NULL, &ns);
        for (int i = 0; i < k; i++) {
            pad_t *p = new_pad(s, cap);
            const char *pv = res_get(c, simple[t].res, ns[i]);
            p->id = s_fmt(s, "%s%ld", simple[t].pfx, ns[i]);
            p->kind = simple[t].kind;
            p->group = simple[t].group;
            pin(p, simple[t].pinname, pv);
            timed(c, p, pv);
        }
    }
    k = res_nums(s, c, "FLASH_CS", NULL, &ns);
    for (int i = 0; i < k; i++) {
        pad_t *p = new_pad(s, cap);
        p->id = s_fmt(s, "flash%ld", ns[i]);
        p->kind = "spi";
        pin(p, "cs", res_get(c, "FLASH_CS", ns[i]));
        const char *bus = kv_get(&c->sets, "flash_spi_bus");
        if (bus) p->bus = s_fmt(s, "spi%s", bus);
    }
}

static pad_t *pad_by_id(fb_session_t *s, const char *id)
{
    for (int i = 0; i < s->npads; i++) {
        if (!strcmp(s->pads[i].id, id)) return &s->pads[i];
    }
    return NULL;
}

static int casefold_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* wiring._pad_data: board.json pad data keys onto the pad; -1 (error added) on a bad value */
static int pad_data(fb_session_t *s, pad_t *p, const fb_json_node_t *o)
{
    static const char *const keys[] = { "group", "pins", "bus", "timer", "dma", "serial", NULL };
    for (int i = 0; keys[i]; i++) {
        const fb_json_node_t *v = fb_json_get(o, keys[i]);
        if (!v) continue;
        int ok;
        if (!strcmp(keys[i], "pins")) {
            ok = v->type == FB_JSON_OBJECT && v->count <= MAX_PINS;
            if (ok) FOR_MEMBERS(v, k) ok &= k[1].type == FB_JSON_STRING;
        } else if (!strcmp(keys[i], "serial")) {
            ok = is_int(v) && num(v) >= 0 && num(v) <= 255;
        } else {
            ok = v->type == FB_JSON_STRING;
        }
        if (!ok) {
            err(s, "board.json pad %s: bad %s %s", p->id, keys[i], R(s, v));
            return -1;
        }
        if (!strcmp(keys[i], "pins")) {
            p->npins = 0;
            FOR_MEMBERS(v, k) pin(p, S(s, k), S(s, k + 1));
        } else if (!strcmp(keys[i], "serial")) {
            p->serial = (int)num(v);
        } else {
            const char *z = S(s, v);
            if (i == 0) p->group = z;
            else if (!strcmp(keys[i], "bus")) p->bus = z;
            else if (!strcmp(keys[i], "timer")) p->timer = z;
            else p->dma = z;
        }
    }
    return 0;
}

/* [A-Z][0-9]{2}(:inv)? */
static int beeper_pin(const char *z)
{
    if (!(z[0] >= 'A' && z[0] <= 'Z') || !is_d(z[1]) || !is_d(z[2])) return 0;
    return !z[3] || !strcmp(z + 3, ":inv");
}

/* the pad lists' flags -> session allocations (fb_session_free frees nothing per pad) */
static void own_flags(fb_session_t *s)
{
    for (int i = 0; i < s->npads; i++) {
        strlist_t *f = &s->pads[i].flags;
        if (!f->v) continue;
        const char **v = s_alloc(s, sizeof *v * (size_t)(f->n ? f->n : 1));
        memcpy((void *)v, (const void *)f->v, sizeof *v * (size_t)f->n);
        free((void *)f->v);
        f->v = v;
        f->cap = f->n;
    }
}

static int load_board_(fb_session_t *s, board_t *b, const char *config_path, const char *board_json, int warn)
{
    const board_spec_t *spec = b->spec;
    const char *d = join(s, s->root, spec->dir);
    const char *bj_path = board_json && *board_json ? board_json : join(s, d, "board.json");
    size_t n;
    const char *why;
    char *text = read_file(s, bj_path, &n, &why);
    if (!text) {
        err(s, "board.json %s: %s", bj_path, why);
        return -1;
    }
    char jerr[128];
    const fb_json_node_t *bj = parse_doc(s, text, n, jerr, sizeof jerr);
    if (!bj || bj->type != FB_JSON_OBJECT) {
        err(s, "board.json %s: %s", bj_path, bj ? "not a JSON object" : jerr);
        return -1;
    }
    b->cli = fb_json_str_eq(fb_json_get(bj, "config"), "cli");
    const char *bj_cfg = jstr(s, fb_json_get(bj, "config"));
    const char *cfg_path = b->cli ? "" : config_path && *config_path ? config_path : join(s, d, bj_cfg ? bj_cfg : "");
    size_t cn = 0;
    const char *cfg = "";
    if (cfg_path && *cfg_path && exists(cfg_path)) {
        cfg = read_file(s, cfg_path, &cn, &why);
        if (!cfg) {
            cfg = "";
            cn = 0;
        }
    }
    cfg_t c = { 0 };
    parse_config(s, cfg, cn, &c);
    int cap = 0;
    config_pads(s, &c, &cap);
    /* re.search(r"^(timer|dma pin) \w+", cfg, re.M) and not re.search(r"^# pin \w+:", ...) */
    {
        int nl, timed_lines = 0, pin_comments = 0;
        line_t *lines = split_lines(s, cfg, cn, &nl);
        for (int i = 0; i < nl; i++) {
            const char *q = lines[i].p, *e = q + lines[i].n;
            if ((m_lit(&q, e, "timer ") || (q = lines[i].p, m_lit(&q, e, "dma pin "))) &&
                m_run(&q, e, is_w, NULL, NULL)) {
                timed_lines = 1;
            }
            q = lines[i].p;
            if (m_lit(&q, e, "# pin ") && m_run(&q, e, is_w, NULL, NULL) && m_lit(&q, e, ":")) {
                pin_comments = 1;
            }
        }
        if (timed_lines && !pin_comments && warn) {
            note(s, "board config %s: no '# pin' comments (a dump without them): pad timers and DMA are unknown",
                 cfg_path);
        }
    }
    const fb_json_node_t *bpads = fb_json_get(bj, "pads");
    if (bpads && bpads->type == FB_JSON_ARRAY) {
        FOR_ITEMS(bpads, o) {
            const char *id = S(s, fb_json_get(o, "id"));
            pad_t *p = pad_by_id(s, id);
            if (!p) {
                const fb_json_node_t *kind = fb_json_get(o, "kind");
                if (!kind) {
                    if (warn) note(s, "board.json pad %s: not in the board config, left out", id);
                    continue;
                }
                p = new_pad(s, &cap);
                p->id = id;
                p->kind = S(s, kind);
            }
            if (pad_data(s, p, o) < 0) return -1;
            p->label = jstr(s, fb_json_get(o, "label"));
            const fb_json_node_t *fl = fb_json_get(o, "flags");
            if (fl && fl->type == FB_JSON_ARRAY) {
                strlist_t add = { 0 };
                FOR_ITEMS(fl, x) {
                    const char *z = jstr(s, x);
                    if (z && !sl_has(&p->flags, z)) sl_add(&add, z);
                }
                for (int i = 0; i < add.n; i++) sl_add(&p->flags, add.v[i]);
                free((void *)add.v);
            }
            if (fb_json_get(o, "hint")) p->hint = fb_json_get(o, "hint");
            if (fb_json_get(o, "note")) p->note = fb_json_get(o, "note");
        }
    }
    const fb_json_node_t *onb = fb_json_get(bj, "onboard");
    if (onb && onb->type == FB_JSON_ARRAY) {
        FOR_ITEMS(onb, ob) {
            const char *padid = S(s, fb_json_get(ob, "pad"));
            pad_t *p = pad_by_id(s, padid);
            if (!p) {
                if (warn) {
                    note(s, "board.json onboard %s on %s: no such pad in the board config, left out",
                         S(s, fb_json_get(ob, "device")), padid);
                }
                continue;
            }
            p->onboard = 1;
            p->device = S(s, fb_json_get(ob, "device"));
            p->detachable = fb_json_truthy(fb_json_get(ob, "detachable"));
            if (fb_json_truthy(fb_json_get(ob, "part"))) p->part = fb_json_get(ob, "part");
        }
    }
    strlist_t seen = { 0 };
    int bad = 0;
    for (int i = 0; i < s->npads && !bad; i++) {
        pad_t *p = &s->pads[i];
        for (int m = 0; m < spec->nmodelled; m++) {
            const modelled_t *md = &spec->modelled[m];
            int on = 0, dup = 0;
            for (int k = 0; k < MAX_MPADS && md->pads[k]; k++) on |= !strcmp(md->pads[k], p->id);
            for (int k = 0; k < p->ndevices; k++) dup |= !strcmp(p->devices[k], md->dev);
            if (!on || dup || p->ndevices == MAX_PAD_DEVICES) continue;
            int at = p->ndevices++;
            while (at > 0 && strcmp(p->devices[at - 1], md->dev) > 0) {
                p->devices[at] = p->devices[at - 1];
                at--;
            }
            p->devices[at] = md->dev;
        }
        if (p->label && *p->label) {
            int clash = 0;
            for (int k = 0; k < seen.n; k++) clash |= casefold_eq(seen.v[k], p->label);
            for (int k = 0; k < s->npads && !clash; k++) {
                clash |= casefold_eq(s->pads[k].id, p->label) && !casefold_eq(p->label, p->id);
            }
            if (clash) {
                err(s, "board.json: label %s is not unique", Rs(s, p->label));
                bad = 1;
            }
            sl_add(&seen, p->label);
        }
    }
    free((void *)seen.v);
    if (bad) return -1;
    b->name = fb_json_get(bj, "name");
    b->mcu = fb_json_get(bj, "mcu");
    /* machine, MSP UART, beeper, base CLI script (wiring.load_board's checks, in its order) */
    const fb_json_node_t *v = fb_json_get(bj, "machine");
    if (v && (v->type != FB_JSON_STRING || !*S(s, v))) {
        err(s, "board.json: machine %s: need the QEMU machine name", R(s, v));
        return -1;
    }
    b->machine = v ? S(s, v) : spec->id;
    /* the MSP UART: a uart pad of a CLI board (a unified config may leave any pad out) */
    v = fb_json_get(bj, "msp_uart");
    b->msp_uart = v && v->type == FB_JSON_STRING ? S(s, v) : v ? NULL : "uart1";
    const pad_t *mp = b->msp_uart ? pad_by_id(s, b->msp_uart) : NULL;
    if (!b->msp_uart || !*b->msp_uart || (b->cli && (!mp || strcmp(mp->kind, "uart")))) {
        err(s, "board.json: msp_uart %s is not a uart pad", v ? R(s, v) : "'uart1'");
        return -1;
    }
    v = fb_json_get(bj, "beeper");
    if (v && v->type != FB_JSON_NULL) {
        if (v->type != FB_JSON_STRING || !beeper_pin(S(s, v))) {
            err(s, "board.json: beeper %s: need a pin like 'C15' or 'C15:inv'", R(s, v));
            return -1;
        }
        b->beeper = S(s, v);
    }
    v = fb_json_get(bj, "firmware_family");
    if (v && (v->type != FB_JSON_STRING || !*S(s, v))) {
        err(s, "board.json: firmware_family %s: need a name like 'rotorflight' or 'inav'", R(s, v));
        return -1;
    }
    b->firmware_family = v ? S(s, v) : "rotorflight";
    v = fb_json_get(bj, "defaults_cli");
    if (v && v->type != FB_JSON_NULL) {
        if (v->type != FB_JSON_STRING) {
            err(s, "board.json: defaults_cli %s: need a file name", R(s, v));
            return -1;
        }
        if (*S(s, v)) b->defaults_cli = join(s, d, S(s, v));
    }
    return 0;
}

/* wiring.load_board into b (the pads built in s->pads, which is restored): 0 or -1 (errors
 * added); notes only when warn */
static int load_board(fb_session_t *s, board_t *b, const board_spec_t *spec, const char *config_path,
                      const char *board_json, int warn)
{
    pad_t *keep = s->pads;
    int nkeep = s->npads;
    s->pads = NULL;
    s->npads = 0;
    *b = (board_t) { .spec = spec };
    int rc = load_board_(s, b, config_path, board_json, warn);
    own_flags(s);
    b->pads = s->pads;
    b->npads = s->npads;
    s->pads = keep;
    s->npads = nkeep;
    return rc;
}

/* "PORT-A (uart4)" or "motor1" */
static const char *pad_name(fb_session_t *s, const pad_t *p)
{
    return p->label && *p->label ? s_fmt(s, "%s (%s)", p->label, p->id) : p->id;
}

static const char *pad_name_id(fb_session_t *s, const char *id)
{
    pad_t *p = pad_by_id(s, id);
    return p ? pad_name(s, p) : id;
}

/* ---- devices: the catalog as manifest.py reads it (fb_manifest's JSON) ----------------------- */

static int load_catalog(fb_session_t *s)
{
    size_t n = fb_device_catalog_json(NULL, 0) + 1;
    char *text = s_alloc(s, n);
    fb_device_catalog_json(text, n);
    char why[128];
    s->catalog = parse_doc(s, text, strlen(text), why, sizeof why);
    if (!s->catalog) {
        err(s, "device catalog: %s", why);
        return -1;
    }
    return 0;
}

static const fb_json_node_t *device_desc(fb_session_t *s, const fb_json_node_t *id)
{
    if (!id || id->type != FB_JSON_STRING) return NULL;
    FOR_ITEMS(s->catalog, d) {
        if (py_eq(s, fb_json_get(d, "id"), id)) return d;
    }
    return NULL;
}

static const fb_json_node_t *device_by_name(fb_session_t *s, const char *id)
{
    FOR_ITEMS(s->catalog, d) {
        if (fb_json_str_eq(fb_json_get(d, "id"), id)) return d;
    }
    return NULL;
}

/* ---- aircraft (wiring.aircraft_path, load_aircraft, roles, has_fdm) ------------------------ */

static const char *aircraft_path(fb_session_t *s, const char *ref)
{
    char *r = s_strdup(s, ref && *ref ? ref : "models/rs7/aircraft.json");
    for (char *c = r; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    size_t rl = strlen(r);
    if (!strchr(r, '/') && !(rl >= 5 && !strcmp(r + rl - 5, ".json"))) {
        r = s_fmt(s, "models/%s/aircraft.json", r);
    }
    r = expand_user(s, r);
    const char *p = is_abs(r) ? r : join(s, s->root, r);
    if (!exists(p)) {
        /* the app writes its own repo path (another machine: "D:/.../models/rs7/aircraft.json"):
         * the model is known by its directory name */
        const char *dn = basename_of(dirname_of(s, r));
        const char *cand = join(s, join(s, join(s, s->root, "models"), *dn ? dn : r), "aircraft.json");
        if (exists(cand)) return cand;
    }
    return p;
}

/* the FDM types the core has (fdm/: heli, plane) */
static int is_plane(const fb_json_node_t *a) { return fb_json_str_eq(fb_json_get(a, "fdm"), "plane"); }

static int has_fdm(fb_session_t *s, const fb_json_node_t *a)
{
    (void)s;
    return fb_json_str_eq(fb_json_get(a, "fdm"), "heli") || is_plane(a);
}

/* wiring.fdm_on: the aircraft's FDM runs on this board's machine */
static int fdm_on(fb_session_t *s, const fb_json_node_t *a, const board_spec_t *b)
{
    if (!has_fdm(s, a)) return 0;
    for (int i = 0; b->fdm[i]; i++) {
        if (fb_json_str_eq(fb_json_get(a, "fdm"), b->fdm[i])) return 1;
    }
    return 0;
}

typedef struct {
    const char *k;
    double lo, hi;
    int open_lo;
    int vec;                    /* > 0: a list of this many numbers (in lo..hi) */
} range_key_t;

static const range_key_t ESC_KEYS[] = {
    { "kv", 0, INFINITY, 1, 0 }, { "poles", 2, 64, 0, 0 }, { "tau_s", 0, INFINITY, 1, 0 },
    { "max_current_a", 0, INFINITY, 1, 0 }, { "idle_current_a", 0, INFINITY, 0, 0 },
    { "voltage_v", 0, INFINITY, 1, 0 }, { NULL, 0, 0, 0, 0 },
};

static const char *range_txt(fb_session_t *s, double lo, double hi, int open_lo)
{
    if (isinf(hi)) return isinf(lo) ? "a number" : s_fmt(s, "a number %s %g", open_lo ? ">" : ">=", lo);
    return s_fmt(s, "a number %g..%g", lo, hi);
}

/* load_aircraft: the checked aircraft object or NULL (errors added, each "aircraft <where>: ...");
 * *id = its id (the directory name when it has none) */
static const fb_json_node_t *load_aircraft(fb_session_t *s, const char *path, const char **id)
{
    size_t rl = strlen(s->root);
    const char *where = path;
    if (is_abs(path) && !strncmp(path, s->root, rl) && (path[rl] == '/' || s->root[rl - 1] == '/')) {
        where = path + rl + (path[rl] == '/');
    }
    size_t n;
    const char *why;
    char *text = read_file(s, path, &n, &why);
    if (!text) {
        err(s, "aircraft %s: %s", where, why);
        return NULL;
    }
    char jerr[128];
    const fb_json_node_t *a = parse_doc(s, text, n, jerr, sizeof jerr);
    if (!a) {
        err(s, "aircraft %s: %s", where, jerr);
        return NULL;
    }
    if (a->type != FB_JSON_OBJECT) {
        err(s, "aircraft %s: not a JSON object", where);
        return NULL;
    }
    *id = fb_json_get(a, "id") ? S(s, fb_json_get(a, "id")) : basename_of(dirname_of(s, path));
    int n0 = s->errs.n;
#define AERR(...) err(s, "aircraft %s: %s", where, s_fmt(s, __VA_ARGS__))
    const fb_json_node_t *acts = fb_json_get(a, "actuators");
    int acts_ok = 1;
    if (acts) {
        if (acts->type != FB_JSON_ARRAY) {
            acts_ok = 0;
        } else {
            FOR_ITEMS(acts, x) {
                if (!actuator_name(jstr(s, x))) acts_ok = 0;
                FOR_ITEMS(acts, y) {
                    if (y < x && py_eq(s, x, y)) acts_ok = 0;
                }
            }
        }
    }
    if (!acts_ok) {
        AERR("actuators: need a list of unique names (letters, digits, _)");
        acts = NULL;
    }
    int nacts = acts ? acts->count : 0;
    const fb_json_node_t *ins = fb_json_get(a, "fdm_inputs");
    if (ins && ins->type == FB_JSON_NULL) ins = NULL;
    if (ins) {
        int ok = ins->type == FB_JSON_ARRAY;
        if (ok) FOR_ITEMS(ins, x) ok &= x->type == FB_JSON_STRING;
        if (!ok) {
            AERR("fdm_inputs: need a list of actuator names");
            ins = NULL;
        }
    }
    const fb_json_node_t *thr = fb_json_get(a, "throttle");
    if (thr && thr->type == FB_JSON_NULL) thr = NULL;
    if (thr && thr->type != FB_JSON_STRING) {
        AERR("throttle: need an actuator name");
        thr = NULL;
    }
    const fb_json_node_t *t = fb_json_get(a, "transmission");
    if (is_plane(a)) {
        if (!ins || ins->count != 4) AERR("fdm_inputs: the plane FDM needs 4 (aileron_l, aileron_r, elevator, rudder, in that order)");
        if (!thr) AERR("throttle: the plane FDM needs the ESC's actuator name");
    } else if (has_fdm(s, a)) {
        if (!ins || ins->count != 4) AERR("fdm_inputs: the heli FDM needs 4 (swash servos in RotorFlight order, tail)");
        if (!thr) AERR("throttle: the heli FDM needs the ESC's actuator name");
        if (!t || t->type != FB_JSON_OBJECT || !fb_json_get(t, "main_rotor_gear_ratio")) {
            AERR("transmission.main_rotor_gear_ratio: missing (the heli FDM needs the gear, [pinion, main])");
        }
    }
    const fb_json_node_t *chk[16];
    int nchk = 0;
    if (ins) FOR_ITEMS(ins, x) if (nchk < 15) chk[nchk++] = x;
    if (fb_json_truthy(thr)) chk[nchk++] = thr;
    for (int i = 0; i < nchk; i++) {
        int in = 0;
        if (nacts) FOR_ITEMS(acts, y) in |= py_eq(s, chk[i], y);
        if (nacts && !in) AERR("%s (fdm_inputs/throttle) is not in actuators", R(s, chk[i]));
    }
    if (fb_json_truthy(thr) && fb_json_truthy(ins)) {
        int in = 0;
        FOR_ITEMS(ins, x) in |= py_eq(s, x, thr);
        if (in) AERR("throttle %s is also an fdm_input", R(s, thr));
    }
    if (t && t->type != FB_JSON_NULL) {
        if (t->type != FB_JSON_OBJECT) {
            AERR("transmission: need an object");
        } else if (fb_json_get(t, "main_rotor_gear_ratio")) {
            const fb_json_node_t *g = fb_json_get(t, "main_rotor_gear_ratio");
            int ok = g->type == FB_JSON_ARRAY && g->count == 2;
            if (ok) FOR_ITEMS(g, x) ok &= is_num(x) && num(x) > 0;
            if (!ok) AERR("transmission.main_rotor_gear_ratio %s: need [pinion, main], 2 numbers > 0", R(s, g));
        }
    }
    /* fdm_params: the model's own parameters (docs/fdm.md); the core builds the FDM from them and
     * names a wrong one (a session-check does it too), the resolver only wants an object */
    {
        const fb_json_node_t *fp = fb_json_get(a, "fdm_params");
        if (fp && fp->type != FB_JSON_OBJECT) AERR("fdm_params: need an object (docs/fdm.md)");
    }
    for (int sect = 0; sect < 1; sect++) {
        const char *sn = "esc";
        const range_key_t *keys = ESC_KEYS;
        const fb_json_node_t *d = fb_json_get(a, sn);
        if (!d) continue;
        if (d->type != FB_JSON_OBJECT) {
            AERR("%s: need an object", sn);
            continue;
        }
        FOR_MEMBERS(d, k) {
            const char *kn = S(s, k);
            const fb_json_node_t *v = k + 1;
            int ki = -1;
            for (int i = 0; keys[i].k; i++) {
                if (!strcmp(keys[i].k, kn)) ki = i;
            }
            if (ki < 0) {
                sbuf_t kl = { 0 };
                for (int i = 0; keys[i].k; i++) sb_printf(&kl, "%s%s", i ? ", " : "", keys[i].k);
                AERR("%s.%s: unknown key (known: %s)", sn, kn, s_take(s, &kl));
                continue;
            }
            double lo = keys[ki].lo, hi = keys[ki].hi, x = num(v);
            if (keys[ki].vec) {
                int ok = v->type == FB_JSON_ARRAY && v->count == keys[ki].vec;
                if (ok) FOR_ITEMS(v, e) ok &= is_num(e) && num(e) >= lo && num(e) <= hi;
                if (!ok) AERR("%s.%s %s: need a list of %d numbers", sn, kn, R(s, v), keys[ki].vec);
            } else if (sect == 2 && !strcmp(kn, "kt") && v->type == FB_JSON_NULL) {
                /* null: derived */
            } else if (!is_num(v) || x < lo || x > hi || (keys[ki].open_lo && x == lo)) {
                AERR("%s.%s %s: need %s", sn, kn, R(s, v), range_txt(s, lo, hi, keys[ki].open_lo));
            } else if (!strcmp(kn, "poles") && (x != floor(x) || (long long)x % 2)) {
                AERR("esc.poles %s: need an even integer 2..64", R(s, v));
            }
        }
    }
    const fb_json_node_t *c = fb_json_get(a, "centre_us");
    if (c) {
        if (c->type != FB_JSON_OBJECT) {
            AERR("centre_us: need an object {fdm input: us}");
        } else {
            FOR_MEMBERS(c, k) {
                const fb_json_node_t *v = k + 1;
                int in = 0;
                if (ins) FOR_ITEMS(ins, x) in |= py_eq(s, x, k);
                if (ins && !in) {
                    sbuf_t il = { 0 };
                    int i = 0;
                    FOR_ITEMS(ins, x) sb_printf(&il, "%s%s", i++ ? ", " : "", S(s, x));
                    AERR("centre_us.%s: not one of the fdm_inputs (%s)", S(s, k), s_take(s, &il));
                } else if (!is_num(v) || num(v) != floor(num(v)) || num(v) < 1000 || num(v) > 2000) {
                    AERR("centre_us.%s %s: need an integer 1000..2000", S(s, k), R(s, v));
                }
            }
        }
    }
    static const char *const SCENARIO_KEYS[] = { "env_default", "acc_calibration", NULL };
    for (int i = 0; SCENARIO_KEYS[i]; i++) {
        const char *kn = SCENARIO_KEYS[i];
        const fb_json_node_t *ed = fb_json_get(a, kn);
        if (!ed || ed->type == FB_JSON_NULL) continue;
        if (ed->type != FB_JSON_STRING) {
            AERR("%s %s: need a path (a scenario file, relative to the aircraft's directory)", kn, R(s, ed));
        } else if (!exists(join(s, dirname_of(s, path), S(s, ed)))) {
            AERR("%s %s: no such file next to the aircraft", kn, R(s, ed));
        }
    }
    /* functions (wiring.py FUNCTION_PARAMS / FUNCTION_KEYS) */
    static const char *const FN_NAMES[] = { "calibrate_acc", "calibrate_gyro", "launch", "calibrate_mag", NULL };
    static const char *const FN_KEYS[] = { "name", "label", "auto_first_start", "scenario", "params", NULL };
    static const range_key_t FN_GYRO[] = { { "hold_s", 0, 60, 0, 0 }, { NULL, 0, 0, 0, 0 } };
    static const range_key_t FN_LAUNCH[] = { { "speed_kmh", 0, 360, 1, 0 }, { "duration_s", 0, 10, 1, 0 },
                                             { "pitch_deg", -60, 60, 0, 0 }, { NULL, 0, 0, 0, 0 } };
    static const range_key_t FN_NONE[] = { { NULL, 0, 0, 0, 0 } };
    const fb_json_node_t *fl = fb_json_get(a, "functions");
    if (fl && fl->type != FB_JSON_NULL && fl->type != FB_JSON_ARRAY) {
        AERR("functions: need a list of {name, label, auto_first_start, scenario, params}");
    } else if (fl && fl->type == FB_JSON_ARRAY) {
        int fi = 0, seen[4] = { 0, 0, 0, 0 };
        FOR_ITEMS(fl, f) {
            const char *w = s_fmt(s, "functions[%d]", fi++);
            if (f->type != FB_JSON_OBJECT) {
                AERR("%s: need an object {name, label, auto_first_start, scenario, params}", w);
                continue;
            }
            const fb_json_node_t *nv = fb_json_get(f, "name");
            int ni = -1;
            for (int k = 0; FN_NAMES[k]; k++) {
                if (nv && fb_json_str_eq(nv, FN_NAMES[k])) ni = k;
            }
            if (ni < 0) {
                AERR("%s.name %s: need one of calibrate_acc, calibrate_gyro, launch, calibrate_mag", w,
                     nv ? R(s, nv) : "None");
                continue;
            }
            if (seen[ni]++) AERR("%s.name %s: listed twice", w, R(s, nv));
            FOR_MEMBERS(f, k) {
                if (!in_strs(FN_KEYS, S(s, k))) {
                    AERR("%s.%s: unknown key (known: name, label, auto_first_start, scenario, params)", w, S(s, k));
                }
            }
            const fb_json_node_t *lb = fb_json_get(f, "label");
            if (lb && lb->type != FB_JSON_STRING) AERR("%s.label %s: need a string", w, R(s, lb));
            const fb_json_node_t *au = fb_json_get(f, "auto_first_start");
            if (au && !is_bool(au)) {
                AERR("%s.auto_first_start %s: need true|false", w, R(s, au));
            } else if (au && fb_json_truthy(au) && ni == 2) {
                AERR("%s.auto_first_start: launch is never part of the first start", w);
            }
            const fb_json_node_t *sc = fb_json_get(f, "scenario");
            if (sc && sc->type != FB_JSON_NULL) {
                if (ni != 0) {
                    AERR("%s.scenario: only calibrate_acc takes a scenario (its poses)", w);
                } else if (sc->type != FB_JSON_STRING) {
                    AERR("%s.scenario %s: need a path (a scenario file, relative to the aircraft's directory)", w, R(s, sc));
                } else if (!exists(join(s, dirname_of(s, path), S(s, sc)))) {
                    AERR("%s.scenario %s: no such file next to the aircraft", w, R(s, sc));
                }
            }
            const fb_json_node_t *pr = fb_json_get(f, "params");
            if (pr && pr->type != FB_JSON_OBJECT) {
                AERR("%s.params: need an object", w);
                continue;
            }
            const range_key_t *pk = ni == 1 ? FN_GYRO : ni == 2 ? FN_LAUNCH : FN_NONE;
            if (pr) FOR_MEMBERS(pr, k) {
                const fb_json_node_t *v = k + 1;
                int ki = -1;
                for (int j = 0; pk[j].k; j++) {
                    if (!strcmp(pk[j].k, S(s, k))) ki = j;
                }
                if (ki < 0) {
                    sbuf_t kl = { 0 };
                    for (int j = 0; pk[j].k; j++) sb_printf(&kl, "%s%s", j ? ", " : "", pk[j].k);
                    const char *kls = kl.p ? s_take(s, &kl) : "none";
                    AERR("%s.params.%s: unknown key (known: %s)", w, S(s, k), kls);
                    continue;
                }
                double x = num(v);
                if (!is_num(v) || x < pk[ki].lo || x > pk[ki].hi || (pk[ki].open_lo && x == pk[ki].lo)) {
                    AERR("%s.params.%s %s: need %s", w, S(s, k), R(s, v),
                         range_txt(s, pk[ki].lo, pk[ki].hi, pk[ki].open_lo));
                }
            }
        }
    }
    const fb_json_node_t *el = fb_json_get(a, "env");
    if (el && el->type != FB_JSON_NULL) {
        int ok = el->type == FB_JSON_ARRAY;
        if (ok) FOR_ITEMS(el, x) ok &= x->type == FB_JSON_STRING;
        if (!ok) AERR("env: need a list of scenario paths");
    }
    const fb_json_node_t *fc = fb_json_get(a, "firmware_cli");
    if (fc && fc->type != FB_JSON_NULL && fc->type != FB_JSON_STRING) {
        AERR("firmware_cli %s: need a path (a CLI script, relative to the data root)", R(s, fc));
    }
#undef AERR
    return s->errs.n > n0 ? NULL : a;
}

/* roles(): the servo actuators (the FDM's inputs) and the ESC's (throttle) */
static int roles(fb_session_t *s, const char **ins, const char **thr)
{
    const fb_json_node_t *a = s->ac, *acts = fb_json_get(a, "actuators");
    int nacts = acts && acts->type == FB_JSON_ARRAY ? acts->count : 0;
    const fb_json_node_t *t = fb_json_get(a, "throttle");
    if (fb_json_truthy(t)) {
        *thr = S(s, t);
    } else {
        int has_motor = 0;
        for (int i = 0; i < nacts; i++) has_motor |= fb_json_str_eq(fb_json_at(acts, i), "motor");
        *thr = !nacts || has_motor ? "motor" : S(s, fb_json_at(acts, nacts - 1));
    }
    const fb_json_node_t *fi = fb_json_get(a, "fdm_inputs");
    int n = 0;
    if (fi && fi->type == FB_JSON_ARRAY) {
        FOR_ITEMS(fi, x) if (n < 16) ins[n++] = S(s, x);
    } else if (nacts) {
        for (int i = 0; i < nacts && n < 16; i++) {
            const char *z = S(s, fb_json_at(acts, i));
            if (strcmp(z, *thr)) ins[n++] = z;
        }
    } else {
        for (int i = 0; i < 4; i++) ins[n++] = DEFAULT_ACTUATORS[i];
    }
    return n;
}

/* ---- the assembly (wiring.resolve, machine_options, device_config) ------------------------- */

static const fb_json_node_t *prop_get(const entry_t *e, const char *k)
{
    for (int i = 0; i < e->nprops; i++) {
        if (!strcmp(e->props[i].k, k)) return e->props[i].v;
    }
    return NULL;
}

/* _check_prop: NULL or the problem */
static const char *check_prop(fb_session_t *s, const char *dev, const fb_json_node_t *desc,
                              const char *k, const fb_json_node_t *v)
{
    const char *t = jstr(s, fb_json_get(desc, "type"));
    const fb_json_node_t *lo = fb_json_get(desc, "min"), *hi = fb_json_get(desc, "max");
    if (lo && lo->type == FB_JSON_NULL) lo = NULL;
    if (hi && hi->type == FB_JSON_NULL) hi = NULL;
    const char *rng = lo && hi ? s_fmt(s, " %g..%g", num(lo), num(hi)) : "";
    if (!t) return NULL;
    if (!strcmp(t, "float")) {
        if (!is_num(v) || (lo && num(v) < num(lo)) || (hi && num(v) > num(hi))) {
            return s_fmt(s, "%s %s %s: need a number%s", dev, k, R(s, v), rng);
        }
    } else if (!strcmp(t, "int")) {
        if (!is_int(v) || (lo && num(v) < num(lo)) || (hi && num(v) > num(hi))) {
            return s_fmt(s, "%s %s %s: need an integer%s", dev, k, R(s, v), rng);
        }
    } else if (!strcmp(t, "bool")) {
        if (!is_bool(v)) return s_fmt(s, "%s %s %s: need true|false", dev, k, R(s, v));
    } else if (!strcmp(t, "enum")) {
        const fb_json_node_t *opts = fb_json_get(desc, "options");
        int in = 0;
        sbuf_t ol = { 0 };
        if (opts && opts->type == FB_JSON_ARRAY) {
            int i = 0;
            FOR_ITEMS(opts, o) {
                in |= py_eq(s, o, v);
                sb_printf(&ol, "%s%s", i++ ? ", " : "", S(s, o));
            }
        }
        const char *ols = s_take(s, &ol);
        if (!in) return s_fmt(s, "%s %s %s: need one of %s", dev, k, R(s, v), ols);
    } else if (!strcmp(t, "actuator")) {
        if (!actuator_name(jstr(s, v))) {
            return s_fmt(s, "%s %s %s: actuator name, letters, digits and _ only", dev, k, R(s, v));
        }
    }
    return NULL;
}

static void entry_add(fb_session_t *s, int *cap, entry_t e)
{
    if (s->nentries == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        entry_t *ne = s_alloc(s, sizeof *ne * (size_t)*cap);
        if (s->nentries) memcpy(ne, s->entries, sizeof *ne * (size_t)s->nentries);
        s->entries = ne;
    }
    s->entries[s->nentries++] = e;
}

static void prop_set(fb_session_t *s, entry_t *e, int *cap, const char *k, const fb_json_node_t *v)
{
    for (int i = 0; i < e->nprops; i++) {
        if (!strcmp(e->props[i].k, k)) {
            e->props[i].v = v;
            return;
        }
    }
    if (e->nprops == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        prop_t *np = s_alloc(s, sizeof *np * (size_t)*cap);
        if (e->nprops) memcpy(np, e->props, sizeof *np * (size_t)e->nprops);
        e->props = np;
    }
    e->props[e->nprops++] = (prop_t) { k, v };
}

static const pad_t *find_pad(fb_session_t *s, const fb_json_node_t *ref)
{
    const char *z = jstr(s, ref);
    if (!z) return NULL;
    pad_t *p = pad_by_id(s, z);
    if (p) return p;
    for (int i = 0; i < s->npads; i++) {
        if (s->pads[i].label && *s->pads[i].label && casefold_eq(s->pads[i].label, z)) return &s->pads[i];
    }
    return NULL;
}

static int is_old_key(fb_session_t *s, const fb_json_node_t *k)
{
    (void)s;
    return fb_json_str_eq(k, "slot");
}

static int is_new_key(fb_session_t *s, const fb_json_node_t *k)
{
    (void)s;
    return fb_json_str_eq(k, "pad") || fb_json_str_eq(k, "port") || fb_json_str_eq(k, "detached");
}

/* wiring.resolve: 0, or -1 with the errors added */
static int resolve(fb_session_t *s, const fb_json_node_t *w)
{
    if (w->type != FB_JSON_ARRAY) {
        err(s, "wiring: need a list");
        return -1;
    }
    int old = 0, nw = 0;
    FOR_ITEMS(w, x) {
        if (x->type != FB_JSON_OBJECT) continue;
        FOR_MEMBERS(x, k) {
            old |= is_old_key(s, k);
            nw |= is_new_key(s, k);
        }
    }
    if (old && nw) {
        sbuf_t bad = { 0 };
        int i = 0, j = 0;
        FOR_ITEMS(w, x) {
            if (x->type == FB_JSON_OBJECT && fb_json_get(x, "slot")) {
                sb_printf(&bad, "%swiring[%d]", j++ ? ", " : "", i);
            }
            i++;
        }
        err(s, "wiring: mixes the old form ({device, slot}) and the new one ({device, port, pad} / "
            "detached); entries with slot: %s. Use one form for the whole list", s_take(s, &bad));
        return -1;
    }
    old = old && w->count > 0;
    const board_spec_t *spec = s->board.spec;
    const char *bid = s->board_id;
    if (old && !spec->legacy) {
        err(s, "wiring: the old form ({device, slot}) is only for nexus-xr benches; on %s use {device, port, pad}",
            bid);
        return -1;
    }
    const char *fdm_ins[16], *throttle;
    int nins = roles(s, fdm_ins, &throttle);
    int nerr0 = s->errs.n;
    int *detached = s_alloc(s, sizeof *detached * (size_t)(s->npads + 1));
    memset(detached, 0, sizeof *detached * (size_t)(s->npads + 1));
    int cap = 0;
    s->entries = NULL;
    s->nentries = 0;
#define PLACE_ERR(...) do { \
        const char *m_ = s_fmt(s, __VA_ARGS__); \
        if (old) note(s, "%s: %s; left out (old-form session)", where, m_); \
        else err(s, "%s: %s", where, m_); \
    } while (0)
    int idx = -1;
    FOR_ITEMS(w, x) {
        idx++;
        const char *where = s_fmt(s, "wiring[%d]", idx);
        if (x->type != FB_JSON_OBJECT) {
            err(s, "%s: need an object", where);
            continue;
        }
        const fb_json_node_t *devn = fb_json_get(x, "device");
        const fb_json_node_t *d = device_desc(s, devn);
        const fb_json_node_t *det = fb_json_get(x, "detached");
        if (det && !is_bool(det)) {
            err(s, "%s: detached must be true|false", where);
            continue;
        }
        if (det && det->type == FB_JSON_TRUE) {
            const pad_t *p = NULL;
            if (fb_json_get(x, "pad")) {
                p = find_pad(s, fb_json_get(x, "pad"));
            } else {
                for (int i = 0; i < s->npads && !p; i++) {
                    if (s->pads[i].onboard && devn && devn->type == FB_JSON_STRING &&
                        fb_json_str_eq(devn, s->pads[i].device)) {
                        p = &s->pads[i];
                    }
                }
            }
            if (!p || !p->onboard || (fb_json_truthy(devn) && !fb_json_str_eq(devn, p->device))) {
                err(s, "%s: %s is not an onboard device of %s, nothing to detach", where,
                    fb_json_truthy(devn) ? S(s, devn) : S(s, fb_json_get(x, "pad")), bid);
            } else if (!p->detachable) {
                err(s, "%s: onboard %s on %s is soldered (detachable: false), it cannot be removed", where,
                    p->device, pad_name(s, p));
            } else {
                detached[p - s->pads] = 1;
            }
            continue;
        }
        if (!d) {
            sbuf_t kn = { 0 };
            int i = 0;
            FOR_ITEMS(s->catalog, c) sb_printf(&kn, "%s%s", i++ ? ", " : "", S(s, fb_json_get(c, "id")));
            PLACE_ERR("unknown device %s (known: %s)", R(s, devn), s_take(s, &kn));
            continue;
        }
        const char *dev = S(s, devn);
        const fb_json_node_t *ports = fb_json_get(d, "ports"), *port0 = fb_json_at(ports, 0);
        const char *port0_id = S(s, fb_json_get(port0, "id"));
        const fb_json_node_t *portn = fb_json_get(x, "port");
        const fb_json_node_t *port = NULL;
        FOR_ITEMS(ports, pp) {
            if (!port && (portn ? py_eq(s, fb_json_get(pp, "id"), portn)
                                : fb_json_str_eq(fb_json_get(pp, "id"), port0_id))) {
                port = pp;
            }
        }
        if (!port) {
            sbuf_t pl = { 0 };
            int i = 0;
            FOR_ITEMS(ports, pp) sb_printf(&pl, "%s%s", i++ ? ", " : "", S(s, fb_json_get(pp, "id")));
            err(s, "%s: device %s has no port %s (ports: %s)", where, dev,
                portn ? R(s, portn) : Rs(s, port0_id), s_take(s, &pl));
            continue;
        }
        const char *port_id = S(s, fb_json_get(port, "id"));
        if (fb_json_truthy(fb_json_get(port, "bundled"))) {
            err(s, "%s: %s.%s comes with the %s pad, wire %s.%s", where, dev, port_id, port0_id, dev, port0_id);
            continue;
        }
        const fb_json_node_t *ref = fb_json_get(x, "pad") ? fb_json_get(x, "pad") : fb_json_get(x, "slot");
        const pad_t *p = find_pad(s, ref);
        if (!p) {
            sbuf_t kn = { 0 };
            for (int i = 0; i < s->npads; i++) {
                if (s->pads[i].label && *s->pads[i].label) sb_printf(&kn, "%s%s=%s", i ? ", " : "", s->pads[i].label, s->pads[i].id);
                else sb_printf(&kn, "%s%s", i ? ", " : "", s->pads[i].id);
            }
            PLACE_ERR("unknown pad %s on %s (pads: %s)", R(s, ref), bid, s_take(s, &kn));
            continue;
        }
        const char *pkind = S(s, fb_json_get(port, "kind"));
        if (!kind_fits(p->kind, pkind)) {
            PLACE_ERR("%s.%s is a %s port, pad %s is %s", dev, port_id, pkind, pad_name(s, p), p->kind);
            continue;
        }
        if (!strcmp(p->kind, "uart")) {
            const fb_json_node_t *dirn = fb_json_get(port, "dir");
            const char *dir = dirn ? S(s, dirn) : "both";
            const char *need[2];
            int nneed = 0;
            if (!strcmp(dir, "in")) need[nneed++] = "tx";
            else if (!strcmp(dir, "out")) need[nneed++] = "rx";
            else { need[nneed++] = "tx"; need[nneed++] = "rx"; }
            sbuf_t miss = { 0 };
            int usb = sl_has(&p->flags, "usb");
            for (int i = 0; i < nneed; i++) {
                if (!pin_get(p, need[i]) && !usb) sb_printf(&miss, "%s%s", miss.len ? "/" : "", need[i]);
            }
            if (miss.len) {
                const char *ms = s_take(s, &miss);
                PLACE_ERR("%s.%s (%s) needs %s on the board side, pad %s has no %s", dev, port_id,
                          dirn ? S(s, dirn) : "None", nneed == 2 ? "TX+RX" : upper(s, need[0], 2),
                          pad_name(s, p), upper(s, ms, strlen(ms)));
                continue;
            }
        }
        /* props: the descriptor's; the app writes actuator "", reversed false and option ""
         * on every entry (option = the ESC's telemetry) */
        entry_t e = { .device = dev, .port = port_id, .pad = p->id, .src = where };
        int pcap = 0, bad = 0;
        const fb_json_node_t *pdesc = fb_json_get(d, "props");
        FOR_MEMBERS(x, kn) {
            const char *k = S(s, kn);
            const fb_json_node_t *v = kn + 1;
            if (in_strs(ENTRY_KEYS, k)) continue;
            if ((!strcmp(k, "actuator") && fb_json_str_eq(v, "")) ||
                (!strcmp(k, "reversed") && v->type == FB_JSON_FALSE) ||
                (!strcmp(k, "option") && fb_json_str_eq(v, ""))) {
                continue;
            }
            const fb_json_node_t *pd = NULL;
            int has_tel = 0;
            FOR_ITEMS(pdesc, q) has_tel |= fb_json_str_eq(fb_json_get(q, "id"), "telemetry");
            if (!strcmp(k, "option") && has_tel) {
                const fb_json_node_t *tel = fb_json_get(x, "telemetry");
                if (tel && !py_eq(s, tel, v)) {
                    err(s, "%s: option %s and telemetry %s disagree", where, R(s, v), R(s, tel));
                    bad = 1;
                    continue;
                }
                k = "telemetry";
            }
            FOR_ITEMS(pdesc, q) {
                if (fb_json_str_eq(fb_json_get(q, "id"), k)) pd = q;
            }
            if (!pd) {
                sbuf_t pl = { 0 };
                int i = 0;
                FOR_ITEMS(pdesc, q) sb_printf(&pl, "%s%s", i++ ? ", " : "props: ", S(s, fb_json_get(q, "id")));
                err(s, "%s: %s has no prop %s (%s)", where, dev, Rs(s, k),
                    pl.len ? s_take(s, &pl) : "it takes none");
                free(pl.p);
                bad = 1;
                continue;
            }
            const char *m = check_prop(s, dev, pd, k, v);
            if (m) {
                err(s, "%s: %s", where, m);
                bad = 1;
                continue;
            }
            prop_set(s, &e, &pcap, k, v);
        }
        const fb_json_node_t *a = prop_get(&e, "actuator");
        if (a) {
            const char *an = S(s, a);
            int in = 0;
            for (int i = 0; i < nins; i++) in |= !strcmp(fdm_ins[i], an);
            if (!strcmp(dev, "servo") && !in) {
                sbuf_t il = { 0 };
                for (int i = 0; i < nins; i++) sb_printf(&il, "%s%s", i ? ", " : "", fdm_ins[i]);
                err(s, "%s: servo actuator %s is not one of the aircraft's FDM inputs (%s)", where, R(s, a),
                    s_take(s, &il));
                bad = 1;
            } else if (!strcmp(dev, "esc") && (strcmp(port_id, port0_id) || strcmp(an, throttle))) {
                err(s, "%s: esc actuator %s: the ESC's signal drives the aircraft's throttle %s%s", where, R(s, a),
                    Rs(s, throttle), strcmp(port_id, port0_id) ? s_fmt(s, ", not on its %s port", port_id) : "");
                bad = 1;
            }
        }
        if (bad) continue;
        e.onboard = p->onboard && !strcmp(p->device, dev);
        entry_add(s, &cap, e);
    }
#undef PLACE_ERR

    /* old form: a pad named twice keeps the first entry (before: one device there) */
    if (old) {
        int k = 0;
        for (int i = 0; i < s->nentries; i++) {
            entry_t *e = &s->entries[i];
            const entry_t *first = NULL;
            for (int j = 0; j < k && !first; j++) {
                if (!strcmp(s->entries[j].pad, e->pad)) first = &s->entries[j];
            }
            if (first && strcmp(pad_by_id(s, e->pad)->kind, "i2c")) {
                note(s, "%s: pad %s is already used by %s.%s (%s); left out (old-form session)", e->src,
                     pad_name_id(s, e->pad), first->device, first->port, first->src);
                continue;
            }
            s->entries[k++] = *e;
        }
        s->nentries = k;
    }

    /* onboard devices: present unless detached (new form) / unless the old-form list omits them */
    int old_rx = 0, rx_assumed = 0;
    for (int i = 0; i < s->nentries; i++) {
        old_rx |= !strcmp(s->entries[i].device, "crsf") && modelled_on(spec, "crsf", "uart", s->entries[i].pad);
    }
    for (int pi = 0; pi < s->npads; pi++) {
        pad_t *p = &s->pads[pi];
        if (!p->onboard) continue;
        const entry_t *named = NULL;
        int any_here = 0;
        for (int i = 0; i < s->nentries; i++) {
            if (strcmp(s->entries[i].pad, p->id)) continue;
            any_here = 1;
            if (!named && !strcmp(s->entries[i].device, p->device)) named = &s->entries[i];
        }
        if (detached[pi]) {
            if (named) err(s, "%s: onboard %s on %s is both wired and detached", named->src, p->device, pad_name(s, p));
            continue;
        }
        if (named) continue;
        if (old) {
            if (!strcmp(p->device, "crsf") && !old_rx && !any_here) {
                rx_assumed = 1;         /* the old form's receiver: assumed present */
            } else if (p->detachable) {
                detached[pi] = 1;
                note(s, "onboard %s on %s not in the (old-form) wiring: detached", p->device, pad_name(s, p));
                continue;
            } else if (p->group) {      /* a pad the old form can name at all */
                note(s, "onboard %s on %s not in the (old-form) wiring: it is soldered, kept", p->device,
                     pad_name(s, p));
            }
        }
        const fb_json_node_t *d = device_by_name(s, p->device);
        const char *port0 = d ? S(s, fb_json_get(fb_json_at(fb_json_get(d, "ports"), 0), "id")) : p->device;
        entry_add(s, &cap, (entry_t) { .device = p->device, .port = port0, .pad = p->id,
                                       .onboard = 1, .src = s_fmt(s, "onboard %s", p->id) });
    }

    /* old form: the machine before the wiring model always had the receiver, the ESC, servo1..4 */
    if (old) {
        sbuf_t assumed = { 0 };
        int rx_here = 0;
        for (int i = 0; i < s->nentries; i++) rx_here |= !strcmp(s->entries[i].pad, OLD_RX_PAD);
        if (!old_rx && !rx_assumed && pad_by_id(s, OLD_RX_PAD) && !rx_here) {
            entry_add(s, &cap, (entry_t) { .device = "crsf", .port = "uart", .pad = OLD_RX_PAD,
                                           .src = "old-form default" });
            rx_assumed = 1;
        }
        if (rx_assumed) sb_printf(&assumed, "receiver on %s", pad_name_id(s, OLD_RX_PAD));
        int esc = 0, esc_pad_used = 0;
        int used_servo[MAX_SERVOS] = { 0 };
        for (int i = 0; i < s->nentries; i++) {
            const entry_t *e = &s->entries[i];
            esc |= !strcmp(e->device, "esc") && !strcmp(e->port, "signal");
            esc_pad_used |= !strcmp(e->pad, OLD_ESC_PAD);
            for (int k = 0; k < spec->nservo; k++) used_servo[k] |= !strcmp(e->pad, spec->servo_pads[k]);
        }
        if (!esc && !esc_pad_used && pad_by_id(s, OLD_ESC_PAD)) {
            entry_add(s, &cap, (entry_t) { .device = "esc", .port = "signal", .pad = OLD_ESC_PAD,
                                           .src = "old-form default" });
            sb_printf(&assumed, "%sESC on %s", assumed.len ? "; " : "", OLD_ESC_PAD);
        }
        sbuf_t miss = { 0 };
        for (int k = 0; k < spec->nservo; k++) {
            if (used_servo[k] || !pad_by_id(s, spec->servo_pads[k])) continue;
            entry_add(s, &cap, (entry_t) { .device = "servo", .port = "pwm", .pad = spec->servo_pads[k],
                                           .src = "old-form default" });
            sb_printf(&miss, "%s%s", miss.len ? ", " : "", spec->servo_pads[k]);
        }
        if (miss.len) sb_printf(&assumed, "%sservos on %s", assumed.len ? "; " : "", miss.p);
        free(miss.p);
        if (assumed.len) {
            note(s, "old-form session: receiver/ESC/servos assumed present (not in the list: %s)", assumed.p);
        }
        free(assumed.p);
    }

    /* one device per pad, except on a bus; on a bus one device of a kind (same address) */
    for (int i = 0; i < s->nentries; i++) {
        const char *pid = s->entries[i].pad;
        int first = 1, cnt = 0;
        for (int j = 0; j < i; j++) first &= strcmp(s->entries[j].pad, pid) != 0;
        if (!first) continue;
        for (int j = i; j < s->nentries; j++) cnt += !strcmp(s->entries[j].pad, pid);
        if (cnt < 2) continue;
        if (strcmp(pad_by_id(s, pid)->kind, "i2c")) {
            sbuf_t b = { 0 };
            for (int j = i; j < s->nentries; j++) {
                const entry_t *e = &s->entries[j];
                if (!strcmp(e->pad, pid)) {
                    sb_printf(&b, "%s%s.%s (%s)", b.len ? ", " : "", e->device, e->port, e->src);
                }
            }
            err(s, "pad %s used by %s", pad_name_id(s, pid), s_take(s, &b));
        } else {
            strlist_t devs = { 0 };
            for (int j = i; j < s->nentries; j++) {
                const entry_t *e = &s->entries[j];
                if (strcmp(e->pad, pid) || sl_has(&devs, e->device)) continue;
                int c = 0;
                for (int m = i; m < s->nentries; m++) {
                    c += !strcmp(s->entries[m].pad, pid) && !strcmp(s->entries[m].device, e->device);
                }
                if (c > 1) sl_add(&devs, e->device);
            }
            if (devs.n > 1) qsort((void *)devs.v, (size_t)devs.n, sizeof *devs.v, cmp_str);
            for (int k = 0; k < devs.n; k++) {
                sbuf_t b = { 0 };
                for (int j = i; j < s->nentries; j++) {
                    const entry_t *e = &s->entries[j];
                    if (!strcmp(e->pad, pid) && !strcmp(e->device, devs.v[k])) {
                        sb_printf(&b, "%s%s", b.len ? ", " : "", e->src);
                    }
                }
                err(s, "bus %s: two %s (same I2C address): %s", pad_name_id(s, pid), devs.v[k], s_take(s, &b));
            }
            free((void *)devs.v);
        }
    }

    /* one OSD per session: the machine models one MSP DisplayPort device and the app one canvas */
    {
        sbuf_t b = { 0 };
        int n = 0;
        for (int i = 0; i < s->nentries; i++) {
            const entry_t *e = &s->entries[i];
            if (strcmp(e->device, "msp_osd")) continue;
            sb_printf(&b, "%s%s (%s)", n++ ? ", " : "", pad_name_id(s, e->pad), e->src);
        }
        if (n > 1) err(s, "one OSD per session: msp_osd on %s", b.p);
        free(b.p);
    }
    /* one GPS, one battery: the machine models one of each */
    static const char *const one_of[] = { "ublox", "battery", NULL };
    for (int k = 0; one_of[k]; k++) {
        sbuf_t b = { 0 };
        int n = 0;
        for (int i = 0; i < s->nentries; i++) {
            const entry_t *e = &s->entries[i];
            if (strcmp(e->device, one_of[k])) continue;
            sb_printf(&b, "%s%s (%s)", n++ ? ", " : "", pad_name_id(s, e->pad), e->src);
        }
        if (n > 1) err(s, "one %s per session: %s", one_of[k], b.p);
        free(b.p);
    }

    /* actuators: each on one output */
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        const fb_json_node_t *a = prop_get(e, "actuator");
        if (!fb_json_truthy(a)) continue;
        for (int j = 0; j < i; j++) {
            const entry_t *f = &s->entries[j];
            const fb_json_node_t *b = prop_get(f, "actuator");
            if (fb_json_truthy(b) && py_eq(s, a, b)) {
                err(s, "%s: actuator %s is already driven by %s on %s (%s)", e->src, S(s, a), f->device,
                    pad_name_id(s, f->pad), f->src);
                break;
            }
        }
    }
    if (s->errs.n > nerr0) return -1;

    /* what the machine builds; one receiver (the explicitly wired one first) */
    int have_rx = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s->nentries; i++) {
            entry_t *e = &s->entries[i];
            if (e->onboard != pass) continue;
            int cap_ = modelled_on(spec, e->device, e->port, e->pad);
            e->modelled = cap_;
            if (cap_ && !strcmp(e->device, "crsf")) {
                if (have_rx) {
                    e->modelled = 0;
                    note(s, "crsf on %s: the machine models one receiver (the wired one first), left out",
                         pad_name_id(s, e->pad));
                }
                have_rx = 1;
            } else if (!cap_) {
                note(s, "%s.%s on %s (%s): not modelled on this machine, left out", e->device, e->port,
                     pad_name_id(s, e->pad), e->onboard ? "onboard, listed" : "wired");
            }
            if (cap_ && !strcmp(e->device, "esc")) {
                const fb_json_node_t *t = prop_get(e, "telemetry");
                if (t && !fb_json_str_eq(t, "bidir")) {
                    note(s, "esc telemetry %s: only bidirectional DShot is modelled", R(s, t));
                }
            }
        }
    }
    return 0;
}

static const char *float_str(fb_session_t *s, double v)
{
    sbuf_t b = { 0 };
    py_float(&b, v, 0);
    return s_take(s, &b);
}

static const char *int_str(fb_session_t *s, const fb_json_node_t *v)
{
    sbuf_t b = { 0 };
    py_int(&b, v);
    return s_take(s, &b);
}

/* wiring.props_value: a wiring entry's props for -M <dev>-props=, "<id>=<value>:..." in the
 * descriptor's order, only the ones the entry gives; "" = none */
static const char *props_value(fb_session_t *s, const entry_t *e, const char *dev)
{
    sbuf_t b = { 0 };
    FOR_ITEMS(fb_json_get(device_by_name(s, dev), "props"), q) {
        const char *id = S(s, fb_json_get(q, "id"));
        const fb_json_node_t *v = prop_get(e, id), *t = fb_json_get(q, "type");
        if (!v) continue;
        sb_printf(&b, "%s%s=", b.len ? ":" : "", id);
        if (fb_json_str_eq(t, "bool")) sb_puts(&b, fb_json_truthy(v) ? "true" : "false");
        else if (fb_json_str_eq(t, "enum")) sb_puts(&b, S(s, v));
        else if (fb_json_str_eq(t, "int")) sb_puts(&b, int_str(s, v));
        else sb_puts(&b, float_str(s, num(v)));
    }
    if (!b.p) return "";
    return s_take(s, &b);
}

/* wiring.machine_options: the -M device options of the board's machine, dict order */
static void machine_options(fb_session_t *s)
{
    const board_spec_t *spec = s->board.spec;
    const char *fdm_ins[16], *throttle;
    int nins = roles(s, fdm_ins, &throttle);
    kvlist_t *o = &s->mopts;
    int imu = 0, esc = 0;
    const entry_t *baro = NULL;
    const char *rx = NULL, *osd = NULL;
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        if (!e->modelled) continue;
        imu |= !strcmp(e->device, "icm42688p");
        esc |= !strcmp(e->device, "esc");
        if (!baro && (!strcmp(e->device, "dps310") || !strcmp(e->device, "spl06"))) baro = e;
        if (!rx && !strcmp(e->device, "crsf")) rx = e->pad;
        if (!osd && !strcmp(e->device, "msp_osd")) osd = e->pad;
    }
    kv_set(s, o, "imu", imu ? "on" : "off");
    const fb_json_node_t *dg = NULL, *da = NULL;
    const fb_json_node_t *icm = device_by_name(s, "icm42688p");
    FOR_ITEMS(fb_json_get(icm, "props"), q) {
        if (fb_json_str_eq(fb_json_get(q, "id"), "gyro_noise_dps")) dg = fb_json_get(q, "default");
        if (fb_json_str_eq(fb_json_get(q, "id"), "accel_noise_g")) da = fb_json_get(q, "default");
    }
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        if (!e->modelled || strcmp(e->device, "icm42688p")) continue;
        const fb_json_node_t *g = prop_get(e, "gyro_noise_dps"), *a = prop_get(e, "accel_noise_g");
        if (g || a) {
            double gv = g ? num(g) : dg ? num(dg) : 0.05, av = a ? num(a) : da ? num(da) : 0.002;
            kv_set(s, o, "imu-noise", s_fmt(s, "%s:%s", float_str(s, gv), float_str(s, av)));
        }
        const fb_json_node_t *eo = prop_get(e, "accel_offset_mg"), *es = prop_get(e, "accel_scale_pct");
        if (eo || es) {
            kv_set(s, o, "imu-accel-error", s_fmt(s, "%s:%s", float_str(s, eo ? num(eo) : 0),
                                                  float_str(s, es ? num(es) : 0)));
        }
    }
    kv_set(s, o, "crsf", rx ? rx : "off");
    kv_set(s, o, "esc", esc ? "on" : "off");
    if (board_models(spec, "dps310") || board_models(spec, "spl06")) {
        kv_set(s, o, "baro", baro ? "on" : "off");
        if (baro && *props_value(s, baro, baro->device)) kv_set(s, o, "baro-props", props_value(s, baro, baro->device));
    }
    if (osd) kv_set(s, o, "osd", osd);     /* absent = off: a -M string without it is unchanged */
    static const struct { const char *dev, *key; int pad; } more[] = {
        { "qmc5883l", "mag", 0 }, { "ublox", "gps", 1 }, { "battery", "battery", 0 },
    };
    for (int k = 0; k < 3; k++) {
        if (!board_models(spec, more[k].dev)) continue;
        const entry_t *m = NULL;
        for (int i = 0; i < s->nentries && !m; i++) {
            if (s->entries[i].modelled && !strcmp(s->entries[i].device, more[k].dev)) m = &s->entries[i];
        }
        kv_set(s, o, more[k].key, !m ? "off" : more[k].pad ? m->pad : "on");
        if (m && *props_value(s, m, more[k].dev)) {
            kv_set(s, o, s_fmt(s, "%s-props", more[k].key), props_value(s, m, more[k].dev));
        }
    }
    const char *names[MAX_SERVOS] = { NULL };
    int nout = spec->nservo;
    unsigned rev = 0, mask = 0;
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        if (!e->modelled || strcmp(e->device, "servo")) continue;
        for (int k = 0; k < nout; k++) {
            if (strcmp(e->pad, spec->servo_pads[k])) continue;
            mask |= 1u << k;
            if (fb_json_truthy(prop_get(e, "actuator"))) names[k] = S(s, prop_get(e, "actuator"));
            if (fb_json_truthy(prop_get(e, "reversed"))) rev |= 1u << k;
        }
    }
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        if (!e->modelled || strcmp(e->device, "esc")) continue;
        const fb_json_node_t *ms = prop_get(e, "signal_timeout_ms");
        if (ms) kv_set(s, o, "esc-timeout-ms", int_str(s, ms));
    }
    /* a servo output the wiring does not name keeps the FDM input of its position unless another
     * output has it; the ESC's output is always the aircraft's throttle */
    for (int i = 0; i < nout; i++) {
        if (names[i]) continue;
        const char *dflt = i < nins ? fdm_ins[i] : NULL;
        int taken = 0;
        for (int k = 0; k < nout && dflt; k++) taken |= names[k] && !strcmp(names[k], dflt);
        names[i] = dflt && *dflt && !taken ? dflt : s_fmt(s, "servo%d", i + 1);
    }
    kv_set(s, o, "servos", s_fmt(s, "%u", mask));
    sbuf_t an = { 0 };
    for (int i = 0; i < nout; i++) sb_printf(&an, "%s:", names[i]);
    sb_puts(&an, throttle);
    kv_set(s, o, "actuators", s_take(s, &an));
    if (rev) kv_set(s, o, "servo-rev", s_fmt(s, "%u", rev));
}

/* wiring.device_config: the wired devices' CLI lines (`config`, then `config_firmware` of the board's
 * firmware family), "{serial}" = the UART pad's index */
static int device_config(fb_session_t *s)
{
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        if (!e->modelled) continue;
        const fb_json_node_t *d = device_by_name(s, e->device);
        const fb_json_node_t *cfgs[2] = { d ? fb_json_get(d, "config") : NULL, NULL };
        const fb_json_node_t *fw = d ? fb_json_get(d, "config_firmware") : NULL;
        if (fw && fw->type == FB_JSON_OBJECT) cfgs[1] = fb_json_get(fw, s->board.firmware_family);
        if (!fb_json_truthy(cfgs[0]) && !fb_json_truthy(cfgs[1])) continue;
        int serial = pad_by_id(s, e->pad)->serial;
        sl_add(&s->devcfg, s_fmt(s, "# %s on %s (device config)", e->device, e->pad));
        for (int k = 0; k < 2; k++) {
            const fb_json_node_t *cfg = cfgs[k];
            if (!fb_json_truthy(cfg)) continue;
            FOR_ITEMS(cfg, l) {
                const char *z = S(s, l);
                sbuf_t b = { 0 };
                for (const char *q = z; *q;) {
                    if (!strncmp(q, "{serial}", 8)) {
                        if (serial < 0) {
                            free(b.p);
                            err(s, "%s on %s: its config needs a serial index, the pad has none", e->device, e->pad);
                            return -1;
                        }
                        sb_printf(&b, "%d", serial);
                        q += 8;
                    } else {
                        sb_putc(&b, *q++);
                    }
                }
                sl_add(&s->devcfg, s_take(s, &b));
            }
        }
    }
    return 0;
}

/* ---- the session (session.py main) --------------------------------------------------------- */

static void opt(fb_session_t *s, const char *k, const char *v) { kv_set(s, &s->options, k, v); }

/* session "environment": what the baro sees -> "p0:h0:t0", "" when absent, NULL on an error */
static const char *environment(fb_session_t *s, const fb_json_node_t *env)
{
    static const struct { const char *k; int lo, hi; double d; } keys[] = {
        { "sea_level_pa", 30000, 120000, 101325 }, { "field_elevation_m", -500, 9000, 0 },
        { "temp_c", -60, 60, 22 },
    };
    if (!env || env->type == FB_JSON_NULL) return "";
    if (env->type != FB_JSON_OBJECT) {
        err(s, "environment: need an object {sea_level_pa, field_elevation_m, temp_c}");
        return NULL;
    }
    FOR_MEMBERS(env, k) {
        const char *kn = S(s, k);
        int ki = -1;
        for (int i = 0; i < 3; i++) {
            if (!strcmp(keys[i].k, kn)) ki = i;
        }
        if (ki < 0) {
            err(s, "environment.%s: unknown key (known: sea_level_pa, field_elevation_m, temp_c)", kn);
            return NULL;
        }
        const fb_json_node_t *v = k + 1;
        if (v->type != FB_JSON_NUMBER || !(num(v) >= keys[ki].lo && num(v) <= keys[ki].hi)) {
            err(s, "environment.%s %s: need a number %d..%d", kn, R(s, v), keys[ki].lo, keys[ki].hi);
            return NULL;
        }
    }
    sbuf_t b = { 0 };
    for (int i = 0; i < 3; i++) {
        const fb_json_node_t *v = fb_json_get(env, keys[i].k);
        if (i) sb_putc(&b, ':');
        py_float(&b, v ? num(v) : keys[i].d, 0);
    }
    return s_take(s, &b);
}

/* resource BEEPER 1 <pin> / set beeper_inversion = ON (last one wins), case-insensitive */
static int ci_lit(const char **p, const char *e, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(e - *p) < n) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = (*p)[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lit[i]) return 0;
    }
    *p += n;
    return 1;
}

static int sp_run(const char **p, const char *e)
{
    const char *q = *p;
    while (q < e && is_sp(*q)) q++;
    int any = q > *p;
    *p = q;
    return any;
}

static void beeper_from(fb_session_t *s, const char *text, size_t n, const char **pin_out, int *inv)
{
    int nl;
    line_t *lines = split_lines(s, text, n, &nl);
    *pin_out = NULL;
    *inv = 0;
    for (int i = 0; i < nl; i++) {
        const char *e = lines[i].p + lines[i].n, *q = lines[i].p, *v;
        size_t vn;
        sp_run(&q, e);
        if (ci_lit(&q, e, "resource") && sp_run(&q, e) && ci_lit(&q, e, "beeper") && sp_run(&q, e) &&
            ci_lit(&q, e, "1") && sp_run(&q, e) && m_run(&q, e, not_sp, &v, &vn)) {
            char *u = upper(s, v, vn);
            *pin_out = strcmp(u, "NONE") ? u : NULL;
        }
        q = lines[i].p;
        sp_run(&q, e);
        if (ci_lit(&q, e, "set") && sp_run(&q, e) && ci_lit(&q, e, "beeper_inversion")) {
            sp_run(&q, e);
            if (m_lit(&q, e, "=")) {
                sp_run(&q, e);
                if (m_run(&q, e, not_sp, &v, &vn)) *inv = !strcmp(upper(s, v, vn), "ON");
            }
        }
    }
}

/* a path value of the bench: a string (or absent/null/""/false: none); -1 on another type */
static int path_value(fb_session_t *s, const char *key, const fb_json_node_t *v, const char **out)
{
    if (!fb_json_truthy(v)) {
        *out = "";
        return 0;
    }
    if (v->type != FB_JSON_STRING) {
        err(s, "%s %s: need a path", key, R(s, v));
        return -1;
    }
    *out = bench_path(s, S(s, v));
    return 0;
}

static int build_blob(fb_session_t *s);
static void blob_provenance(fb_session_t *s);
static int build_config_cli(fb_session_t *s);
static int build_manifest(fb_session_t *s);

/* "nexus-xr, speedybee-f405-wing" */
static const char *board_list(fb_session_t *s)
{
    sbuf_t b = { 0 };
    for (int i = 0; i < NBOARDS; i++) sb_printf(&b, "%s%s", i ? ", " : "", BOARDS[i].id);
    return s_take(s, &b);
}

static const char *strip_cr(fb_session_t *s, const char *z)
{
    sbuf_t b = { 0 };
    for (; *z; z++) {
        if (*z != '\r') sb_putc(&b, *z);
    }
    return s_take(s, &b);
}

static int is_blank(const char *z)
{
    for (; *z; z++) {
        if (!is_sp(*z)) return 0;
    }
    return 1;
}

static void run(fb_session_t *s)
{
    const fb_json_node_t *b = s->bench, *v;
    if (b->type != FB_JSON_OBJECT) {
        err(s, "bench: need a JSON object");
        return;
    }
    v = fb_json_get(b, "board");
    const board_spec_t *spec = v ? (v->type == FB_JSON_STRING ? board_spec(S(s, v)) : NULL) : DEFAULT_BOARD;
    if (!spec) {
        err(s, "board %s: not emulated (boards: %s)", R(s, v), board_list(s));
        return;
    }
    s->board_id = spec->id;
    s->board.spec = spec;
    s->board.machine = spec->id;
    if (path_value(s, "firmware", fb_json_get(b, "firmware"), &s->firmware) < 0) return;
    if (!*s->firmware) {
        err(s, "no firmware");
        return;
    }
    const fb_json_node_t *bc = fb_json_get(b, "board_config");
    if (!bc) s->board_config = spec->default_config ? bench_path(s, spec->default_config) : "";
    else if (path_value(s, "board_config", bc, &s->board_config) < 0) return;
    v = fb_json_get(b, "profile");
    if (v && v->type == FB_JSON_ARRAY) {
        FOR_ITEMS(v, p) {
            const char *pp;
            if (path_value(s, "profile", p, &pp) < 0) return;
            if (*pp) sl_add(&s->profiles, pp);
        }
    } else {
        const char *pp;
        if (path_value(s, "profile", v, &pp) < 0) return;
        if (*pp) sl_add(&s->profiles, pp);
    }
    v = fb_json_get(b, "profile_text");
    if (v && v->type != FB_JSON_STRING) {
        err(s, "profile_text %s: need a string", R(s, v));
        return;
    }
    if (v && !is_blank(S(s, v))) s->profile_text = s_fmt(s, "%s\n", strip_cr(s, S(s, v)));

    /* assembly(): board, aircraft, wiring */
    if (load_board(s, &s->board, spec, s->board_config, s->board_json, 1) < 0) return;
    s->pads = s->board.pads;
    s->npads = s->board.npads;
    int cli = s->board.cli;
    v = fb_json_get(b, "aircraft");
    if (v && v->type != FB_JSON_NULL && v->type != FB_JSON_STRING) {
        err(s, "aircraft %s: need a path or a model id", R(s, v));
        return;
    }
    s->aircraft_path = aircraft_path(s, jstr(s, v));
    if (!exists(s->aircraft_path)) {
        err(s, "aircraft %s: no %s", R(s, v), s->aircraft_path);
        return;
    }
    s->ac = load_aircraft(s, s->aircraft_path, &s->ac_id);
    if (!s->ac) return;
    const fb_json_node_t *w = fb_json_get(b, "wiring");
    if (!fb_json_truthy(w) && spec->legacy) {
        note(s, "no wiring: the machine's defaults (the RS7 layout: gyro, baro on i2c3, CRSF on uart5, ESC on "
             "motor1, servo1..4)");
    } else {
        if (!fb_json_truthy(w)) {
            /* a board without legacy defaults: its onboard devices, nothing wired */
            note(s, "no wiring: %s with its onboard devices only (no receiver, no ESC, no servos)", spec->id);
            char why[64];
            w = parse_doc(s, "[]", 2, why, sizeof why);
        }
        s->have_wiring = 1;
        if (resolve(s, w) < 0) return;
        machine_options(s);
    }
    if (cli && !bc) s->board_config = s->board.defaults_cli ? s->board.defaults_cli : "";
    /* the bench's own lines (a file the app writes) and the lines it disables: a CLI board's script */
    if (path_value(s, "bench_cli", fb_json_get(b, "bench_cli"), &s->bench_cli) < 0) return;
    v = fb_json_get(b, "disabled_lines");
    if (v && v->type != FB_JSON_NULL) {
        if (v->type != FB_JSON_ARRAY) {
            err(s, "disabled_lines %s: need a list of line texts or layer:index refs", R(s, v));
            return;
        }
        FOR_ITEMS(v, e) {
            if (e->type != FB_JSON_STRING) {
                err(s, "disabled_lines %s: need a string (a line's text or layer:index)", R(s, e));
                return;
            }
        }
        if (v->count) s->disabled = v;
    }
    if (!cli && (*s->bench_cli || s->disabled)) {
        err(s, "%s: only on a CLI board (config: cli); %s takes its lines in the custom defaults (profile, "
             "profile_text)", *s->bench_cli ? "bench_cli" : "disabled_lines", spec->id);
        return;
    }

    opt(s, "aircraft", s->ac_id);
    if (fdm_on(s, s->ac, spec)) {
        opt(s, "fdm", "on");
        opt(s, "aircraft-file", s->aircraft_path);
    } else if (has_fdm(s, s->ac)) {
        note(s, "aircraft %s: no FDM for it on %s, the IMU sees a board at rest", Rs(s, s->ac_id), spec->id);
    } else {
        note(s, "aircraft %s: no FDM for it, the IMU sees a board at rest", Rs(s, s->ac_id));
    }
    if (s->have_wiring && device_config(s) < 0) return;
    static const char *const dev_keys[] = { "imu", "imu-noise", "imu-accel-error", "crsf", "esc", "esc-timeout-ms", "baro",
                                            "baro-props", "servos", "osd", "mag", "mag-props", "gps", "gps-props",
                                            "battery", "battery-props", "actuators", "servo-rev", NULL };
    for (int i = 0; dev_keys[i]; i++) {
        const char *mv = kv_get(&s->mopts, dev_keys[i]);
        if (mv) opt(s, dev_keys[i], mv);
    }
    const char *env = environment(s, fb_json_get(b, "environment"));
    if (!env) return;
    if (*env && (board_models(spec, "dps310") || board_models(spec, "spl06"))) opt(s, "baro-env", env);
    else if (*env) note(s, "environment: %s has no modelled baro, not applied", spec->id);

    if (cli) {
        /* a CLI board: the beeper from board.json */
        if (s->board.beeper) opt(s, "beeper", s->board.beeper);
        else note(s, "no beeper in board.json: `beep` events only from DShot beacons (motor_beep)");
    } else {
    /* the beeper pin from the board config and the profiles (the files that exist) */
    sbuf_t text = { 0 };
    const char *files[64];
    int nf = 0;
    if (*s->board_config) files[nf++] = s->board_config;
    for (int i = 0; i < s->profiles.n && nf < 62; i++) files[nf++] = s->profiles.v[i];
    for (int i = 0; i < nf; i++) {
        size_t n;
        const char *why;
        const char *t = exists(files[i]) ? read_file(s, files[i], &n, &why) : NULL;
        if (t) {
            sb_add(&text, t, n);
            sb_putc(&text, '\n');
        }
    }
    if (s->profile_text) {
        sb_puts(&text, s->profile_text);
        sb_putc(&text, '\n');
    }
    if (s->devcfg.n) {
        for (int i = 0; i < s->devcfg.n; i++) sb_printf(&text, "%s\n", s->devcfg.v[i]);
        sb_putc(&text, '\n');
    }
    const char *bpin;
    int inv;
    beeper_from(s, text.p ? text.p : "", text.len, &bpin, &inv);
    free(text.p);
    if (bpin) opt(s, "beeper", s_fmt(s, "%s%s", bpin, inv ? ":inv" : ""));
    else note(s, "no resource BEEPER in the board config: `beep` events only from DShot beacons (motor_beep)");
    }

    /* time */
    const fb_json_node_t *t = fb_json_get(b, "time");
    if (t && t->type != FB_JSON_OBJECT) {
        err(s, "time %s: need an object", R(s, t));
        return;
    }
    const fb_json_node_t *sh = fb_json_get(t, "icount_shift");
    s->shift = 2;
    if (sh && sh->type == FB_JSON_NULL) {
        s->shift = -1;
    } else if (sh) {
        if (!is_int(sh) || num(sh) < 0 || num(sh) > 10) {
            err(s, "time.icount_shift %s: need an integer 0..10 or null", R(s, sh));
            return;
        }
        s->shift = (int)num(sh);
    }
    if (fb_json_truthy(fb_json_get(t, "realtime"))) {
        s->realtime = 1;
        opt(s, "realtime", "on");
    }
    const fb_json_node_t *skip = fb_json_get(t, "idle_skip");
    if (skip && !is_bool(skip)) {
        err(s, "time.idle_skip %s: need true|false", R(s, skip));
        return;
    }
    if (skip && skip->type == FB_JSON_TRUE) {
        if (s->shift < 0) {
            err(s, "time.idle_skip needs time.icount_shift (virtual time = instruction count)");
            return;
        }
        opt(s, "idle-skip", "on");
        static const struct { const char *k, *o; int lo, hi; } ik[] = {
            { "idle_skip_n", "idle-skip-n", 2, 1000 }, { "idle_skip_gran_us", "idle-skip-gran-us", 1, 100000 },
        };
        for (int i = 0; i < 2; i++) {
            const fb_json_node_t *iv = fb_json_get(t, ik[i].k);
            if (!iv) continue;
            if (!is_int(iv) || num(iv) < ik[i].lo || num(iv) > ik[i].hi) {
                err(s, "time.%s %s: need an integer %d..%d", ik[i].k, R(s, iv), ik[i].lo, ik[i].hi);
                return;
            }
            opt(s, ik[i].o, int_str(s, iv));
        }
    }
    /* ports */
    const fb_json_node_t *ports = fb_json_get(b, "ports");
    static const struct { const char *k; int d; } pk[] = { { "control", 31250 }, { "msp", 31235 } };
    for (int i = 0; i < 2; i++) {
        const fb_json_node_t *pv = ports && ports->type == FB_JSON_OBJECT ? fb_json_get(ports, pk[i].k) : NULL;
        int port = pk[i].d;
        if (pv) {
            if (!is_int(pv) || num(pv) < 1 || num(pv) > 65535) {
                err(s, "ports.%s %s: need a TCP port 1..65535", pk[i].k, R(s, pv));
                return;
            }
            port = (int)num(pv);
        }
        if (i) s->port_msp = port;
        else s->port_ctl = port;
    }
    sbuf_t os = { 0 };
    for (int i = 0; i < s->options.n; i++) {
        sb_printf(&os, "%s%s=", i ? "," : "", s->options.v[i].k);
        for (const char *q = s->options.v[i].v; *q; q++) {
            sb_putc(&os, *q);
            if (*q == ',') sb_putc(&os, ',');       /* -M splits at commas: a literal one is ",," */
        }
    }
    s->options_str = s_take(s, &os);

    if ((cli ? build_config_cli(s) : build_blob(s)) < 0) return;
    if (!cli) blob_provenance(s);
    build_manifest(s);
}

/* ---- the custom-defaults blob (mkdefaults.sh) ------------------------------------------------- */

/* grep -v -E '^[[:space:]]*(#|$)' over a profile (CRs removed): the kept lines, each + "\n" */
static void profile_lines(sbuf_t *b, const char *t, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && t[j] != '\n') j++;
        size_t k = i;
        while (k < j && (t[k] == ' ' || t[k] == '\t' || t[k] == '\v' || t[k] == '\f' || t[k] == '\r')) k++;
        if (k < j && t[k] != '#') {
            for (size_t q = i; q < j; q++) {
                if (t[q] != '\r') sb_putc(b, t[q]);
            }
            sb_putc(b, '\n');
        }
        i = j + 1;
    }
}

static int build_blob(fb_session_t *s)
{
    if (!*s->board_config) return 0;
    size_t n;
    const char *why;
    const char *cfg = read_file(s, s->board_config, &n, &why);
    if (!cfg) {
        err(s, "board config %s: %s", s->board_config, why);
        return -1;
    }
    sbuf_t *b = &s->blob;
    sb_puts(b, DEFAULTS_HEADER);
    for (size_t i = 0; i < n; i++) {
        if (cfg[i] != '\r') sb_putc(b, cfg[i]);
    }
    for (int i = 0; i < s->profiles.n; i++) {
        const char *p = read_file(s, s->profiles.v[i], &n, &why);
        if (!p) {
            err(s, "profile %s: %s", s->profiles.v[i], why);
            return -1;
        }
        profile_lines(b, p, n);
    }
    if (s->profile_text) profile_lines(b, s->profile_text, strlen(s->profile_text));
    for (int i = 0; i < s->devcfg.n; i++) {
        profile_lines(b, s->devcfg.v[i], strlen(s->devcfg.v[i]));
    }
    if (b->len >= FB_SESSION_DEFAULTS_MAX) {
        err(s, "defaults too big: %zu >= %d", b->len, FB_SESSION_DEFAULTS_MAX);
        return -1;
    }
    s->have_blob = 1;
    return 0;
}

/* ---- a CLI board's script (session.py config_cli) --------------------------------------------- */

/* a path for the report: relative to the data root when under it (session.py rel_path) */
static const char *rel_path(fb_session_t *s, const char *p)
{
    size_t lr = strlen(s->root);
    if (!lr || strncmp(p, s->root, lr)) return p;
    if (s->root[lr - 1] == '/') return p + lr;
    return p[lr] == '/' ? p + lr + 1 : p;
}

static void cl_add(fb_session_t *s, const char *layer, const char *source, const char *t, size_t n)
{
    if (s->ncl == s->capcl) {
        s->capcl = s->capcl ? s->capcl * 2 : 64;
        cline_t *v = s_alloc(s, sizeof *v * (size_t)s->capcl);
        if (s->ncl) memcpy(v, s->cl, sizeof *v * (size_t)s->ncl);
        s->cl = v;
    }
    int idx = 1;
    for (int i = 0; i < s->ncl; i++) idx += !strcmp(s->cl[i].layer, layer);
    s->cl[s->ncl++] = (cline_t) { s_dup(s, t, n), layer, source, NULL, n, idx };
}

/* session._cli_lines: split at "\n", strip ASCII whitespace, drop blank and "#" lines; by the first
 * word (ASCII lower case): save/exit dropped with a note, the reset/wipe commands an error. The
 * kept lines go to s->cl with their layer and source: file = "<file>:<line>" (the file shown
 * relative to the data root), else lsrc[line - 1] */
static void cli_lines(fb_session_t *s, const char *src, const char *t, size_t n, const char *layer,
                      const char *file, const char *const *lsrc)
{
    size_t i = 0;
    for (int ln = 1;; ln++) {
        size_t j = i;
        while (j < n && t[j] != '\n') j++;
        size_t a = i, e = j;
        while (a < e && is_sp(t[a])) a++;
        while (e > a && is_sp(t[e - 1])) e--;
        if (a < e && t[a] != '#') {
            size_t w = a;
            while (w < e && !is_sp(t[w])) w++;
            const char *word = lower(s, t + a, w - a);
            int plain = strlen(word) == w - a;          /* no NUL byte in it */
            if (plain && in_strs(CLI_DROP, word)) {
                note(s, "config_cli: %s line %d: '%s' dropped (the core saves at the end)", src, ln, word);
            } else if (plain && in_strs(CLI_FORBID, word)) {
                err(s, "config_cli: %s line %d: '%s' would reset or wipe the board mid-script, not allowed", src, ln,
                    word);
            } else {
                cl_add(s, layer, file ? s_fmt(s, "%s:%d", rel_path(s, file), ln) : lsrc[ln - 1], t + a, e - a);
            }
        }
        if (j >= n) break;
        i = j + 1;
    }
}

/* the device lines' sources: "device <name> on <pad>" from each block's "# <name> on <pad> (device
 * config)" line (wiring.device_config writes it first), NULL for that line itself */
static const char **device_sources(fb_session_t *s)
{
    const char **v = s_alloc(s, sizeof *v * (size_t)(s->devcfg.n + 1));
    const char *cur = "device";
    static const char tail[] = " (device config)";
    for (int i = 0; i < s->devcfg.n; i++) {
        const char *l = s->devcfg.v[i];
        size_t n = strlen(l), nt = sizeof tail - 1;
        if (n > 2 + nt && !strncmp(l, "# ", 2) && !strcmp(l + n - nt, tail)) {
            cur = s_fmt(s, "device %.*s", (int)(n - 2 - nt), l + 2);
            v[i] = NULL;
        } else {
            v[i] = cur;
        }
    }
    v[s->devcfg.n] = NULL;
    return v;
}

static const char *const LAYERS[] = { "board", "aircraft", "profile", "device", "bench", NULL };

/* disabled_lines: each entry a line's exact text or a ref "layer:index"; the lines it matches are
 * not sent (reported, a note each); an entry that matches nothing is a note */
static void apply_disabled(fb_session_t *s)
{
    if (!s->disabled) return;
    FOR_ITEMS(s->disabled, v) {
        const char *d = S(s, v);
        const char *colon = strchr(d, ':');
        int ref = 0, idx = 0;
        const char *layer = NULL;
        if (colon && colon[1]) {
            const char *q = colon + 1;
            while (*q >= '0' && *q <= '9') q++;
            for (int k = 0; LAYERS[k] && !*q; k++) {
                if (strlen(LAYERS[k]) == (size_t)(colon - d) && !strncmp(d, LAYERS[k], (size_t)(colon - d))) {
                    ref = 1;
                    layer = LAYERS[k];
                    idx = atoi(colon + 1);
                }
            }
        }
        const char *text = d;
        if (!ref) {
            size_t a = 0, e = strlen(d);
            while (a < e && is_sp(d[a])) a++;
            while (e > a && is_sp(d[e - 1])) e--;
            text = s_dup(s, d + a, e - a);
        }
        int hit = 0;
        for (int i = 0; i < s->ncl; i++) {
            cline_t *c = &s->cl[i];
            if (ref ? strcmp(c->layer, layer) || c->idx != idx : strlen(c->text) != c->n || strcmp(c->text, text)) continue;
            hit++;
            if (c->by) continue;
            c->by = d;
            note(s, "config: disabled_lines %s: [%s:%s] %s not sent", Rs(s, d), c->layer, c->source, c->text);
        }
        if (!hit) note(s, "config: disabled_lines %s: matches no line of the script", Rs(s, d));
    }
}

/* the same setting in two layers with different values: the later line wins (cli_norm.h) */
static void find_conflicts(fb_session_t *s)
{
    fb_cli_line_t *l = s_alloc(s, sizeof *l * (size_t)(s->ncl + 1));
    int *map = s_alloc(s, sizeof *map * (size_t)(s->ncl + 1));
    int n = 0;
    for (int i = 0; i < s->ncl; i++) {
        if (s->cl[i].by) continue;
        map[n] = i;
        l[n++] = (fb_cli_line_t) { s->cl[i].text, s->cl[i].layer, s->cl[i].source };
    }
    fb_cli_conflict_t *c;
    int nc = fb_cli_conflicts(l, n, &c);
    for (int k = 0; k < nc; k++) {
        for (int j = 0; j < c[k].n; j++) c[k].lines[j] = map[c[k].lines[j]];
        sbuf_t b = { 0 };
        for (int j = 0; j < c[k].n; j++) {
            const cline_t *x = &s->cl[c[k].lines[j]];
            sb_printf(&b, "%s[%s:%s] %s", j ? ", then " : "", x->layer, x->source, x->text);
        }
        note(s, "config: conflict: %s: %s: the later line wins", c[k].key, b.p);
        free(b.p);
    }
    s->conf = c;
    s->nconf = nc;
}

/* the base script, the aircraft's firmware_cli, the profiles, profile_text, the wired devices' lines,
 * the bench's lines; then disabled_lines and the conflicts */
static int build_config_cli(fb_session_t *s)
{
    int n0 = s->errs.n;
    const char *fc = jstr(s, fb_json_get(s->ac, "firmware_cli"));
    const char **what = s_alloc(s, sizeof *what * (size_t)(s->profiles.n + 2));
    const char **files = s_alloc(s, sizeof *files * (size_t)(s->profiles.n + 2));
    const char **layers = s_alloc(s, sizeof *layers * (size_t)(s->profiles.n + 2));
    int nf = 0;
    if (*s->board_config) {
        what[nf] = "board config";
        layers[nf] = "board";
        files[nf++] = s->board_config;
    }
    if (fc && *fc) {
        what[nf] = "aircraft firmware_cli";
        layers[nf] = "aircraft";
        files[nf++] = bench_path(s, fc);
    }
    for (int i = 0; i < s->profiles.n; i++) {
        what[nf] = "profile";
        layers[nf] = "profile";
        files[nf++] = s->profiles.v[i];
    }
    for (int i = 0; i < nf; i++) {
        size_t n;
        const char *why;
        const char *t = read_file(s, files[i], &n, &why);
        if (!t) {
            err(s, "%s %s: %s", what[i], files[i], why);
            return -1;
        }
        sl_add(&s->layer_files, s_fmt(s, "%s\t%s", layers[i], files[i]));
        cli_lines(s, files[i], t, n, layers[i], files[i], NULL);
    }
    if (s->profile_text) {
        size_t n = strlen(s->profile_text);
        const char **ls = s_alloc(s, sizeof *ls * (n + 2));
        int k = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || s->profile_text[i] == '\n') ls[k] = s_fmt(s, "profile_text:%d", k + 1), k++;
        }
        cli_lines(s, "profile_text", s->profile_text, n, "profile", NULL, ls);
    }
    if (s->devcfg.n) {
        sbuf_t d = { 0 };
        for (int i = 0; i < s->devcfg.n; i++) sb_printf(&d, "%s\n", s->devcfg.v[i]);
        cli_lines(s, "device config", d.p, d.len, "device", NULL, device_sources(s));
        free(d.p);
    }
    if (*s->bench_cli) {
        size_t n;
        const char *why;
        const char *t = read_file(s, s->bench_cli, &n, &why);
        if (!t) {
            err(s, "bench_cli %s: %s", s->bench_cli, why);
            return -1;
        }
        sl_add(&s->layer_files, s_fmt(s, "bench\t%s", s->bench_cli));
        cli_lines(s, s->bench_cli, t, n, "bench", s->bench_cli, NULL);
    }
    apply_disabled(s);
    for (int i = 0; i < s->ncl; i++) {
        if (s->cl[i].by) continue;
        sb_add(&s->config_cli, s->cl[i].text, s->cl[i].n);
        sb_putc(&s->config_cli, '\n');
    }
    find_conflicts(s);
    return s->errs.n > n0 ? -1 : 0;
}

/* a blob board: the files of its custom defaults, and the device lines with their sources (the
 * lines the blob has: not blank, not "#") */
static void blob_provenance(fb_session_t *s)
{
    if (*s->board_config) sl_add(&s->layer_files, s_fmt(s, "board\t%s", s->board_config));
    for (int i = 0; i < s->profiles.n; i++) sl_add(&s->layer_files, s_fmt(s, "profile\t%s", s->profiles.v[i]));
    const char **src = device_sources(s);
    for (int i = 0; i < s->devcfg.n; i++) {
        const char *l = s->devcfg.v[i];
        size_t a = 0, e = strlen(l);
        while (a < e && is_sp(l[a])) a++;
        while (e > a && is_sp(l[e - 1])) e--;
        if (a < e && l[a] != '#') cl_add(s, "device", src[i] ? src[i] : "device", l + a, e - a);
    }
}

/* ---- the manifest (manifest.py) ------------------------------------------------------------ */

static void dump_pad(fb_session_t *s, sbuf_t *b, const pad_t *p)
{
    sb_puts(b, "{\"id\":");
    dump_zstr(b, p->id);
    sb_puts(b, ",\"label\":");
    if (p->label) dump_zstr(b, p->label);
    else sb_puts(b, "null");
    sb_puts(b, ",\"kind\":");
    dump_zstr(b, p->kind);
    sb_puts(b, ",\"flags\":[");
    for (int i = 0; i < p->flags.n; i++) {
        if (i) sb_putc(b, ',');
        dump_zstr(b, p->flags.v[i]);
    }
    sb_putc(b, ']');
    if (p->npins) {
        sb_puts(b, ",\"pins\":{");
        for (int i = 0; i < p->npins; i++) {
            if (i) sb_putc(b, ',');
            dump_zstr(b, p->pins[i].k);
            sb_putc(b, ':');
            dump_zstr(b, p->pins[i].v);
        }
        sb_putc(b, '}');
    }
    static const char *const keys[] = { "bus", "timer", "dma" };
    const char *vals[] = { p->bus, p->timer, p->dma };
    for (int i = 0; i < 3; i++) {
        if (!vals[i]) continue;
        sb_printf(b, ",\"%s\":", keys[i]);
        dump_zstr(b, vals[i]);
    }
    if (p->serial >= 0) sb_printf(b, ",\"serial\":%d", p->serial);
    sb_printf(b, ",\"onboard\":%s", p->onboard ? "true" : "false");
    if (p->onboard) {
        sb_puts(b, ",\"device\":");
        dump_zstr(b, p->device);
        sb_printf(b, ",\"detachable\":%s", p->detachable ? "true" : "false");
        if (p->part) {
            sb_puts(b, ",\"part\":");
            py_dump(s, b, p->part, 1);
        }
    }
    if (fb_json_truthy(p->hint)) {
        sb_puts(b, ",\"hint\":");
        py_dump(s, b, p->hint, 1);
    }
    if (fb_json_truthy(p->note)) {
        sb_puts(b, ",\"note\":");
        py_dump(s, b, p->note, 1);
    }
    if (p->ndevices) {
        sb_puts(b, ",\"devices\":[");
        for (int i = 0; i < p->ndevices; i++) {
            if (i) sb_putc(b, ',');
            dump_zstr(b, p->devices[i]);
        }
        sb_putc(b, ']');
    }
    sb_putc(b, '}');
}

/* an object's members without the closing brace */
static void dump_members(fb_session_t *s, sbuf_t *b, const fb_json_node_t *o)
{
    sb_putc(b, '{');
    int i = 0;
    FOR_MEMBERS(o, k) {
        if (i++) sb_putc(b, ',');
        py_dump(s, b, k, 1);
        sb_putc(b, ':');
        py_dump(s, b, k + 1, 1);
    }
}

static int build_manifest(fb_session_t *s)
{
    sbuf_t *b = &s->manifest;
    sb_puts(b, "{\"type\":\"manifest\",\"core\":\"flightbench-qemu\",\"version\":\"0.1\",\"boards\":[");
    for (int bi = 0; bi < NBOARDS; bi++) {
    /* the bench's board as loaded (its board config); the others with their own, silently */
    board_t other;
    const board_t *bd = &s->board;
    if (s->board.spec != &BOARDS[bi]) {
        if (load_board(s, &other, &BOARDS[bi], NULL, NULL, 0) < 0) return -1;
        bd = &other;
    }
    sb_puts(b, bi ? ",{\"id\":" : "{\"id\":");
    dump_zstr(b, bd->spec->id);
    sb_puts(b, ",\"name\":");
    if (bd->name) py_dump(s, b, bd->name, 1);
    else dump_zstr(b, bd->spec->id);
    sb_puts(b, ",\"mcu\":");
    if (bd->mcu) py_dump(s, b, bd->mcu, 1);
    else dump_zstr(b, bd->spec->mcu);
    sb_puts(b, ",\"machine\":");
    dump_zstr(b, bd->machine);
    sb_printf(b, ",\"config\":\"%s\",\"msp_uart\":", bd->cli ? "cli" : "unified");
    dump_zstr(b, bd->msp_uart);
    sb_puts(b, ",\"firmware_family\":");
    dump_zstr(b, bd->firmware_family);
    sb_puts(b, ",\"slots\":[");
    int n = 0;
    for (int g = 0; SLOT_GROUPS[g]; g++) {
        for (int i = 0; i < bd->npads; i++) {
            const pad_t *p = &bd->pads[i];
            if (!p->group || strcmp(p->group, SLOT_GROUPS[g])) continue;
            if (n++) sb_putc(b, ',');
            sb_puts(b, "{\"id\":");
            dump_zstr(b, p->id);
            sb_puts(b, ",\"kind\":");
            dump_zstr(b, p->kind);
            if (p->ndevices) {
                sb_puts(b, ",\"devices\":[");
                for (int k = 0; k < p->ndevices; k++) {
                    if (k) sb_putc(b, ',');
                    dump_zstr(b, p->devices[k]);
                }
                sb_putc(b, ']');
            }
            sb_putc(b, '}');
        }
    }
    sb_puts(b, "],\"pads\":[");
    for (int i = 0; i < bd->npads; i++) {
        if (i) sb_putc(b, ',');
        dump_pad(s, b, &bd->pads[i]);
    }
    sb_puts(b, "]}");
    }
    sb_puts(b, "],\"devices\":[");
    int n = 0;
    FOR_ITEMS(s->catalog, d) {
        if (n++) sb_putc(b, ',');
        dump_members(s, b, d);
        sb_printf(b, ",\"implemented\":%s}", device_modelled(S(s, fb_json_get(d, "id"))) ? "true" : "false");
    }
    sb_puts(b, "],\"aircraft\":[");
    strlist_t dirs = { 0 };
    const char *models = join(s, s->root, "models");
    list_dir(s, models, &dirs);
    n = 0;
    for (int i = 0; i < dirs.n; i++) {
        const char *p = s_fmt(s, "%s/%s/aircraft.json", models, dirs.v[i]);
        if (!exists(p)) continue;
        int n0 = s->errs.n;
        const char *id;
        const fb_json_node_t *a = load_aircraft(s, p, &id);
        if (!a) {
            /* one warning line per error (each gets its "session: " prefix, as the reference's
             * manifest.py stderr lines do); the last one says what happened */
            int ne = s->errs.n;
            s->errs.n = n0;
            for (int k = n0; k < ne; k++) {
                note(s, "%s%s", s->errs.v[k], k == ne - 1 ? " (left out of the manifest)" : "");
            }
            if (ne == n0) note(s, " (left out of the manifest)");
            continue;
        }
        if (n++) sb_putc(b, ',');
        dump_members(s, b, a);
        if (!fb_json_get(a, "id")) {
            sb_printf(b, "%s\"id\":", a->count ? "," : "");
            dump_zstr(b, id);
        }
        sb_printf(b, "%s\"implemented\":%s}", a->count || !fb_json_get(a, "id") ? "," : "",
                  has_fdm(s, a) ? "true" : "false");
    }
    free((void *)dirs.v);
    sb_puts(b, "]}\n");
    return 0;
}

/* ---- the report (session-check) ------------------------------------------------------------ */

static void json_kv(sbuf_t *b, const char *k, const char *v, size_t n, int first)
{
    if (!first) sb_putc(b, ',');
    dump_zstr(b, k);
    sb_putc(b, ':');
    py_json_str(b, v, n, 0);
}

static void json_line(fb_session_t *s, sbuf_t *b, const cline_t *c, int by)
{
    sb_putc(b, '{');
    json_kv(b, "text", c->text, c->n, 1);
    json_kv(b, "layer", c->layer, strlen(c->layer), 0);
    json_kv(b, "source", c->source, strlen(c->source), 0);
    const char *ref = s_fmt(s, "%s:%d", c->layer, c->idx);
    json_kv(b, "ref", ref, strlen(ref), 0);
    if (by) json_kv(b, "by", c->by, strlen(c->by), 0);
    sb_putc(b, '}');
}

/* the layered assembly as one line of JSON (docs/protocol.md "config_json") */
static void config_json(fb_session_t *s, sbuf_t *b)
{
    sb_printf(b, "{\"kind\":\"%s\",\"layers\":[", s->board.cli ? "cli" : "blob");
    for (int i = 0; i < s->layer_files.n; i++) {
        const char *lf = s->layer_files.v[i], *tab = strchr(lf, '\t');
        const char *layer = s_dup(s, lf, (size_t)(tab - lf));
        sb_puts(b, i ? ",{" : "{");
        json_kv(b, "layer", layer, strlen(layer), 1);
        json_kv(b, "file", rel_path(s, tab + 1), strlen(rel_path(s, tab + 1)), 0);
        json_kv(b, "path", tab + 1, strlen(tab + 1), 0);
        sb_putc(b, '}');
    }
    sb_puts(b, "],\"lines\":[");
    for (int i = 0, k = 0; i < s->ncl; i++) {
        if (s->cl[i].by) continue;
        if (k++) sb_putc(b, ',');
        json_line(s, b, &s->cl[i], 0);
    }
    sb_puts(b, "],\"disabled\":[");
    for (int i = 0, k = 0; i < s->ncl; i++) {
        if (!s->cl[i].by) continue;
        if (k++) sb_putc(b, ',');
        json_line(s, b, &s->cl[i], 1);
    }
    sb_puts(b, "],\"conflicts\":[");
    for (int i = 0; i < s->nconf; i++) {
        const fb_cli_conflict_t *c = &s->conf[i];
        sb_puts(b, i ? ",{" : "{");
        json_kv(b, "setting", c->key, strlen(c->key), 1);
        sb_puts(b, ",\"lines\":[");
        for (int j = 0; j < c->n; j++) {
            if (j) sb_putc(b, ',');
            json_line(s, b, &s->cl[c->lines[j]], 0);
        }
        const cline_t *w = &s->cl[c->lines[c->n - 1]];
        const char *ref = s_fmt(s, "%s:%d", w->layer, w->idx);
        sb_puts(b, "]");
        json_kv(b, "wins", ref, strlen(ref), 0);
        sb_putc(b, '}');
    }
    sb_puts(b, "]}");
}

static void report(fb_session_t *s)
{
    sbuf_t *r = &s->report;
    for (int i = 0; i < s->nentries; i++) {
        const entry_t *e = &s->entries[i];
        const pad_t *p = pad_by_id(s, e->pad);
        const char *dp = s_fmt(s, "%s.%s", e->device, e->port);
        const char *lab = p->label && *p->label ? s_fmt(s, "(%s)", p->label) : "";
        sb_printf(r, "%-16s -> %-7s %-10s%s%s", dp, p->id, lab, e->onboard ? " onboard" : "",
                  e->modelled ? " modelled" : " left out");
        if (e->nprops) {
            sb_puts(r, " {");
            for (int k = 0; k < e->nprops; k++) {
                if (k) sb_puts(r, ", ");
                py_json_str(r, e->props[k].k, strlen(e->props[k].k), 1);
                sb_puts(r, ": ");
                py_dump(s, r, e->props[k].v, 0);
            }
            sb_putc(r, '}');
        }
        sb_putc(r, '\n');
    }
    sb_puts(r, "machine: ");
    for (int i = 0; i < s->mopts.n; i++) sb_printf(r, "%s%s=%s", i ? "," : "", s->mopts.v[i].k, s->mopts.v[i].v);
    sb_putc(r, '\n');
    for (int i = 0; i < s->devcfg.n; i++) sb_printf(r, "config: %s\n", s->devcfg.v[i]);
    sb_printf(r, "root: %s\n", s->root);
    sb_printf(r, "firmware: %s\n", s->firmware);
    sb_printf(r, "board: %s\n", s->board_id);
    sb_printf(r, "board_machine: %s\n", s->board.machine);
    sb_printf(r, "board_config: %s\n", s->board_config);
    sb_printf(r, "aircraft: %s %s\n", s->ac_id, s->aircraft_path);
    for (int i = 0; i < s->profiles.n; i++) sb_printf(r, "profile: %s\n", s->profiles.v[i]);
    if (s->profile_text) sb_printf(r, "profile_text: %zu bytes\n", strlen(s->profile_text));
    sb_printf(r, "options: %s\n", s->options_str);
    if (s->shift >= 0) sb_printf(r, "icount: shift=%d,sleep=off\n", s->shift);
    else sb_puts(r, "icount: none\n");
    sb_printf(r, "realtime: %s\n", s->realtime ? "on" : "off");
    sb_printf(r, "ports: control=%d msp=%d\n", s->port_ctl, s->port_msp);
    /* what a launcher needs, one line of space-separated key=value (docs/protocol.md) */
    if (s->shift >= 0) sb_printf(r, "launch: icount_shift=%d", s->shift);
    else sb_puts(r, "launch: icount_shift=none");
    sb_printf(r, " realtime=%s control=%d msp=%d machine=%s\n", s->realtime ? "on" : "off", s->port_ctl,
              s->port_msp, s->board.machine);
    sb_printf(r, "manifest: %s", s->manifest.p);
    /* a CLI board's script: the count, then each line after "config_cli| " */
    if (s->config_cli.len) {
        int nl = 0;
        for (size_t i = 0; i < s->config_cli.len; i++) nl += s->config_cli.p[i] == '\n';
        sb_printf(r, "config_cli: %d lines\n", nl);
        for (size_t i = 0; i < s->config_cli.len;) {
            const char *e = memchr(s->config_cli.p + i, '\n', s->config_cli.len - i);
            sb_puts(r, "config_cli| ");
            sb_add(r, s->config_cli.p + i, (size_t)(e - (s->config_cli.p + i)) + 1);
            i = (size_t)(e - s->config_cli.p) + 1;
        }
    } else {
        sb_puts(r, "config_cli: none\n");
    }
    /* the script's lines with their layer and source, the disabled ones, the conflicts, all as JSON */
    for (int i = 0; s->board.cli && i < s->ncl; i++) {
        const cline_t *c = &s->cl[i];
        sb_printf(r, "%s [%s:%s] ", c->by ? "config_disabled:" : "config_src:", c->layer, c->source);
        sb_add(r, c->text, c->n);
        if (c->by) {
            sb_puts(r, " (disabled_lines ");
            sb_puts(r, Rs(s, c->by));
            sb_putc(r, ')');
        }
        sb_putc(r, '\n');
    }
    for (int i = 0; i < s->nconf; i++) {
        const fb_cli_conflict_t *c = &s->conf[i];
        sb_printf(r, "config_conflict: %s:", c->key);
        for (int j = 0; j < c->n; j++) {
            const cline_t *x = &s->cl[c->lines[j]];
            sb_printf(r, "%s [%s:%s] ", j ? " ->" : "", x->layer, x->source);
            sb_add(r, x->text, x->n);
        }
        sb_puts(r, " (the last wins)\n");
    }
    sb_puts(r, "config_json: ");
    config_json(s, r);
    sb_putc(r, '\n');
    if (s->have_blob) {
        sb_printf(r, "defaults: %zu bytes (max %d)\n", s->blob.len, FB_SESSION_DEFAULTS_MAX);
        sb_add(r, s->blob.p, s->blob.len);
    } else {
        sb_puts(r, "defaults: none\n");
    }
}

/* ---- API ----------------------------------------------------------------------------------- */

static fb_session_t *session_new(void)
{
    fb_session_t *s = xmalloc(sizeof *s);
    *s = (fb_session_t) { .shift = 2, .port_ctl = 31250, .port_msp = 31235 };
    return s;
}

static void set_root(fb_session_t *s, const fb_session_opts_t *o, const char *board)
{
    const char *r = o && o->root && *o->root ? o->root : u8getenv(s, "FB_ROOT");
    if (r) {
        s->root = abs_path(s, r);
        return;
    }
    if (o && o->bench_path && *o->bench_path) {
        char *d = dirname_of(s, abs_path(s, o->bench_path));
        for (int i = 0; i < 64 && *d; i++) {
            if (exists(s_fmt(s, "%s%sboards/%s/board.json", d, d[strlen(d) - 1] == '/' ? "" : "/", board))) {
                s->root = d;
                return;
            }
            char *up = dirname_of(s, d);
            if (!strcmp(up, d)) break;
            d = up;
        }
    }
    s->root = abs_path(s, ".");
}

fb_session_t *fb_session_resolve(const char *text, size_t n, const fb_session_opts_t *o)
{
    fb_session_t *s = session_new();
    char why[128];
    s->bench = parse_doc(s, s_dup(s, text, n), n, why, sizeof why);
    const char *board = s->bench ? jstr(s, fb_json_get(s->bench, "board")) : NULL;
    set_root(s, o, board && *board ? board : DEFAULT_BOARD->id);
    if (!s->bench) {
        err(s, "bench%s%s: %s", o && o->bench_path ? " " : "", o && o->bench_path ? o->bench_path : "", why);
        return s;
    }
    if (load_catalog(s) < 0) return s;
    if (o && o->board_json && *o->board_json) s->board_json = s_strdup(s, o->board_json);
    run(s);
    if (!s->errs.n) report(s);
    return s;
}

fb_session_t *fb_session_load(const char *path, const fb_session_opts_t *o)
{
    fb_session_opts_t oo = o ? *o : (fb_session_opts_t) { 0 };
    oo.bench_path = path;
    fb_session_t *tmp = session_new();
    size_t n = 0;
    const char *why;
    char *text = read_file(tmp, path, &n, &why);
    if (!text) {
        set_root(tmp, &oo, DEFAULT_BOARD->id);
        err(tmp, "bench %s: %s", path, why);
        return tmp;
    }
    fb_session_t *s = fb_session_resolve(text, n, &oo);
    fb_session_free(tmp);
    return s;
}

fb_session_t *fb_session_manifest_only(const fb_session_opts_t *o)
{
    fb_session_t *s = session_new();
    set_root(s, o, DEFAULT_BOARD->id);
    s->board_id = DEFAULT_BOARD->id;
    s->board.spec = DEFAULT_BOARD;
    s->board.machine = DEFAULT_BOARD->id;
    if (load_catalog(s) < 0) return s;
    if (load_board(s, &s->board, DEFAULT_BOARD, NULL, o ? o->board_json : NULL, 0) < 0) return s;
    s->pads = s->board.pads;
    s->npads = s->board.npads;
    build_manifest(s);
    return s;
}

void fb_session_free(fb_session_t *s)
{
    if (!s) return;
    for (int i = 0; i < s->nallocs; i++) free(s->allocs[i]);
    free(s->allocs);
    free((void *)s->errs.v);
    free((void *)s->warns.v);
    free((void *)s->profiles.v);
    free((void *)s->devcfg.v);
    free(s->blob.p);
    free(s->manifest.p);
    free(s->report.p);
    free(s->config_cli.p);
    free((void *)s->layer_files.v);
    fb_cli_conflicts_free(s->conf, s->nconf);
    free(s->config_json);
    free(s);
}

int fb_session_ok(const fb_session_t *s) { return s->errs.n == 0; }
int fb_session_nerrors(const fb_session_t *s) { return s->errs.n; }
const char *fb_session_error(const fb_session_t *s, int i) { return i >= 0 && i < s->errs.n ? s->errs.v[i] : NULL; }
int fb_session_nwarnings(const fb_session_t *s) { return s->warns.n; }
const char *fb_session_warning(const fb_session_t *s, int i) { return i >= 0 && i < s->warns.n ? s->warns.v[i] : NULL; }
const char *fb_session_root(const fb_session_t *s) { return s->root; }
const char *fb_session_firmware(const fb_session_t *s) { return s->firmware; }
int fb_session_noptions(const fb_session_t *s) { return s->errs.n ? 0 : s->options.n; }
const char *fb_session_option_key(const fb_session_t *s, int i) { return s->options.v[i].k; }
const char *fb_session_option_value(const fb_session_t *s, int i) { return s->options.v[i].v; }
const char *fb_session_options_str(const fb_session_t *s) { return s->options_str; }
int fb_session_icount_shift(const fb_session_t *s) { return s->shift; }
int fb_session_realtime(const fb_session_t *s) { return s->realtime; }
int fb_session_port_control(const fb_session_t *s) { return s->port_ctl; }
int fb_session_port_msp(const fb_session_t *s) { return s->port_msp; }

const char *fb_session_defaults(const fb_session_t *s, size_t *len)
{
    if (s->errs.n || !s->have_blob) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->blob.len;
    return s->blob.p;
}

const char *fb_session_machine(const fb_session_t *s) { return s->board.machine; }
const char *fb_session_msp_uart(const fb_session_t *s) { return s->board.msp_uart; }

const char *fb_session_config_cli(const fb_session_t *s, size_t *len)
{
    if (s->errs.n || !s->config_cli.len) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->config_cli.len;
    return s->config_cli.p;
}

int fb_session_config_nlines(const fb_session_t *s)
{
    int n = 0;
    for (int i = 0; !s->errs.n && s->board.cli && i < s->ncl; i++) n += !s->cl[i].by;
    return n;
}

const char *fb_session_config_line(const fb_session_t *s, int i, const char **layer, const char **source)
{
    for (int k = 0; !s->errs.n && s->board.cli && k < s->ncl; k++) {
        if (s->cl[k].by || i--) continue;
        if (layer) *layer = s->cl[k].layer;
        if (source) *source = s->cl[k].source;
        return s->cl[k].text;
    }
    return NULL;
}

const char *fb_session_config_json(fb_session_t *s)
{
    if (s->errs.n) return NULL;
    if (!s->config_json) {
        sbuf_t b = { 0 };
        config_json(s, &b);
        s->config_json = b.p;
    }
    return s->config_json;
}

const char *fb_session_manifest(const fb_session_t *s)
{
    return s->errs.n || !s->manifest.p ? NULL : s->manifest.p;
}

const char *fb_session_report(const fb_session_t *s, size_t *len)
{
    if (s->errs.n || !s->report.p) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s->report.len;
    return s->report.p;
}
