/*
 * runtime/platform/host_wait.c: the budget, the retry loop and the init
 * contract, driven deterministically.
 *
 * The REAL loop runs (host_wait_fake.c compiles host_wait.c with the test
 * seams), against a scripted monotonic clock and scripted native waits, once
 * per backend: PS3_HW_RELATIVE=0 is the absolute CLOCK_MONOTONIC deadline of
 * Linux, =1 the relative duration of macOS. Both run on any host. No sleep, no
 * host clock; the only thing that orders threads is a hook or an atomic gate.
 *
 * What this pins that a wall-clock-stable run of the old code also passed:
 *  - a REALTIME read or deadline anywhere (the original defect) is seen as a
 *    clock_gettime(CLOCK_REALTIME) from the code under test, or as an absolute
 *    time that is not `monotonic now + remaining`, while the synthetic wall
 *    clock sits 1.7e18 ns away and jumps both ways;
 *  - one budget lives across wakes: after a wake the next request is what is
 *    left, never the whole duration again;
 *  - 0 versus infinite, 1 us, nanosecond carry, UINT64_MAX and slice chunking;
 *  - a native error is neither a timeout nor a wake.
 *
 * Built twice by CMake (host_wait_monotonic_abs, host_wait_monotonic_rel).
 */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../host_wait.h"
#include "host_wait_fake.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define US 1000ull
#define MS 1000000ull
#define SEC 1000000000ull
#define BASE (1000ull * SEC)                 /* the fake monotonic clock at the start of a case */
#define WALL (1700000000ull * SEC)           /* the synthetic wall clock: nowhere near BASE */
#define SLICE_MAX (1ull << 40)               /* PS3_HW_MAX_SLICE_NS */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;   /* never really waited on: the fake owns the native wait */

static int is_held(pthread_mutex_t* m)
{
    const int rc = pthread_mutex_trylock(m);
    if (rc == 0) { pthread_mutex_unlock(m); return 0; }
    return rc == EBUSY;
}

static void begin(uint64_t now)
{
    hw_fake_reset(now, WALL);
    CHECK(pthread_mutex_lock(&g_lock) == 0);
}

static void finish(void)
{
    CHECK(is_held(&g_lock));                 /* the lock is the caller's on every return */
    pthread_mutex_unlock(&g_lock);
    CHECK(hw_fake_failures() == 0);
    CHECK(hw_fake_steps_left() == 0);
}

static void push(int kind, int rc, uint64_t advance, uint64_t want_rel)
{
    hw_fake_step s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.rc = rc; s.advance_ns = advance; s.want_rel_ns = want_rel;
    hw_fake_push(&s);
}

static void push_hook(int kind, int rc, uint64_t advance, uint64_t want_rel, hw_fake_hook h, void* arg)
{
    hw_fake_step s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.rc = rc; s.advance_ns = advance; s.want_rel_ns = want_rel;
    s.hook = h; s.hook_arg = arg;
    hw_fake_push(&s);
}

static int wait_on(const ps3_wait_budget* b) { return ps3_cond_wait_monotonic(&g_cv, &g_lock, b); }

/* ------------------------------------------------------------------------- */
static void case_budget_math(void)
{
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
    CHECK(b.start_ns == BASE && b.duration_ns == 10 * MS && !b.infinite);
    b = ps3_wait_budget_us(1, 0);
    CHECK(b.duration_ns == 1000);
    b = ps3_wait_budget_us(0, 0);
    CHECK(b.duration_ns == 0 && !b.infinite);          /* a finite zero is expiry, not "forever" */
    b = ps3_wait_budget_us(0, 1);
    CHECK(b.infinite);
    b = ps3_wait_budget_ns(7, 1);
    CHECK(b.infinite && b.duration_ns == 7);
    b = ps3_wait_budget_ns(7, 0);
    CHECK(!b.infinite && b.duration_ns == 7);

    /* usec * 1000 saturates: a huge FINITE wait, never a wrapped short one and never infinite. */
    const uint64_t edge = UINT64_MAX / 1000u;
    b = ps3_wait_budget_us(edge, 0);
    CHECK(b.duration_ns == edge * 1000u && !b.infinite);
    b = ps3_wait_budget_us(edge + 1, 0);
    CHECK(b.duration_ns == UINT64_MAX && !b.infinite);
    b = ps3_wait_budget_us(UINT64_MAX, 0);
    CHECK(b.duration_ns == UINT64_MAX && !b.infinite);
    finish();
}

/* The sequence of the design: T = 10 ms. A wake at 3 ms; the next iteration of the caller asks for the
 * 7 ms that are left; EINTR at 5 ms asks for 5; a timeout the kernel reports at 8 ms (early or capped) asks
 * for 2; the clock reaching 10 ms is ETIMEDOUT. One budget throughout. */
static void case_sequence(void)
{
    const uint64_t T = 10 * MS;
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);

    push(HW_FAKE_TIMED, 0, 3 * MS, T);                 /* a signal, or a spurious wake, at 3 ms */
    CHECK(wait_on(&b) == 0);
    CHECK(hw_fake_now() == BASE + 3 * MS);

    push(HW_FAKE_TIMED, EINTR, 2 * MS, 7 * MS);        /* interrupted at 5 ms */
    push(HW_FAKE_TIMED, ETIMEDOUT, 3 * MS, 5 * MS);    /* reported at 8 ms with 2 ms left */
    push(HW_FAKE_TIMED, ETIMEDOUT, 2 * MS, 2 * MS);    /* the last 2 ms */
    CHECK(wait_on(&b) == ETIMEDOUT);
    CHECK(hw_fake_now() - BASE == T);                  /* >= T, and no slack was scripted: exactly T */

    hw_fake_stats st;
    hw_fake_get_stats(&st);
    CHECK(st.timed_calls == 4 && st.infinite_calls == 0);
    finish();
}

/* A wake at the deadline is a wake; the caller decides what the predicate says. */
static void case_wake_at_deadline_and_slack(void)
{
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
    push(HW_FAKE_TIMED, 0, 10 * MS, 10 * MS);
    CHECK(wait_on(&b) == 0);
    CHECK(wait_on(&b) == ETIMEDOUT);                   /* now spent: no native wait */
    finish();

    /* The thread was descheduled: the native wait returned 37 us late. ETIMEDOUT, elapsed >= T, within the slack. */
    begin(BASE);
    b = ps3_wait_budget_us(10000, 0);
    push(HW_FAKE_TIMED, ETIMEDOUT, 10 * MS + 37 * US, 10 * MS);
    CHECK(wait_on(&b) == ETIMEDOUT);
    const uint64_t elapsed = hw_fake_now() - BASE;
    CHECK(elapsed >= 10 * MS && elapsed <= 10 * MS + 37 * US);
    finish();
}

/* Repeated spurious wakes never extend the wait: ten 1 ms wakes spend a 10 ms budget, the eleventh call
 * is ETIMEDOUT without a native wait. */
static void case_repeated_wakes(void)
{
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
    for (unsigned i = 0; i < 10; i++) {
        push(HW_FAKE_TIMED, 0, 1 * MS, (10 - i) * MS);
        CHECK(wait_on(&b) == 0);
    }
    CHECK(wait_on(&b) == ETIMEDOUT);
    hw_fake_stats st;
    hw_fake_get_stats(&st);
    CHECK(st.timed_calls == 10);
    finish();
}

/* The clock domain. The synthetic wall clock is 1.7e18 ns from the monotonic one and jumps both ways while the
 * waiter sleeps. Nothing the wait does may depend on it. */
static void jump_wall_forward(void* a)  { (void)a; hw_fake_set_wall(WALL + 86400ull * SEC); }
static void jump_wall_backward(void* a) { (void)a; hw_fake_set_wall(5ull * SEC); }

static void case_clock_domain(void)
{
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
    push_hook(HW_FAKE_TIMED, ETIMEDOUT, 4 * MS, 10 * MS, jump_wall_forward, NULL);
    push_hook(HW_FAKE_TIMED, ETIMEDOUT, 6 * MS, 6 * MS, jump_wall_backward, NULL);
    CHECK(wait_on(&b) == ETIMEDOUT);
    CHECK(hw_fake_now() - BASE == 10 * MS);            /* exactly the budget, whatever the wall clock did */

    hw_fake_stats st;
    hw_fake_get_stats(&st);
    CHECK(st.realtime_reads == 0);                     /* no wall clock read, no REALTIME deadline built */
    CHECK(st.monotonic_gettime_reads == 0);            /* the loop reads the clock through one function only */
    CHECK(st.stray_native == 0);                       /* no native wait around the seam */
    finish();

    if (!hw_fake_backend_is_relative()) {
        /* The absolute deadline is `monotonic now + remaining`, to the nanosecond, on every request. */
        begin(BASE + 123456789ull);
        b = ps3_wait_budget_us(2500, 0);
        push(HW_FAKE_TIMED, 0, 1 * MS, 2500 * US);
        CHECK(wait_on(&b) == 0);
        hw_fake_get_stats(&st);
        CHECK((uint64_t)st.last_abs_sec * SEC + (uint64_t)st.last_abs_nsec == BASE + 123456789ull + 2500 * US);
        push(HW_FAKE_TIMED, 0, 0, 1500 * US);
        CHECK(wait_on(&b) == 0);
        hw_fake_get_stats(&st);
        CHECK((uint64_t)st.last_abs_sec * SEC + (uint64_t)st.last_abs_nsec == BASE + 123456789ull + 1 * MS + 1500 * US);
        finish();
    }
}

static void case_boundaries(void)
{
    hw_fake_stats st;

    /* zero finite: expiry, no native wait, lock held */
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(0, 0);
    CHECK(wait_on(&b) == ETIMEDOUT);
    hw_fake_get_stats(&st);
    CHECK(st.timed_calls == 0 && st.infinite_calls == 0);
    finish();

    /* 1 us */
    begin(BASE);
    b = ps3_wait_budget_us(1, 0);
    push(HW_FAKE_TIMED, ETIMEDOUT, 1000, 1000);
    CHECK(wait_on(&b) == ETIMEDOUT);
    finish();

    /* infinite: the native infinite wait, however far the clock goes; the clock is not read again */
    begin(BASE);
    b = ps3_wait_budget_us(5, 1);
    hw_fake_get_stats(&st);
    const unsigned reads_before = st.clock_reads;
    push(HW_FAKE_INFINITE, 0, 100ull * 365 * 86400 * SEC, HW_FAKE_ANY);
    CHECK(wait_on(&b) == 0);                           /* 100 years elapse; an infinite wait does not time out */
    hw_fake_get_stats(&st);
    CHECK(st.timed_calls == 0 && st.infinite_calls == 1 && st.clock_reads == reads_before);
    finish();

    /* UINT64_MAX: a finite budget served in capped slices, never wrapped and never infinite */
    begin(BASE);
    b = ps3_wait_budget_us(UINT64_MAX, 0);
    push(HW_FAKE_TIMED, ETIMEDOUT, SLICE_MAX, SLICE_MAX);
    push(HW_FAKE_TIMED, ETIMEDOUT, SLICE_MAX, SLICE_MAX);
    push(HW_FAKE_TIMED, ETIMEDOUT, SLICE_MAX, SLICE_MAX);
    push(HW_FAKE_TIMED, 0, 0, SLICE_MAX);
    CHECK(wait_on(&b) == 0);
    CHECK(hw_fake_now() - BASE == 3 * SLICE_MAX);
    finish();

    /* the nanosecond carry: 999,999,999 ns into a second, plus 1 ns */
    begin(BASE + 999999999ull);
    b = ps3_wait_budget_ns(1, 0);
    push(HW_FAKE_TIMED, ETIMEDOUT, 1, 1);
    CHECK(wait_on(&b) == ETIMEDOUT);
    hw_fake_get_stats(&st);
    if (!hw_fake_backend_is_relative()) {
        CHECK(st.last_abs_sec == (int64_t)(BASE / SEC) + 1 && st.last_abs_nsec == 0);
    } else {
        CHECK(st.last_rel_ns == 1);
    }
    finish();

    /* 1.5 s: more than a second's worth of nanoseconds in the request */
    begin(BASE + 800 * MS);
    b = ps3_wait_budget_ns(1500 * MS, 0);
    push(HW_FAKE_TIMED, ETIMEDOUT, 1500 * MS, 1500 * MS);
    CHECK(wait_on(&b) == ETIMEDOUT);
    hw_fake_get_stats(&st);
    if (!hw_fake_backend_is_relative())
        CHECK(st.last_abs_sec == (int64_t)(BASE / SEC) + 2 && st.last_abs_nsec == 300 * (int64_t)MS);
    finish();

    /* a clock that reads below the budget's start (it must not) neither wraps nor expires early */
    begin(BASE);
    b = ps3_wait_budget_us(10000, 0);
    hw_fake_set_now(BASE - 5 * MS);
    push(HW_FAKE_TIMED, ETIMEDOUT, 15 * MS, 10 * MS);
    CHECK(wait_on(&b) == ETIMEDOUT);
    finish();
}

/* A host refusal is its own answer: not a timeout, not a wake. */
static void case_errors(void)
{
    static const int errs[] = { EINVAL, EPERM, ENOMEM };
    for (unsigned i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        begin(BASE);
        ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
        push(HW_FAKE_TIMED, errs[i], 1 * MS, 10 * MS);
        CHECK(wait_on(&b) == errs[i]);
        finish();

        begin(BASE);
        b = ps3_wait_budget_us(0, 1);
        push(HW_FAKE_INFINITE, errs[i], 0, HW_FAKE_ANY);
        CHECK(wait_on(&b) == errs[i]);
        finish();
    }
    /* ... after an interrupted slice too */
    begin(BASE);
    ps3_wait_budget b = ps3_wait_budget_us(10000, 0);
    push(HW_FAKE_TIMED, EINTR, 1 * MS, 10 * MS);
    push(HW_FAKE_TIMED, EINVAL, 0, 9 * MS);
    CHECK(wait_on(&b) == EINVAL);
    finish();
}

/* ------------------------------------------------------------------------- */
static void case_init(void)
{
    hw_fake_init_stats st;
    pthread_cond_t cv;
    hw_fake_init_ctl ctl;

    hw_fake_reset(BASE, WALL);
    CHECK(ps3_cond_init_monotonic(&cv) == 0);
    hw_fake_init_get(&st);
    if (!hw_fake_backend_is_relative()) {
        /* Linux: a CLOCK_MONOTONIC attribute, never the default clock. */
        CHECK(st.attr_init == 1 && st.setclock == 1 && st.clock_id == (int)CLOCK_MONOTONIC);
        CHECK(st.cond_init == 1 && st.cond_init_with_attr == 1 && st.attr_destroy == 1);
    } else {
        /* macOS: an ordinary condition variable; its wait takes a duration. */
        CHECK(st.attr_init == 0 && st.setclock == 0 && st.attr_destroy == 0);
        CHECK(st.cond_init == 1 && st.cond_init_with_attr == 0);
    }
    pthread_cond_destroy(&cv);

    if (!hw_fake_backend_is_relative()) {
        memset(&ctl, 0, sizeof ctl);
        ctl.attr_init_rc = EAGAIN;                       /* nothing was built: nothing to undo */
        hw_fake_reset(BASE, WALL);
        hw_fake_init_set(&ctl);
        CHECK(ps3_cond_init_monotonic(&cv) == EAGAIN);
        hw_fake_init_get(&st);
        CHECK(st.setclock == 0 && st.cond_init == 0 && st.attr_destroy == 0);

        memset(&ctl, 0, sizeof ctl);
        ctl.setclock_rc = EINVAL;                        /* no fallback to the default (REALTIME) clock */
        hw_fake_reset(BASE, WALL);
        hw_fake_init_set(&ctl);
        CHECK(ps3_cond_init_monotonic(&cv) == EINVAL);
        hw_fake_init_get(&st);
        CHECK(st.cond_init == 0 && st.attr_destroy == 1);

        memset(&ctl, 0, sizeof ctl);
        ctl.cond_init_rc = ENOMEM;
        hw_fake_reset(BASE, WALL);
        hw_fake_init_set(&ctl);
        CHECK(ps3_cond_init_monotonic(&cv) == ENOMEM);
        hw_fake_init_get(&st);
        CHECK(st.attr_destroy == 1 && st.cond_destroy == 0);

        memset(&ctl, 0, sizeof ctl);
        ctl.attr_destroy_rc = EBUSY;                     /* built, then undone: no half-initialised variable */
        hw_fake_reset(BASE, WALL);
        hw_fake_init_set(&ctl);
        CHECK(ps3_cond_init_monotonic(&cv) == EBUSY);
        hw_fake_init_get(&st);
        CHECK(st.cond_init == 1 && st.cond_destroy == 1);
    } else {
        memset(&ctl, 0, sizeof ctl);
        ctl.cond_init_rc = EAGAIN;
        hw_fake_reset(BASE, WALL);
        hw_fake_init_set(&ctl);
        CHECK(ps3_cond_init_monotonic(&cv) == EAGAIN);
        hw_fake_init_get(&st);
        CHECK(st.cond_init == 1 && st.cond_destroy == 0);
    }
}

/* ------------------------------------------------------------------------- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static void case_mutex(void)
{
    const uint64_t* sleeps;
    unsigned n;

    /* infinite: the native lock, no tries, no clock */
    hw_fake_reset(BASE, WALL);
    ps3_wait_budget b = ps3_wait_budget_us(0, 1);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == 0);
    CHECK(hw_fake_mutex_locks() == 1 && hw_fake_mutex_tries() == 0);
    pthread_mutex_unlock(&g_mx);

    /* zero finite: one try, no waiting; free is acquired, busy is ETIMEDOUT */
    static const int free_once[] = { 0 };
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_us(0, 0);
    hw_fake_mutex_script(free_once, 1);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == 0 && is_held(&g_mx));
    pthread_mutex_unlock(&g_mx);
    static const int busy_once[] = { EBUSY };
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_us(0, 0);
    hw_fake_mutex_script(busy_once, 1);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == ETIMEDOUT && !is_held(&g_mx));
    CHECK(hw_fake_mutex_tries() == 1 && hw_fake_mutex_sleeps(&sleeps) == 0 && hw_fake_mutex_yields() == 0);

    /* a real error is returned at once, not waited out */
    static const int bad[] = { EINVAL };
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_us(1000, 0);
    hw_fake_mutex_script(bad, 1);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == EINVAL);
    CHECK(hw_fake_mutex_tries() == 1 && hw_fake_mutex_sleeps(&sleeps) == 0);

    /* busy three times, then free: the back-off is capped by the remaining time and by 200 us */
    static const int busy3[] = { EBUSY, EBUSY, EBUSY, 0 };
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_us(1000, 0);
    hw_fake_mutex_script(busy3, 4);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == 0 && is_held(&g_mx));
    pthread_mutex_unlock(&g_mx);
    n = hw_fake_mutex_sleeps(&sleeps);
    CHECK(n == 3 && sleeps[0] == 5000 && sleeps[1] == 10000 && sleeps[2] == 20000);
    CHECK(hw_fake_now() - BASE == 35000);

    /* never free: ETIMEDOUT at the budget; no nap longer than 200 us or than what was left */
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_us(3000, 0);
    hw_fake_mutex_script(busy_once, 0);                 /* scripted, empty: every try is busy */
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == ETIMEDOUT && !is_held(&g_mx));
    uint64_t elapsed = hw_fake_now() - BASE;
    CHECK(elapsed >= 3 * MS && elapsed <= 3 * MS + 5000);
    n = hw_fake_mutex_sleeps(&sleeps);
    uint64_t total = 0, longest = 0;
    for (unsigned i = 0; i < n; i++) {
        CHECK(sleeps[i] >= 5000 && sleeps[i] <= 200 * US);
        CHECK(total + sleeps[i] <= 3 * MS);             /* never past the budget */
        total += sleeps[i];
        if (sleeps[i] > longest) longest = sleeps[i];
    }
    CHECK(longest == 200 * US);                         /* the cap is reached on a long wait */

    /* the last try at the boundary: acquiring then counts, it is not a timeout */
    static const int busy_then_free[] = { EBUSY, 0 };
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_ns(5000, 0);
    hw_fake_mutex_script(busy_then_free, 2);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == 0 && is_held(&g_mx));
    pthread_mutex_unlock(&g_mx);
    CHECK(hw_fake_now() - BASE == 5000);

    /* under 5 us left the thread yields instead of sleeping, and the clock still converges */
    hw_fake_reset(BASE, WALL);
    b = ps3_wait_budget_ns(3000, 0);
    hw_fake_mutex_script(busy_once, 0);
    CHECK(ps3_mutex_lock_monotonic(&g_mx, &b) == ETIMEDOUT);
    CHECK(hw_fake_mutex_yields() == 3 && hw_fake_mutex_sleeps(&sleeps) == 0);
}

/* ------------------------------------------------------------------------- */
PS3_STATIC_MONOTONIC_COND(static_ok)
PS3_STATIC_MONOTONIC_COND(static_bad)
PS3_STATIC_MONOTONIC_COND(static_producer)

enum { NTHREADS = 8 };
static _Atomic unsigned g_ready;
static _Atomic int g_go;
static pthread_cond_t* g_seen[NTHREADS];
static pthread_cond_t* (*g_accessor)(void);

static void* first_use_thread(void* arg)
{
    const long i = (long)arg;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load(&g_go)) { }                     /* released only when every thread is at the gate */
    g_seen[i] = g_accessor();
    return NULL;
}

static void run_first_use(pthread_cond_t* (*accessor)(void))
{
    pthread_t t[NTHREADS];
    atomic_store(&g_ready, 0);
    atomic_store(&g_go, 0);
    g_accessor = accessor;
    for (long i = 0; i < NTHREADS; i++) CHECK(pthread_create(&t[i], NULL, first_use_thread, (void*)i) == 0);
    while (atomic_load(&g_ready) != NTHREADS) { }
    atomic_store(&g_go, 1);
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
}

static void case_static_cond(void)
{
    hw_fake_init_stats st;

    /* eight threads race the first use: one initialisation, one object */
    hw_fake_reset(BASE, WALL);
    run_first_use(static_ok);
    hw_fake_init_get(&st);
    CHECK(st.cond_init == 1);
    CHECK(g_seen[0] != NULL);
    for (int i = 1; i < NTHREADS; i++) CHECK(g_seen[i] == g_seen[0]);
    CHECK(static_ok() == g_seen[0]);                    /* later uses neither re-initialise nor move it */
    hw_fake_init_get(&st);
    CHECK(st.cond_init == 1);

    /* a failed initialisation is remembered for every user and never retried over a live variable */
    hw_fake_reset(BASE, WALL);
    hw_fake_init_ctl ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.cond_init_rc = EAGAIN;
    hw_fake_init_set(&ctl);
    run_first_use(static_bad);
    for (int i = 0; i < NTHREADS; i++) CHECK(g_seen[i] == NULL);
    memset(&ctl, 0, sizeof ctl);
    hw_fake_init_set(&ctl);                             /* the host recovers: the answer does not change */
    CHECK(static_bad() == NULL);
    hw_fake_init_get(&st);
    CHECK(st.cond_init == 1);

    /* a producer that arrives before any waiter reaches a live variable: signal and broadcast are gated too */
    hw_fake_reset(BASE, WALL);
    pthread_cond_t* c = static_producer();
    CHECK(c != NULL);
    CHECK(pthread_cond_broadcast(c) == 0);
    CHECK(pthread_cond_signal(c) == 0);
    CHECK(static_producer() == c);
    hw_fake_init_get(&st);
    CHECK(st.cond_init == 1);
}

int main(void)
{
    case_budget_math();
    case_sequence();
    case_wake_at_deadline_and_slack();
    case_repeated_wakes();
    case_clock_domain();
    case_boundaries();
    case_errors();
    case_init();
    case_mutex();
    case_static_cond();
    printf("PASS: host_wait monotonic budget, %s backend\n",
           hw_fake_backend_is_relative() ? "relative" : "absolute");
    return 0;
}
