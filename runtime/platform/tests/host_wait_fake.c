/*
 * The scripted clock and native waits behind runtime/platform/host_wait.c.
 * See host_wait_fake.h. Build with -DPS3_HW_RELATIVE=0 or =1.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host_wait_fake.h"
#define FK_UNUSED __attribute__((unused))

#ifndef PS3_HW_RELATIVE
#  error "host_wait_fake.c: define PS3_HW_RELATIVE=0 (absolute) or 1 (relative)"
#endif

enum { MAX_STEPS = 64, MAX_TRIES = 64, MAX_SLEEPS = 256 };

static pthread_mutex_t g_state = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_now;
static _Atomic uint64_t g_wall;
static _Atomic unsigned g_failures;
static _Atomic unsigned g_clock_reads;
static hw_fake_step g_steps[MAX_STEPS];
static unsigned g_nsteps, g_pos;
static hw_fake_stats g_stats;
static int g_try_script[MAX_TRIES];
static unsigned g_try_n, g_try_pos;
static int g_try_scripted;
static uint64_t g_sleeps[MAX_SLEEPS];
static unsigned g_nsleeps, g_yields, g_locks, g_tries;
static uint64_t g_yield_cost = 1000;
static hw_fake_init_ctl g_ictl;
static hw_fake_init_stats g_istats;

static void fail(const char* what, uint64_t a, uint64_t b)
{
    atomic_fetch_add(&g_failures, 1);
    fprintf(stderr, "host_wait_fake: FAIL %s (%llu, %llu)\n", what,
            (unsigned long long)a, (unsigned long long)b);
}

/* ---- the seams, in the order host_wait.c uses them ----------------------- */
static uint64_t fk_now(void)
{
    atomic_fetch_add(&g_clock_reads, 1);
    return atomic_load(&g_now);
}

static int fk_native(pthread_mutex_t* lock, int kind, uint64_t rel_ns)
{
    hw_fake_step s;
    pthread_mutex_lock(&g_state);
    if (g_pos >= g_nsteps) {
        pthread_mutex_unlock(&g_state);
        fprintf(stderr, "host_wait_fake: unscripted native wait (kind %d, rel %llu ns)\n",
                kind, (unsigned long long)rel_ns);
        abort();
    }
    s = g_steps[g_pos++];
    if (kind == HW_FAKE_TIMED) { g_stats.timed_calls++; g_stats.last_rel_ns = rel_ns; }
    else                       { g_stats.infinite_calls++; }
    pthread_mutex_unlock(&g_state);

    if (s.kind != kind) fail("native operation kind", (uint64_t)kind, (uint64_t)s.kind);
    if (kind == HW_FAKE_TIMED && s.want_rel_ns != HW_FAKE_ANY && s.want_rel_ns != rel_ns)
        fail("requested duration", rel_ns, s.want_rel_ns);

    /* A condition wait is entered with the caller's lock held; it releases it
     * for the sleep and holds it again on return. */
    const int held = pthread_mutex_trylock(lock) == EBUSY;
    if (!held) {
        pthread_mutex_unlock(lock);
        fail("lock held on entry to the native wait", 0, 0);
        atomic_fetch_add(&g_now, s.advance_ns);
        return s.rc;
    }
    pthread_mutex_unlock(lock);
    if (s.hook) s.hook(s.hook_arg);
    atomic_fetch_add(&g_now, s.advance_ns);
    pthread_mutex_lock(lock);
    return s.rc;
}

static int fk_cond_wait(pthread_cond_t* cv, pthread_mutex_t* lock)
{
    (void)cv;
    return fk_native(lock, HW_FAKE_INFINITE, 0);
}

/* One of the two timed entry points, and the attribute/stray seams, are unused by the backend a build takes
 * (the stray ones by correct code, which is the point). */
FK_UNUSED static int fk_timed_rel(pthread_cond_t* cv, pthread_mutex_t* lock, const struct timespec* rel)
{
    (void)cv;
    if (rel->tv_nsec < 0 || rel->tv_nsec >= 1000000000L) fail("relative tv_nsec out of range", (uint64_t)rel->tv_nsec, 0);
    return fk_native(lock, HW_FAKE_TIMED,
                     (uint64_t)rel->tv_sec * 1000000000ull + (uint64_t)rel->tv_nsec);
}

FK_UNUSED static int fk_timed_abs(pthread_cond_t* cv, pthread_mutex_t* lock, const struct timespec* abs)
{
    (void)cv;
    if (abs->tv_nsec < 0 || abs->tv_nsec >= 1000000000L) fail("absolute tv_nsec out of range", (uint64_t)abs->tv_nsec, 0);
    const uint64_t abs_ns = (uint64_t)abs->tv_sec * 1000000000ull + (uint64_t)abs->tv_nsec;
    const uint64_t now = atomic_load(&g_now);
    pthread_mutex_lock(&g_state);
    g_stats.last_abs_sec  = (int64_t)abs->tv_sec;
    g_stats.last_abs_nsec = (int64_t)abs->tv_nsec;
    pthread_mutex_unlock(&g_state);
    if (abs_ns < now) fail("absolute deadline already in the past", abs_ns, now);
    return fk_native(lock, HW_FAKE_TIMED, abs_ns >= now ? abs_ns - now : 0);
}

FK_UNUSED static int fk_attr_init(pthread_condattr_t* a)
{
    pthread_mutex_lock(&g_state);
    g_istats.attr_init++;
    const int rc = g_ictl.attr_init_rc;
    pthread_mutex_unlock(&g_state);
    return rc ? rc : pthread_condattr_init(a);
}

FK_UNUSED static int fk_setclock(pthread_condattr_t* a, int clock_id)
{
    (void)a;
    pthread_mutex_lock(&g_state);
    g_istats.setclock++;
    g_istats.clock_id = clock_id;
    const int rc = g_ictl.setclock_rc;
    pthread_mutex_unlock(&g_state);
    return rc;
}

FK_UNUSED static int fk_attr_destroy(pthread_condattr_t* a)
{
    pthread_mutex_lock(&g_state);
    g_istats.attr_destroy++;
    const int rc = g_ictl.attr_destroy_rc;
    pthread_mutex_unlock(&g_state);
    pthread_condattr_destroy(a);
    return rc;
}

static int fk_cond_init(pthread_cond_t* cv, const pthread_condattr_t* a)
{
    pthread_mutex_lock(&g_state);
    g_istats.cond_init++;
    if (a) g_istats.cond_init_with_attr++;
    const int rc = g_ictl.cond_init_rc;
    pthread_mutex_unlock(&g_state);
    return rc ? rc : pthread_cond_init(cv, a);
}

FK_UNUSED static int fk_cond_destroy(pthread_cond_t* cv)
{
    pthread_mutex_lock(&g_state);
    g_istats.cond_destroy++;
    pthread_mutex_unlock(&g_state);
    return pthread_cond_destroy(cv);
}

static int fk_mutex_lock(pthread_mutex_t* m)
{
    pthread_mutex_lock(&g_state);
    g_locks++;
    pthread_mutex_unlock(&g_state);
    return pthread_mutex_lock(m);
}

static int fk_mutex_trylock(pthread_mutex_t* m)
{
    int rc;
    pthread_mutex_lock(&g_state);
    g_tries++;
    if (g_try_scripted) rc = g_try_pos < g_try_n ? g_try_script[g_try_pos++] : EBUSY;
    else                rc = -1;                    /* the real try */
    pthread_mutex_unlock(&g_state);
    if (rc == -1) return pthread_mutex_trylock(m);
    if (rc == 0) pthread_mutex_lock(m);             /* "acquired" really takes it */
    return rc;
}

static void fk_sleep(uint64_t ns)
{
    pthread_mutex_lock(&g_state);
    if (g_nsleeps < MAX_SLEEPS) g_sleeps[g_nsleeps++] = ns;
    pthread_mutex_unlock(&g_state);
    atomic_fetch_add(&g_now, ns);
}

static void fk_yield(void)
{
    pthread_mutex_lock(&g_state);
    g_yields++;
    const uint64_t cost = g_yield_cost;
    pthread_mutex_unlock(&g_state);
    atomic_fetch_add(&g_now, cost);
}

/* Anything that reaches the host clocks or the host waits on its own, around
 * the seams, is a defect the test must see. */
FK_UNUSED static int fk_clock_gettime(clockid_t id, struct timespec* ts)
{
    uint64_t v;
    pthread_mutex_lock(&g_state);
    if (id == CLOCK_REALTIME) { g_stats.realtime_reads++; v = atomic_load(&g_wall); }
    else                      { g_stats.monotonic_gettime_reads++; v = atomic_load(&g_now); }
    pthread_mutex_unlock(&g_state);
    ts->tv_sec  = (time_t)(v / 1000000000ull);
    ts->tv_nsec = (long)(v % 1000000000ull);
    return 0;
}

FK_UNUSED static int fk_stray_timedwait(pthread_cond_t* cv, pthread_mutex_t* lock, const struct timespec* ts)
{
    (void)cv; (void)lock; (void)ts;
    pthread_mutex_lock(&g_state);
    g_stats.stray_native++;
    pthread_mutex_unlock(&g_state);
    return ETIMEDOUT;
}

FK_UNUSED static int fk_stray_wait(pthread_cond_t* cv, pthread_mutex_t* lock)
{
    (void)cv; (void)lock;
    pthread_mutex_lock(&g_state);
    g_stats.stray_native++;
    pthread_mutex_unlock(&g_state);
    return 0;
}

#define PS3_HOST_WAIT_TEST_SEAMS 1
#define HW_NOW_NS()                          fk_now()
#define HW_COND_WAIT(cv, lock)               fk_cond_wait((cv), (lock))
#define HW_COND_TIMEDWAIT_ABS(cv, lock, ts)  fk_timed_abs((cv), (lock), (ts))
#define HW_COND_TIMEDWAIT_REL(cv, lock, ts)  fk_timed_rel((cv), (lock), (ts))
#define HW_CONDATTR_INIT(a)                  fk_attr_init(a)
#define HW_CONDATTR_SETCLOCK(a, id)          fk_setclock((a), (int)(id))
#define HW_CONDATTR_DESTROY(a)               fk_attr_destroy(a)
#define HW_COND_INIT(cv, a)                  fk_cond_init((cv), (a))
#define HW_COND_DESTROY(cv)                  fk_cond_destroy(cv)
#define HW_MUTEX_LOCK(m)                     fk_mutex_lock(m)
#define HW_MUTEX_TRYLOCK(m)                  fk_mutex_trylock(m)
#define HW_SLEEP_NS(ns)                      fk_sleep(ns)
#define HW_YIELD()                           fk_yield()
#define clock_gettime                        fk_clock_gettime
#define pthread_cond_timedwait               fk_stray_timedwait
#define pthread_cond_wait                    fk_stray_wait

/* The code under test. */
#include "../host_wait.c"

#undef clock_gettime
#undef pthread_cond_timedwait
#undef pthread_cond_wait


/* ---- control ------------------------------------------------------------- */
void hw_fake_reset(uint64_t mono_now_ns, uint64_t wall_now_ns)
{
    pthread_mutex_lock(&g_state);
    memset(g_steps, 0, sizeof g_steps);
    g_nsteps = g_pos = 0;
    memset(&g_stats, 0, sizeof g_stats);
    g_try_n = g_try_pos = 0;
    g_try_scripted = 0;
    g_nsleeps = g_yields = g_locks = g_tries = 0;
    g_yield_cost = 1000;
    memset(&g_ictl, 0, sizeof g_ictl);
    memset(&g_istats, 0, sizeof g_istats);
    g_istats.clock_id = -1;
    atomic_store(&g_now, mono_now_ns);
    atomic_store(&g_wall, wall_now_ns);
    atomic_store(&g_failures, 0);
    atomic_store(&g_clock_reads, 0);
    pthread_mutex_unlock(&g_state);
}

void hw_fake_push(const hw_fake_step* step)
{
    pthread_mutex_lock(&g_state);
    if (g_nsteps >= MAX_STEPS) { fputs("host_wait_fake: script full\n", stderr); abort(); }
    g_steps[g_nsteps++] = *step;
    pthread_mutex_unlock(&g_state);
}

unsigned hw_fake_steps_left(void)
{
    pthread_mutex_lock(&g_state);
    const unsigned n = g_nsteps - g_pos;
    pthread_mutex_unlock(&g_state);
    return n;
}

unsigned hw_fake_failures(void) { return atomic_load(&g_failures); }

void hw_fake_get_stats(hw_fake_stats* out)
{
    pthread_mutex_lock(&g_state);
    *out = g_stats;
    out->clock_reads = atomic_load(&g_clock_reads);
    pthread_mutex_unlock(&g_state);
}

uint64_t hw_fake_now(void)             { return atomic_load(&g_now); }
void     hw_fake_set_now(uint64_t ns)  { atomic_store(&g_now, ns); }
void     hw_fake_set_wall(uint64_t ns) { atomic_store(&g_wall, ns); }

void hw_fake_mutex_script(const int* rcs, unsigned count)
{
    pthread_mutex_lock(&g_state);
    if (count > MAX_TRIES) count = MAX_TRIES;
    memcpy(g_try_script, rcs, count * sizeof *rcs);
    g_try_n = count;
    g_try_pos = 0;
    g_try_scripted = 1;
    pthread_mutex_unlock(&g_state);
}

void hw_fake_set_yield_cost_ns(uint64_t ns)
{
    pthread_mutex_lock(&g_state);
    g_yield_cost = ns;
    pthread_mutex_unlock(&g_state);
}

unsigned hw_fake_mutex_sleeps(const uint64_t** ns_out) { *ns_out = g_sleeps; return g_nsleeps; }
unsigned hw_fake_mutex_yields(void) { return g_yields; }
unsigned hw_fake_mutex_locks(void)  { return g_locks; }
unsigned hw_fake_mutex_tries(void)  { return g_tries; }

void hw_fake_init_set(const hw_fake_init_ctl* ctl)
{
    pthread_mutex_lock(&g_state);
    g_ictl = *ctl;
    pthread_mutex_unlock(&g_state);
}

void hw_fake_init_get(hw_fake_init_stats* out)
{
    pthread_mutex_lock(&g_state);
    *out = g_istats;
    pthread_mutex_unlock(&g_state);
}

int hw_fake_backend_is_relative(void) { return PS3_HW_RELATIVE; }

/* A test that links the Win32 shim (win32_compat.c) against this object instead of host_clock.c gets ONE clock:
 * the shim's deadlines and timer due times read the same scripted time as the wait loop. */
#ifdef HW_FAKE_DEFINES_HOST_CLOCK
uint64_t host_monotonic_ns(void) { return fk_now(); }
#endif
