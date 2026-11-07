/*
 * runtime/platform/host_wait.c against the real host: real condition
 * variables, real mutexes, the real clock.
 *
 * The deterministic loop contract is test_host_wait_monotonic.c. This file
 * proves the native half it cannot: that the host really sleeps the requested
 * duration, that a real signal wakes a real waiter with the lock held, and
 * (Linux) that a variable from ps3_cond_init_monotonic really is bound to
 * CLOCK_MONOTONIC. The loop re-checks the clock after every native return, so
 * an elapsed-time bound alone would pass a variable on the wrong clock by
 * spinning; the direct probe of the variable's clock is what catches that.
 *
 * Bounds are lower bounds only (elapsed >= requested): an upper bound on a
 * loaded host is a scheduler statement, not a contract. The only upper bound is
 * a gross sanity limit that turns a lost wake or a runaway wait into a failure
 * instead of a hang.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../host_clock.h"
#include "../host_wait.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define US 1000ull
#define MS 1000000ull
#define SEC 1000000000ull
#define SANITY (5ull * SEC)

static int is_held(pthread_mutex_t* m)
{
    const int rc = pthread_mutex_trylock(m);
    if (rc == 0) { pthread_mutex_unlock(m); return 0; }
    return rc == EBUSY;
}

/* An unsignalled wait lasts at least what was asked, on the clock the budget runs on. */
static void case_lower_bound(void)
{
    static const uint64_t us[] = { 1, 20, 100, 1000, 2000, 10000, 16000, 33000 };
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    CHECK(ps3_cond_init_monotonic(&cv) == 0);
    pthread_mutex_lock(&m);
    for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) {
        const uint64_t t0 = host_monotonic_ns();
        const ps3_wait_budget b = ps3_wait_budget_us(us[i], 0);
        int rc;
        do { rc = ps3_cond_wait_monotonic(&cv, &m, &b); } while (rc == 0);   /* nobody signals: only a spurious wake */
        const uint64_t elapsed = host_monotonic_ns() - t0;
        CHECK(rc == ETIMEDOUT);
        CHECK(is_held(&m));
        CHECK(elapsed >= us[i] * US);
        CHECK(elapsed < SANITY);
    }
    pthread_mutex_unlock(&m);
    pthread_cond_destroy(&cv);
}

/* The variable's own clock, probed directly. */
static void case_native_clock(void)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv;
    CHECK(ps3_cond_init_monotonic(&cv) == 0);
    pthread_mutex_lock(&m);
#if defined(__APPLE__)
    /* Darwin: the relative operation the backend uses, called on its own. */
    struct timespec rel = { 0, (long)(20 * MS) };
    const uint64_t t0 = host_monotonic_ns();
    const int rc = pthread_cond_timedwait_relative_np(&cv, &m, &rel);
    const uint64_t elapsed = host_monotonic_ns() - t0;
    CHECK(rc == ETIMEDOUT);
    CHECK(elapsed >= 15 * MS && elapsed < SANITY);     /* the loop above does not rely on this exactness */
#else
    /* Linux: an absolute CLOCK_MONOTONIC deadline 20 ms ahead. On a variable left on the default
     * (REALTIME) clock this is a time in 1970 and the call returns at once. */
    struct timespec abs;
    clock_gettime(CLOCK_MONOTONIC, &abs);
    const uint64_t t0 = (uint64_t)abs.tv_sec * SEC + (uint64_t)abs.tv_nsec;
    const uint64_t deadline = t0 + 20 * MS;
    abs.tv_sec  = (time_t)(deadline / SEC);
    abs.tv_nsec = (long)(deadline % SEC);
    const int rc = pthread_cond_timedwait(&cv, &m, &abs);
    clock_gettime(CLOCK_MONOTONIC, &abs);
    const uint64_t elapsed = (uint64_t)abs.tv_sec * SEC + (uint64_t)abs.tv_nsec - t0;
    CHECK(rc == ETIMEDOUT);
    CHECK(elapsed >= 20 * MS && elapsed < SANITY);
#endif
    pthread_mutex_unlock(&m);
    pthread_cond_destroy(&cv);
}

/* A real signal wakes a real waiter long before its budget, with the lock held again. */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int             waiting;       /* under m: the waiter is registered, about to wait */
    int             go;            /* under m: the predicate */
    int             rc;
    int             held_on_return;
    uint64_t        elapsed;
} signal_fixture;

static void* signal_waiter(void* p)
{
    signal_fixture* f = (signal_fixture*)p;
    pthread_mutex_lock(&f->m);
    const uint64_t t0 = host_monotonic_ns();
    const ps3_wait_budget b = ps3_wait_budget_us(30 * 1000000ull, 0);   /* 30 s */
    f->waiting = 1;
    while (!f->go) {
        f->rc = ps3_cond_wait_monotonic(&f->cv, &f->m, &b);
        if (f->rc != 0) break;
    }
    f->held_on_return = is_held(&f->m);
    f->elapsed = host_monotonic_ns() - t0;
    pthread_mutex_unlock(&f->m);
    return NULL;
}

static void case_signal(void)
{
    signal_fixture f = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, -1, 0, 0 };
    CHECK(ps3_cond_init_monotonic(&f.cv) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, signal_waiter, &f) == 0);
    for (;;) {                                          /* the waiter holds the lock until it is parked: registered means parked */
        pthread_mutex_lock(&f.m);
        const int waiting = f.waiting;
        if (waiting) { f.go = 1; pthread_cond_signal(&f.cv); }
        pthread_mutex_unlock(&f.m);
        if (waiting) break;
        sched_yield();
    }
    pthread_join(t, NULL);
    CHECK(f.rc == 0 && f.held_on_return);
    CHECK(f.elapsed < 25ull * SEC);                     /* woken, not timed out at 30 s */
    pthread_cond_destroy(&f.cv);
}

/* The timed mutex: it waits out its budget while another thread holds the lock, and takes the lock the
 * moment it is released. */
typedef struct {
    pthread_mutex_t m;
    _Atomic int     held;          /* the holder owns m */
    _Atomic int     release;
} mutex_fixture;

static void* mutex_holder(void* p)
{
    mutex_fixture* f = (mutex_fixture*)p;
    pthread_mutex_lock(&f->m);
    atomic_store(&f->held, 1);
    while (!atomic_load(&f->release)) sched_yield();
    pthread_mutex_unlock(&f->m);
    return NULL;
}

static void case_mutex(void)
{
    mutex_fixture f = { PTHREAD_MUTEX_INITIALIZER, 0, 0 };
    pthread_t t;
    CHECK(pthread_create(&t, NULL, mutex_holder, &f) == 0);
    while (!atomic_load(&f.held)) sched_yield();

    static const uint64_t us[] = { 1, 100, 2000, 20000 };
    for (unsigned i = 0; i < sizeof us / sizeof us[0]; i++) {
        const uint64_t t0 = host_monotonic_ns();
        const ps3_wait_budget b = ps3_wait_budget_us(us[i], 0);
        CHECK(ps3_mutex_lock_monotonic(&f.m, &b) == ETIMEDOUT);
        const uint64_t elapsed = host_monotonic_ns() - t0;
        CHECK(elapsed >= us[i] * US && elapsed < SANITY);
    }

    atomic_store(&f.release, 1);
    const ps3_wait_budget b = ps3_wait_budget_us(30 * 1000000ull, 0);
    CHECK(ps3_mutex_lock_monotonic(&f.m, &b) == 0);
    CHECK(is_held(&f.m));
    pthread_mutex_unlock(&f.m);
    pthread_join(t, NULL);
}

/* The static variable, for real: a producer first, then waiters, from several threads at once. */
PS3_STATIC_MONOTONIC_COND(static_cv)

enum { NTHREADS = 8 };
static _Atomic unsigned g_ready;
static _Atomic int g_go;
static pthread_cond_t* g_seen[NTHREADS];

static void* first_use_thread(void* arg)
{
    const long i = (long)arg;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load(&g_go)) { }
    g_seen[i] = static_cv();
    return NULL;
}

static void case_static_cond(void)
{
    pthread_t t[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) CHECK(pthread_create(&t[i], NULL, first_use_thread, (void*)i) == 0);
    while (atomic_load(&g_ready) != NTHREADS) { }
    atomic_store(&g_go, 1);
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    CHECK(g_seen[0] != NULL);
    for (int i = 1; i < NTHREADS; i++) CHECK(g_seen[i] == g_seen[0]);

    /* used as a timed variable: a producer's broadcast before anyone waits is harmless, then a wait expires */
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    CHECK(pthread_cond_broadcast(static_cv()) == 0);
    pthread_mutex_lock(&m);
    const uint64_t t0 = host_monotonic_ns();
    const ps3_wait_budget b = ps3_wait_budget_us(5000, 0);
    int rc;
    do { rc = ps3_cond_wait_monotonic(static_cv(), &m, &b); } while (rc == 0);
    CHECK(rc == ETIMEDOUT && host_monotonic_ns() - t0 >= 5 * MS);
    pthread_mutex_unlock(&m);
}

int main(void)
{
    case_lower_bound();
    case_native_clock();
    case_signal();
    case_mutex();
    case_static_cond();
    printf("PASS: host_wait on the real host\n");
    return 0;
}
