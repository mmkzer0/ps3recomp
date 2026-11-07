/*
 * ps3recomp - relative waits on the host monotonic clock (see host_wait.h)
 *
 * Two native backends, one loop.
 *
 *   Linux  The condition variable carries a CLOCK_MONOTONIC condattr
 *          (ps3_cond_init_monotonic), so pthread_cond_timedwait takes an
 *          absolute CLOCK_MONOTONIC time. host_monotonic_ns IS CLOCK_MONOTONIC
 *          there, so the deadline of a slice is `now + remaining` on the clock
 *          the loop just read.
 *   macOS  pthread_cond_timedwait_relative_np takes a duration, so no absolute
 *          time of any clock is built. Darwin has no condattr clock, and a
 *          mach_absolute_time number must never be handed to a CLOCK_MONOTONIC
 *          API or the reverse.
 *
 * The loop never trusts a native result for the time: after a timeout it asks
 * the clock whether the budget is spent and goes back to sleep for what is left
 * if it is not (a slice capped at PS3_HW_MAX_SLICE_NS, a kernel that returned
 * early). Only the budget produces ETIMEDOUT.
 *
 * Test seams. A test defines PS3_HOST_WAIT_TEST_SEAMS and every HW_* macro
 * below, then #includes this file, so the REAL loop runs against a scripted
 * clock and scripted native results. Nothing here is a mutable global, and a
 * production build has no seam: the macros expand to the host calls. A fake
 * clock in front of an unmodified kernel wait would prove nothing, because the
 * kernel waits in its own clock domain; the seams replace both ends together.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE          /* pthread_condattr_setclock */
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE     /* pthread_cond_timedwait_relative_np */
#endif

#include "host_wait.h"
#include "host_clock.h"

#include <errno.h>
#include <sched.h>
#include <time.h>

#ifndef PS3_HW_RELATIVE
#  ifdef __APPLE__
#    define PS3_HW_RELATIVE 1
#  else
#    define PS3_HW_RELATIVE 0
#  endif
#endif

#ifdef PS3_HOST_WAIT_TEST_SEAMS
/* HW_NOW_NS() HW_COND_WAIT(cv,lock) HW_COND_TIMEDWAIT_ABS(cv,lock,ts)
 * HW_COND_TIMEDWAIT_REL(cv,lock,ts) HW_CONDATTR_INIT(a) HW_CONDATTR_SETCLOCK(a,id)
 * HW_CONDATTR_DESTROY(a) HW_COND_INIT(cv,a) HW_COND_DESTROY(cv) HW_MUTEX_LOCK(m)
 * HW_MUTEX_TRYLOCK(m) HW_SLEEP_NS(ns) HW_YIELD() are the test's. */
#else
#  define HW_NOW_NS()                           host_monotonic_ns()
#  define HW_COND_WAIT(cv, lock)                pthread_cond_wait((cv), (lock))
#  define HW_COND_INIT(cv, a)                   pthread_cond_init((cv), (a))
#  define HW_COND_DESTROY(cv)                   pthread_cond_destroy(cv)
#  define HW_MUTEX_LOCK(m)                      pthread_mutex_lock(m)
#  define HW_MUTEX_TRYLOCK(m)                   pthread_mutex_trylock(m)
#  define HW_SLEEP_NS(ns)                       hw_sleep_ns(ns)
#  define HW_YIELD()                            sched_yield()
#  ifdef __APPLE__
#    define HW_COND_TIMEDWAIT_REL(cv, lock, ts) pthread_cond_timedwait_relative_np((cv), (lock), (ts))
#  else
#    define HW_COND_TIMEDWAIT_ABS(cv, lock, ts) pthread_cond_timedwait((cv), (lock), (ts))
#    define HW_CONDATTR_INIT(a)                 pthread_condattr_init(a)
#    define HW_CONDATTR_SETCLOCK(a, id)         pthread_condattr_setclock((a), (id))
#    define HW_CONDATTR_DESTROY(a)              pthread_condattr_destroy(a)
#  endif
static void hw_sleep_ns(uint64_t ns)
{
    struct timespec d;
    d.tv_sec  = (time_t)(ns / 1000000000ull);
    d.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&d, NULL);      /* a signal ends it early; the caller re-checks the budget */
}
#endif

#define NS_PER_SEC 1000000000ull

/* The longest single native wait. Whatever the budget, a slice stays far inside
 * what a timespec and `now + slice` can hold; a longer budget simply takes more
 * slices. 2^40 ns is about 18 minutes. */
#define PS3_HW_MAX_SLICE_NS (1ull << 40)

/* Backoff of the timed mutex: starts at 5 us, doubles, never exceeds 200 us.
 * Below the yield threshold a sleep costs more than it saves. */
#define PS3_HW_MUTEX_STEP_MIN_NS   5000ull
#define PS3_HW_MUTEX_STEP_MAX_NS 200000ull
#define PS3_HW_MUTEX_YIELD_BELOW_NS 5000ull

ps3_wait_budget ps3_wait_budget_ns(uint64_t nsec, int infinite)
{
    ps3_wait_budget b;
    b.start_ns    = HW_NOW_NS();
    b.duration_ns = nsec;
    b.infinite    = infinite ? 1 : 0;
    return b;
}

ps3_wait_budget ps3_wait_budget_us(uint64_t usec, int infinite)
{
    return ps3_wait_budget_ns(usec > UINT64_MAX / 1000u ? UINT64_MAX : usec * 1000u, infinite);
}

/* What is left of a finite budget at `now`; 0 once it is spent. Compares before
 * it subtracts, so neither a huge duration nor a clock that reads below the
 * start (it must not) can wrap. */
static uint64_t budget_remaining_ns(const ps3_wait_budget* b, uint64_t now)
{
    const uint64_t elapsed = now >= b->start_ns ? now - b->start_ns : 0;
    return elapsed >= b->duration_ns ? 0 : b->duration_ns - elapsed;
}

int ps3_cond_init_monotonic(pthread_cond_t* cv)
{
#if PS3_HW_RELATIVE
    return HW_COND_INIT(cv, NULL);
#else
    pthread_condattr_t attr;
    int rc = HW_CONDATTR_INIT(&attr);
    if (rc != 0) return rc;
    rc = HW_CONDATTR_SETCLOCK(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = HW_COND_INIT(cv, &attr);
    const int destroy_rc = HW_CONDATTR_DESTROY(&attr);
    if (rc == 0 && destroy_rc != 0) {
        /* The variable exists but its attribute could not be released: report the
         * failure and leave nothing initialised behind. */
        HW_COND_DESTROY(cv);
        rc = destroy_rc;
    }
    return rc;
#endif
}

/* One native wait of at most `rel_ns` (already capped, never 0) starting at
 * `now_ns`. */
static int wait_slice(pthread_cond_t* cv, pthread_mutex_t* lock, uint64_t now_ns, uint64_t rel_ns)
{
#if PS3_HW_RELATIVE
    (void)now_ns;
    struct timespec rel;
    rel.tv_sec  = (time_t)(rel_ns / NS_PER_SEC);
    rel.tv_nsec = (long)(rel_ns % NS_PER_SEC);
    return HW_COND_TIMEDWAIT_REL(cv, lock, &rel);
#else
    const uint64_t abs_ns = now_ns + rel_ns;     /* integer sum: the nanosecond carry is the division */
    struct timespec abs;
    abs.tv_sec  = (time_t)(abs_ns / NS_PER_SEC);
    abs.tv_nsec = (long)(abs_ns % NS_PER_SEC);
    return HW_COND_TIMEDWAIT_ABS(cv, lock, &abs);
#endif
}

int ps3_cond_wait_monotonic(pthread_cond_t* cv, pthread_mutex_t* lock,
                            const ps3_wait_budget* budget)
{
    if (budget->infinite) return HW_COND_WAIT(cv, lock);

    for (;;) {
        const uint64_t now = HW_NOW_NS();
        uint64_t left = budget_remaining_ns(budget, now);
        if (left == 0) return ETIMEDOUT;
        if (left > PS3_HW_MAX_SLICE_NS) left = PS3_HW_MAX_SLICE_NS;

        const int rc = wait_slice(cv, lock, now, left);
        if (rc == 0) return 0;
        /* A timed-out slice and an interrupted one both mean "ask the clock":
         * the next pass either finds the budget spent or sleeps for the rest.
         * Darwin's cond wait is not documented to return EINTR; handling it
         * keeps a platform that does from extending or shortening the budget. */
        if (rc != ETIMEDOUT && rc != EINTR) return rc;
    }
}

int ps3_mutex_lock_monotonic(pthread_mutex_t* lock, const ps3_wait_budget* budget)
{
    if (budget->infinite) return HW_MUTEX_LOCK(lock);

    uint64_t step = PS3_HW_MUTEX_STEP_MIN_NS;
    for (;;) {
        const int rc = HW_MUTEX_TRYLOCK(lock);
        if (rc != EBUSY) return rc;               /* acquired, or a real error */

        const uint64_t left = budget_remaining_ns(budget, HW_NOW_NS());
        if (left == 0) return ETIMEDOUT;

        const uint64_t nap = left < step ? left : step;
        if (nap < PS3_HW_MUTEX_YIELD_BELOW_NS) HW_YIELD();
        else                                   HW_SLEEP_NS(nap);
        if (step < PS3_HW_MUTEX_STEP_MAX_NS) {
            step *= 2;
            if (step > PS3_HW_MUTEX_STEP_MAX_NS) step = PS3_HW_MUTEX_STEP_MAX_NS;
        }
    }
}

#endif /* !_WIN32 */
