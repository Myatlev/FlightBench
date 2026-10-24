/* Host unit tests of the host-time ledger (hw/arm/fb_prof.c.inc) without QEMU: a fake tick
 * counter (2 ticks per ns after the calibration), nested accounts on two threads, untagged
 * threads, the latency histograms against exact percentiles, reset, and the JSON (written to
 * the file given as argv[1]; the Makefile checks it with Python's json module). */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fake_ticks;
static int64_t fake_mono;
static int64_t fake_mono_ns(void)
{
    fake_mono += 1000000;           /* 1 ms per read, 2e6 ticks with it: 0.5 ns per tick */
    fake_ticks += 2000000;
    return fake_mono;
}
#define FBP_TICKS()   (fake_ticks)
#define FBP_MONO_NS() fake_mono_ns()

#include "../hw/arm/fb_prof.c.inc"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void t_calibrate(FbProf *p)
{
    CHECK(p->ns_per_tick == 0.5, "ns_per_tick %f", p->ns_per_tick);
}

/* A(1000 ticks) containing B(200) and C(100, twice); a second top-level A; a timer on main */
static void t_nesting(FbProf *p, uint16_t a, uint16_t b, uint16_t c, uint16_t m)
{
    fbp_thread_set(FBP_T_VCPU);
    fake_ticks = 10000;
    fbp_enter(p, a);
    fake_ticks += 100;
    fbp_enter(p, b);
    fake_ticks += 200;
    fbp_leave(p, b);
    fbp_enter(p, c);
    fake_ticks += 50;
    fbp_leave(p, c);
    fbp_enter(p, c);
    fake_ticks += 50;
    fbp_leave(p, c);
    fake_ticks += 600;
    fbp_leave(p, a);
    fbp_enter(p, a);
    fake_ticks += 1000;
    fbp_leave(p, a);
    CHECK(p->a[a].count == 2 && p->a[a].ticks == 2000 && p->a[a].child_ticks == 300,
          "A count %llu ticks %llu child %llu", (unsigned long long)p->a[a].count,
          (unsigned long long)p->a[a].ticks, (unsigned long long)p->a[a].child_ticks);
    CHECK(p->a[b].count == 1 && p->a[b].ticks == 200 && p->a[b].child_ticks == 0, "B");
    CHECK(p->a[c].count == 2 && p->a[c].ticks == 100, "C count %llu ticks %llu",
          (unsigned long long)p->a[c].count, (unsigned long long)p->a[c].ticks);
    CHECK(p->thr[FBP_T_VCPU].top_ticks == 2000, "vcpu top %llu",
          (unsigned long long)p->thr[FBP_T_VCPU].top_ticks);
    CHECK(p->a[a].threads == 1, "A threads %u", p->a[a].threads);

    /* the main loop thread: its own stack and top-level sum */
    fbp_thread_set(FBP_T_MAIN);
    fbp_enter(p, m);
    fake_ticks += 400;
    fbp_enter(p, b);                /* B also on main: "both" */
    fake_ticks += 100;
    fbp_leave(p, b);
    fbp_leave(p, m);
    CHECK(p->thr[FBP_T_MAIN].top_ticks == 500 && p->thr[FBP_T_VCPU].top_ticks == 2000, "main top %llu",
          (unsigned long long)p->thr[FBP_T_MAIN].top_ticks);
    CHECK(p->a[b].threads == 3 && p->a[b].count == 2, "B threads %u", p->a[b].threads);

    /* an untagged thread changes nothing, a leave without an enter neither */
    fbp_thread_set(-1);
    fbp_enter(p, a);
    fake_ticks += 5000;
    fbp_leave(p, a);
    fbp_thread_set(FBP_T_VCPU);
    fbp_leave(p, a);
    CHECK(p->a[a].count == 2 && p->thr[FBP_T_VCPU].top_ticks == 2000, "untagged counted");

    /* deeper than the stack: the frames beyond are not timed, the ones below stay right */
    fbp_enter(p, a);
    for (int i = 0; i < FBP_MAX_DEPTH + 3; i++) {
        fbp_enter(p, c);
    }
    fake_ticks += 10;
    for (int i = 0; i < FBP_MAX_DEPTH + 3; i++) {
        fbp_leave(p, c);
    }
    fbp_leave(p, a);
    CHECK(fbp_tls.depth == 0, "depth %d", fbp_tls.depth);
    CHECK(p->a[a].count == 3 && p->a[a].ticks == 2010, "A after deep nesting: %llu %llu",
          (unsigned long long)p->a[a].count, (unsigned long long)p->a[a].ticks);
    CHECK(p->a[c].count == 2 + FBP_MAX_DEPTH - 1, "C after deep nesting: %llu",
          (unsigned long long)p->a[c].count);

    fbp_count(p, a, 50000);
    fbp_bytes(p, b, 17);
    CHECK(p->a[a].count == 4 && p->a[a].virt_ns == 50000 && p->a[b].bytes == 17, "count/bytes");
}

/* a sampled leaf: 70 accesses of 10 ticks each, every 7th timed: count 70, time 700 */
static void t_sampled(FbProf *p, uint16_t hot)
{
    fbp_thread_set(FBP_T_VCPU);
    fbp_set_sample(p, hot, 7);
    uint64_t top0 = p->thr[FBP_T_VCPU].top_ticks;
    int timed = 0;
    for (int i = 0; i < 70; i++) {
        bool t = fbp_enter_sampled(p, hot);
        fake_ticks += 10;
        if (t) {
            timed++;
            fbp_leave(p, hot);
        }
    }
    CHECK(timed == 10, "timed %d", timed);
    CHECK(p->a[hot].count == 70 && p->a[hot].ticks == 700, "sampled count %llu ticks %llu",
          (unsigned long long)p->a[hot].count, (unsigned long long)p->a[hot].ticks);
    CHECK(p->thr[FBP_T_VCPU].top_ticks - top0 == 700, "sampled top");
    CHECK(fbp_tls.depth == 0, "sampled depth");
    fbp_set_sample(p, hot, 0);
    CHECK(fbp_enter_sampled(p, hot), "sample 0 = every event timed");
    fbp_leave(p, hot);
}

static void t_registry(FbProf *p, uint16_t a)
{
    CHECK(fbp_acct(p, "A", FBP_TIMER) == a, "same name+kind -> same account");
    uint16_t x = fbp_acct(p, "A", FBP_MMIO);
    CHECK(x != a && x != FBP_NONE, "same name, other kind -> new account");
    char nm[16];
    unsigned n0 = p->n;
    for (unsigned i = n0; i < FBP_MAX_ACCTS; i++) {
        snprintf(nm, sizeof(nm), "f%u", i);
        CHECK(fbp_acct(p, nm, FBP_OTHER) == i, "fill %u", i);
    }
    CHECK(fbp_acct(p, "one too many", FBP_OTHER) == FBP_NONE, "full -> FBP_NONE");
    fbp_enter(p, FBP_NONE);
    fbp_leave(p, FBP_NONE);
    fbp_count(p, FBP_NONE, 1);
    CHECK(fbp_tls.depth == 0, "FBP_NONE is a no-op");
}

static void t_hist(void)
{
    static FbpHist h;
    static uint64_t v[20000];
    unsigned n = 0;

    /* small values are exact */
    for (uint64_t i = 0; i < 16; i++) {
        CHECK(fbp_hist_value(fbp_hist_index(i)) == i, "exact %llu", (unsigned long long)i);
    }
    /* every value lies within 1/16 of its bucket's middle, indices are monotonic */
    unsigned last = 0;
    for (uint64_t x = 1; x < (1ull << 40); x = x * 1.013 + 1) {
        unsigned i = fbp_hist_index(x);
        uint64_t m = fbp_hist_value(i);
        CHECK(i >= last, "monotonic at %llu", (unsigned long long)x);
        CHECK((double)(m > x ? m - x : x - m) <= x / 16.0 + 0.5, "bucket of %llu -> %llu",
              (unsigned long long)x, (unsigned long long)m);
        last = i;
    }
    CHECK(fbp_hist_index(~0ull) == FBP_HIST_N - 1, "clamped");

    /* a skewed distribution: 3 us body, a tail to 400 us */
    uint64_t r = 12345;
    for (n = 0; n < 20000; n++) {
        r = r * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t x = 2500 + (r >> 33) % 1000;
        if (n % 50 == 0) {
            x = 20000 + (r >> 20) % 380000;
        }
        v[n] = x;
        fbp_hist_add(&h, x);
    }
    qsort(v, n, sizeof(v[0]), cmp_u64);
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) {
        sum += v[i];
    }
    uint64_t p50 = v[(n + 1) / 2 - 1], p99 = v[(uint64_t)(0.99 * n + 0.999999) - 1];
    uint64_t e50 = fbp_hist_pct(&h, 0.5), e99 = fbp_hist_pct(&h, 0.99);
    CHECK(h.n == n && h.sum == sum && h.min == v[0] && h.max == v[n - 1], "exact n/sum/min/max");
    CHECK(e50 > p50 * 15 / 16 && e50 < p50 * 17 / 16, "p50 %llu vs %llu",
          (unsigned long long)e50, (unsigned long long)p50);
    CHECK(e99 > p99 * 15 / 16 && e99 < p99 * 17 / 16, "p99 %llu vs %llu",
          (unsigned long long)e99, (unsigned long long)p99);
    CHECK(fbp_hist_pct(&h, 1.0) == v[n - 1] || fbp_hist_pct(&h, 1.0) * 16 > v[n - 1] * 15,
          "p100");

    static FbpHist one;
    fbp_hist_add(&one, 777);
    CHECK(fbp_hist_pct(&one, 0.5) == 777 && fbp_hist_pct(&one, 0.99) == 777,
          "one sample -> clamped to min/max: %llu", (unsigned long long)fbp_hist_pct(&one, 0.5));
    static FbpHist none;
    CHECK(fbp_hist_pct(&none, 0.5) == 0, "empty");
}

static void t_json(FbProf *p, uint16_t tmr, const char *path)
{
    FbpBuf b = { 0 };
    p->reason[FBP_R_TIMER_DEADLINE] = 7;
    p->reason[FBP_R_IO] = 2;
    p->a[tmr].deadlines = 5;
    p->not_machine = 2;
    fbp_hist_add(&p->wake, 3000);
    fbp_hist_add(&p->kick, 9000);
    fake_ticks = p->t0 + 20000;     /* wall 10 us */
    fbp_json(p, &b, 1234567, ",\"host\":{\"cpu\":\"x \\\"y\\\"\"}");
    CHECK(b.len > 0 && b.p[b.len - 1] == '\n', "newline-terminated");
    CHECK(strstr(b.p, "\"wall_ns\":10000,") != NULL, "wall_ns: %.80s", b.p);
    CHECK(strstr(b.p, "\"virt_ns\":1234567,\"host\":{") != NULL, "head inserted");
    CHECK(strstr(b.p, "\"by_timer\":{\"A\":5,\"not_machine\":2}") != NULL, "by_timer");
    CHECK(strstr(b.p, "\"handoffs\":{\"count\":9,") != NULL, "handoff count");
    CHECK(strstr(b.p, "\"name\":\"we\\\"ird\\\\\"") != NULL, "name escaped");
    CHECK(strstr(b.p, "\"wake_ns\":{\"count\":1,\"mean\":3000,\"p50\":3000") != NULL, "wake");
    FILE *f = fopen(path, "w");
    CHECK(f != NULL, "open %s", path);
    if (f) {
        fwrite(b.p, 1, b.len, f);
        fclose(f);
    }
    free(b.p);

    /* reset: counts gone, accounts kept */
    unsigned n = p->n;
    fbp_reset(p, 5);
    CHECK(p->n == n && p->a[tmr].count == 0 && p->a[tmr].deadlines == 0 && p->wake.n == 0 &&
          p->reason[FBP_R_IO] == 0 && p->thr[FBP_T_VCPU].top_ticks == 0 && p->v0 == 5, "reset");
}

int main(int argc, char **argv)
{
    FbProf *p = fbp_new(0);
    uint16_t a = fbp_acct(p, "A", FBP_TIMER);
    uint16_t b = fbp_acct(p, "B", FBP_MMIO);
    uint16_t c = fbp_acct(p, "C", FBP_CHARDEV);
    uint16_t m = fbp_acct(p, "mainloop.dispatch", FBP_MAINLOOP);
    fbp_acct(p, "we\"ird\\", FBP_OTHER);

    t_calibrate(p);
    t_nesting(p, a, b, c, m);
    t_hist();
    t_sampled(p, fbp_acct(p, "hot", FBP_MMIO));
    t_json(p, a, argc > 1 ? argv[1] : "build/prof.json");
    t_registry(p, a);
    printf("test_prof: %s, %d checks, %d failure(s)\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
