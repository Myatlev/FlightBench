/*
 * FlightBench nexus-xr machine: the host-time ledger (-M nexus-xr,prof=on).
 *
 * Emulator-independent (plain C, no QEMU headers): included by nexus_xr.c and host-tested by
 * core/qemu/tests/test_prof.c. The includer defines FBP_TICKS() (a cheap cycle counter, in
 * QEMU cpu_get_host_ticks(): rdtsc) and FBP_MONO_NS() (a monotonic clock in ns, only used for
 * the one calibration at start) before including this file.
 *
 * A ledger is a fixed array of accounts {name, kind, count, host time, virtual ns, bytes},
 * registered at init (the only allocation); the hot path is fbp_enter()/fbp_leave() around a
 * piece of work: two tick reads, a push and a pop on a small per-thread stack. Nesting is
 * tracked, so each account has its inclusive time (host_ns) and its own time without the
 * accounts nested in it (self_ns), and per thread the time spent in top-level accounts:
 * wall - that = what no account covers (on the vCPU thread: guest code, TB lookup and
 * translation, QEMU's own bookkeeping).
 *
 * Threads are tagged explicitly (fbp_thread_set): the vCPU thread and the main loop thread.
 * Untagged threads are not accounted. Accounts are updated from both tagged threads; the
 * includer guarantees they never update the same account at the same time (in QEMU: under the
 * BQL, or accounts only one thread ever touches). The dump (fbp_json) and fbp_reset run on the
 * main loop thread under the BQL while the vCPU thread may be updating its out-of-BQL accounts
 * (vcpu.between_slices, vcpu.bql_wait, the hand-off reasons, the kick histogram): 64-bit aligned
 * loads and stores are not torn on the hosts we run (x86-64, aarch64), so a dump is a snapshot
 * that may be off by the event in flight, and a reset may miss one such update; frames open
 * across a reset keep their pre-reset start.
 *
 * Time source: FBP_TICKS() read on different cores/threads (a wake stamped by the vCPU thread,
 * read by the main loop) assumes a synchronized invariant TSC (constant_tsc/nonstop_tsc on x86);
 * a negative cross-thread difference is counted as 0.
 *
 * Latency histograms (fbp_hist): log-linear buckets, 16 per power of two: p50/p99 are within
 * 1/16 (6 %) of the true value; mean, min and max are exact.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#include <malloc.h>
#endif

enum {
    FBP_MMIO, FBP_TIMER, FBP_CHARDEV, FBP_CTL, FBP_FDM, FBP_HANDOFF, FBP_MAINLOOP, FBP_IDLE,
    FBP_OTHER, FBP_NKINDS
};
static const char *const fbp_kind_name[FBP_NKINDS] = {
    "mmio", "timer", "chardev", "ctl", "fdm", "handoff", "mainloop", "idle", "other",
};

enum { FBP_T_VCPU, FBP_T_MAIN, FBP_NTHREADS };
static const char *const fbp_thread_name[FBP_NTHREADS] = { "vcpu", "main" };

/* Why the vCPU left cpu_exec (one slice of the round-robin loop ended) */
enum {
    FBP_R_TIMER_DEADLINE,   /* budget used up by executed instructions: a virtual timer is due */
    FBP_R_IDLE_SKIP,        /* budget used up by an idle skip (or an idle-skip halt) */
    FBP_R_TIMER_REALTIME,   /* budget used up: a QEMU_CLOCK_REALTIME timer is due */
    FBP_R_TIMER_REARM,      /* the vCPU armed a virtual timer earlier than its budget end */
    FBP_R_IO,               /* another thread (main loop) kicked the vCPU out */
    FBP_R_HALT,             /* the guest halted (WFI/WFE) */
    FBP_R_OTHER,            /* none of the above (budget cap, exception exit, ...) */
    FBP_NREASONS
};
static const char *const fbp_reason_name[FBP_NREASONS] = {
    "timer_deadline", "idle_skip", "timer_deadline_realtime", "timer_rearm", "io", "halt",
    "other",
};

#define FBP_MAX_ACCTS   96
#define FBP_NAME_MAX    40
#define FBP_MAX_DEPTH   16
#define FBP_HIST_SUB    16                          /* buckets per power of two */
#define FBP_HIST_N      (FBP_HIST_SUB + 44 * FBP_HIST_SUB)  /* up to 2^48 ns */
#define FBP_NONE        0xFFFF

/* 64-byte aligned (accounts, per-thread fields, histograms): the vCPU thread and the main loop
 * thread write different ones at high rates; sharing a cache line would make every write a miss */
#define FBP_LINE __attribute__((aligned(64)))

typedef struct FBP_LINE FbpAcct {
    char name[FBP_NAME_MAX];
    uint8_t kind;
    uint8_t threads;            /* bit per thread that entered it */
    uint64_t count;
    uint64_t ticks;             /* inclusive */
    uint64_t child_ticks;       /* spent in accounts nested inside */
    uint64_t bytes;             /* chardev: bytes moved; main-loop poll: fds polled */
    int64_t virt_ns;            /* virtual time the account stands for (idle skips) */
    uint64_t deadlines;         /* timers: slices that ended at this timer's deadline */
    uint8_t sample;             /* > 1: only every sample-th event is timed (fbp_enter_sampled) */
    uint8_t sample_left;
} FbpAcct;

typedef struct FBP_LINE FbpHist {
    uint64_t n, sum, min, max;
    uint32_t b[FBP_HIST_N];
} FbpHist;

typedef struct FbProf {
    double ns_per_tick;
    uint64_t t0;                /* ticks at start / last reset */
    int64_t v0;                 /* virtual ns at start / last reset */
    unsigned n;
    FbpAcct a[FBP_MAX_ACCTS];
    struct FBP_LINE {
        uint64_t top_ticks;     /* time in top-level accounts of this thread */
    } thr[FBP_NTHREADS];
    FBP_LINE uint64_t reason[FBP_NREASONS];     /* vCPU thread only */
    uint64_t not_machine;       /* timer deadlines of timers the machine does not own */
    FbpHist wake;               /* vCPU signals the main loop -> its poll returns */
    FbpHist kick;               /* another thread kicks the vCPU -> the vCPU acts on it */
} FbProf;

typedef struct FbpFrame {
    uint16_t a;
    uint64_t t0, child;
} FbpFrame;

typedef struct FbpTls {
    int thread;                 /* FBP_T_*, -1 = not accounted */
    int depth;
    FbpFrame f[FBP_MAX_DEPTH];
} FbpTls;

static __thread FbpTls fbp_tls = { .thread = -1 };

static void fbp_thread_set(int thread)
{
    fbp_tls.thread = thread;
}

static inline int fbp_thread(void)
{
    return fbp_tls.thread;
}

static void fbp_calibrate(FbProf *p)
{
    int64_t m0 = FBP_MONO_NS();
    uint64_t c0 = FBP_TICKS();
    int64_t m1;
    uint64_t c1;
    do {
        m1 = FBP_MONO_NS();
        c1 = FBP_TICKS();
    } while (m1 - m0 < 20000000);           /* 20 ms */
    p->ns_per_tick = (double)(m1 - m0) / (double)(c1 - c0);
}

static void fbp_reset(FbProf *p, int64_t virt_now)
{
    for (unsigned i = 0; i < p->n; i++) {
        FbpAcct *a = &p->a[i];
        a->count = a->ticks = a->child_ticks = a->bytes = a->deadlines = 0;
        a->virt_ns = 0;
        a->threads = 0;
    }
    for (int t = 0; t < FBP_NTHREADS; t++) {
        p->thr[t].top_ticks = 0;
    }
    memset(p->reason, 0, sizeof(p->reason));
    p->not_machine = 0;
    memset(&p->wake, 0, sizeof(p->wake));
    memset(&p->kick, 0, sizeof(p->kick));
    p->t0 = FBP_TICKS();
    p->v0 = virt_now;
}

static FbProf *fbp_new(int64_t virt_now)
{
    /* never freed; mingw/UCRT have no aligned_alloc */
#ifdef _WIN32
    FbProf *p = _aligned_malloc((sizeof(*p) + 63) & ~(size_t)63, 64);
#else
    FbProf *p = aligned_alloc(64, (sizeof(*p) + 63) & ~(size_t)63);
#endif
    memset(p, 0, sizeof(*p));
    fbp_calibrate(p);
    fbp_reset(p, virt_now);
    return p;
}

/* Register an account (init time). The same name and kind returns the same account. */
static uint16_t fbp_acct(FbProf *p, const char *name, int kind)
{
    for (unsigned i = 0; i < p->n; i++) {
        if (p->a[i].kind == kind && !strcmp(p->a[i].name, name)) {
            return i;
        }
    }
    if (p->n >= FBP_MAX_ACCTS) {
        return FBP_NONE;
    }
    snprintf(p->a[p->n].name, FBP_NAME_MAX, "%s", name);
    p->a[p->n].kind = kind;
    return p->n++;
}

static inline void fbp_enter(FbProf *p, uint16_t a)
{
    FbpTls *t = &fbp_tls;
    (void)p;                    /* the frame knows its account; p for symmetry with leave */
    if (t->thread < 0 || a == FBP_NONE) {
        return;
    }
    if (t->depth < FBP_MAX_DEPTH) {
        FbpFrame *f = &t->f[t->depth];
        f->a = a;
        f->child = 0;
        f->t0 = FBP_TICKS();
    }
    t->depth++;
}

static inline void fbp_leave(FbProf *p, uint16_t a)
{
    FbpTls *t = &fbp_tls;
    if (t->thread < 0 || a == FBP_NONE || t->depth <= 0) {
        return;
    }
    if (--t->depth < FBP_MAX_DEPTH) {
        FbpFrame *f = &t->f[t->depth];
        uint64_t dt = FBP_TICKS() - f->t0;
        FbpAcct *ac = &p->a[f->a];
        if (ac->sample > 1) {
            dt *= ac->sample;
            f->child *= ac->sample;
        }
        ac->count++;
        ac->ticks += dt;
        ac->child_ticks += f->child;
        ac->threads |= 1u << t->thread;
        if (t->depth > 0) {
            t->f[t->depth - 1].child += dt;
        } else {
            p->thr[t->thread].top_ticks += dt;
        }
    }
}

/*
 * Hot leaf accounts (an MMIO region hit millions of times per second whose handler enters no
 * other account): every event is counted, every a->sample-th one is timed and its time counts
 * sample times (fbp_leave scales). Returns whether a frame was pushed (then call fbp_leave).
 */
static void fbp_set_sample(FbProf *p, uint16_t a, unsigned n)
{
    if (a != FBP_NONE) {
        p->a[a].sample = n > 1 && n < 256 ? n : 0;
        p->a[a].sample_left = 1;
    }
}

static inline bool fbp_enter_sampled(FbProf *p, uint16_t a)
{
    if (fbp_tls.thread < 0 || a == FBP_NONE) {
        return false;
    }
    FbpAcct *ac = &p->a[a];
    if (ac->sample > 1 && --ac->sample_left) {
        ac->count++;
        ac->threads |= 1u << fbp_tls.thread;
        return false;
    }
    ac->sample_left = ac->sample;
    fbp_enter(p, a);
    return true;
}

/* count an event without timing it (e.g. an idle skip, with the virtual time it skipped) */
static inline void fbp_count(FbProf *p, uint16_t a, int64_t virt_ns)
{
    if (a != FBP_NONE) {
        p->a[a].count++;
        p->a[a].virt_ns += virt_ns;
    }
}

static inline void fbp_bytes(FbProf *p, uint16_t a, uint64_t n)
{
    if (a != FBP_NONE) {
        p->a[a].bytes += n;
    }
}

/* ---- latency histograms ---- */

static unsigned fbp_hist_index(uint64_t v)
{
    if (v < FBP_HIST_SUB) {
        return v;
    }
    int e = 63 - __builtin_clzll(v);            /* >= 4 */
    unsigned idx = FBP_HIST_SUB + (e - 4) * FBP_HIST_SUB + ((v >> (e - 4)) & (FBP_HIST_SUB - 1));
    return idx < FBP_HIST_N ? idx : FBP_HIST_N - 1;
}

/* the middle of bucket idx */
static uint64_t fbp_hist_value(unsigned idx)
{
    if (idx < FBP_HIST_SUB) {
        return idx;
    }
    unsigned e = (idx - FBP_HIST_SUB) / FBP_HIST_SUB + 4;
    uint64_t sub = (idx - FBP_HIST_SUB) % FBP_HIST_SUB;
    uint64_t lo = (FBP_HIST_SUB + sub) << (e - 4);
    return lo + ((1ull << (e - 4)) >> 1);
}

static inline void fbp_hist_add(FbpHist *h, uint64_t v)
{
    if (!h->n || v < h->min) {
        h->min = v;
    }
    if (v > h->max) {
        h->max = v;
    }
    h->n++;
    h->sum += v;
    h->b[fbp_hist_index(v)]++;
}

/* q in (0, 1]: the value at or below which a fraction q of the samples lie */
static uint64_t fbp_hist_pct(const FbpHist *h, double q)
{
    if (!h->n) {
        return 0;
    }
    uint64_t need = (uint64_t)(q * h->n + 0.999999), acc = 0;
    if (need < 1) {
        need = 1;
    }
    for (unsigned i = 0; i < FBP_HIST_N; i++) {
        acc += h->b[i];
        if (acc >= need) {
            uint64_t v = fbp_hist_value(i);
            return v < h->min ? h->min : v > h->max ? h->max : v;
        }
    }
    return h->max;
}

/* ---- JSON ---- */

typedef struct FbpBuf {
    char *p;
    size_t len, cap;
} FbpBuf;

static void __attribute__((format(printf, 2, 3))) fbp_printf(FbpBuf *b, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? room : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if (b->p && (size_t)n < room) {
            b->len += n;
            return;
        }
        b->cap = b->cap * 2 + n + 256;
        b->p = realloc(b->p, b->cap);
    }
}

static void fbp_json_str(FbpBuf *b, const char *s)
{
    fbp_printf(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') {
            fbp_printf(b, "\\%c", c);
        } else if (c < 0x20) {
            fbp_printf(b, "\\u%04x", c);
        } else {
            fbp_printf(b, "%c", c);
        }
    }
    fbp_printf(b, "\"");
}

static uint64_t fbp_ns(const FbProf *p, uint64_t ticks)
{
    return (uint64_t)(ticks * p->ns_per_tick + 0.5);
}

static void fbp_hist_json(FbpBuf *b, const FbpHist *h)
{
    fbp_printf(b, "{\"count\":%llu,\"mean\":%llu,\"p50\":%llu,\"p99\":%llu,\"min\":%llu,"
               "\"max\":%llu}", (unsigned long long)h->n,
               (unsigned long long)(h->n ? h->sum / h->n : 0),
               (unsigned long long)fbp_hist_pct(h, 0.5), (unsigned long long)fbp_hist_pct(h, 0.99),
               (unsigned long long)h->min, (unsigned long long)h->max);
}

/*
 * The ledger as the body of a `prof` message (docs/protocol.md): `head` is inserted after
 * wall_ns/virt_ns (the includer's host/session objects, starting with a comma, or "").
 * Appends a complete JSON object with a trailing newline.
 */
static void fbp_json(const FbProf *p, FbpBuf *b, int64_t virt_now, const char *head)
{
    uint64_t now = FBP_TICKS();
    uint64_t wall = fbp_ns(p, now - p->t0);
    int im = -1, ip = -1;                   /* main loop accounts */
    uint64_t slices = 0;

    fbp_printf(b, "{\"type\":\"prof\",\"wall_ns\":%llu,\"virt_ns\":%lld%s,\"accounts\":[",
               (unsigned long long)wall, (long long)(virt_now - p->v0), head ? head : "");
    for (unsigned i = 0; i < p->n; i++) {
        const FbpAcct *a = &p->a[i];
        if (!strcmp(a->name, "mainloop.poll")) {
            ip = i;
        } else if (!strcmp(a->name, "mainloop.dispatch")) {
            im = i;
        }
        fbp_printf(b, "%s{\"name\":", i ? "," : "");
        fbp_json_str(b, a->name);
        fbp_printf(b, ",\"kind\":\"%s\",\"thread\":\"%s\",\"count\":%llu,\"host_ns\":%llu,"
                   "\"self_ns\":%llu,\"virt_ns\":%lld", fbp_kind_name[a->kind],
                   a->threads == 1 ? "vcpu" : a->threads == 2 ? "main" : a->threads ? "both" : "",
                   (unsigned long long)a->count, (unsigned long long)fbp_ns(p, a->ticks),
                   /* a sampled child's scaled estimate can exceed the parent's measured time */
                   (unsigned long long)fbp_ns(p, a->ticks > a->child_ticks
                                                 ? a->ticks - a->child_ticks : 0),
                   (long long)a->virt_ns);
        if (a->bytes) {
            fbp_printf(b, ",\"%s\":%llu", a->kind == FBP_MAINLOOP ? "fds" : "bytes",
                       (unsigned long long)a->bytes);
        }
        if (a->deadlines) {
            fbp_printf(b, ",\"deadlines\":%llu", (unsigned long long)a->deadlines);
        }
        if (a->sample > 1) {
            fbp_printf(b, ",\"sampled\":%u", a->sample);
        }
        fbp_printf(b, "}");
    }
    for (int r = 0; r < FBP_NREASONS; r++) {
        slices += p->reason[r];
    }
    fbp_printf(b, "],\"handoffs\":{\"count\":%llu,\"by_reason\":{", (unsigned long long)slices);
    for (int r = 0; r < FBP_NREASONS; r++) {
        fbp_printf(b, "%s\"%s\":%llu", r ? "," : "", fbp_reason_name[r],
                   (unsigned long long)p->reason[r]);
    }
    fbp_printf(b, "},\"by_timer\":{");
    bool first = true;
    for (unsigned i = 0; i < p->n; i++) {
        if (p->a[i].deadlines) {
            fbp_printf(b, "%s", first ? "" : ",");
            fbp_json_str(b, p->a[i].name);
            fbp_printf(b, ":%llu", (unsigned long long)p->a[i].deadlines);
            first = false;
        }
    }
    fbp_printf(b, "%s\"not_machine\":%llu},\"wake_ns\":", first ? "" : ",",
               (unsigned long long)p->not_machine);
    fbp_hist_json(b, &p->wake);
    fbp_printf(b, ",\"kick_ns\":");
    fbp_hist_json(b, &p->kick);
    fbp_printf(b, "},\"mainloop\":{\"iters\":%llu,\"poll_ns\":%llu,\"dispatch_ns\":%llu,"
               "\"fds\":%llu}",
               (unsigned long long)(ip >= 0 ? p->a[ip].count : 0),
               (unsigned long long)(ip >= 0 ? fbp_ns(p, p->a[ip].ticks) : 0),
               (unsigned long long)(im >= 0 ? fbp_ns(p, p->a[im].ticks) : 0),
               (unsigned long long)(ip >= 0 ? p->a[ip].bytes : 0));
    fbp_printf(b, ",\"threads\":{");
    for (int t = 0; t < FBP_NTHREADS; t++) {
        uint64_t acc = fbp_ns(p, p->thr[t].top_ticks);
        fbp_printf(b, "%s\"%s\":{\"accounted_ns\":%llu,\"unaccounted_ns\":%lld,"
                   "\"accounted_ratio\":%.4f}", t ? "," : "", fbp_thread_name[t],
                   (unsigned long long)acc, (long long)wall - (long long)acc,
                   wall ? (double)acc / wall : 0.0);
    }
    uint64_t vacc = fbp_ns(p, p->thr[FBP_T_VCPU].top_ticks);
    fbp_printf(b, "},\"guest_exec_or_unaccounted_ns\":%lld,\"accounted_ratio\":%.4f,"
               "\"ns_per_tick\":%.6f}\n", (long long)wall - (long long)vacc,
               wall ? (double)vacc / wall : 0.0, p->ns_per_tick);
}
