/*
 * ps3recomp - relative waits on the host monotonic clock (POSIX hosts)
 *
 * A guest timeout is a duration ("give up after N us"). pthread_cond_timedwait
 * takes an absolute time, and on a default condition variable that time is
 * CLOCK_REALTIME, so a wall-clock step during the wait (NTP, a manual change, a
 * VM resume) shortened or lengthened a relative guest timeout. These functions
 * measure the duration on host_monotonic_ns (host_clock.h) instead.
 *
 * The caller owns the predicate, the lock and the arbitration. The pattern for
 * every timed wait in the tree:
 *
 *     lock;
 *     ps3_wait_budget b = ps3_wait_budget_us(timeout_us, timeout_us == 0);
 *     while (!predicate) {
 *         int rc = ps3_cond_wait_monotonic(&cv, &lock, &b);
 *         if (rc == ETIMEDOUT) { decide, with the lock held, as the object always did }
 *         else if (rc != 0)    { a host error: unwind the object's bookkeeping }
 *     }
 *
 * The budget is built ONCE, outside the predicate loop, and handed to every
 * wait of that loop. Rebuilding it after a wake restarts the whole timeout.
 *
 * Windows keeps its native waits (SleepConditionVariable*, WaitFor*, QPC): none
 * of this exists there and the header declares nothing under _WIN32.
 */
#ifndef PS3RECOMP_HOST_WAIT_H
#define PS3RECOMP_HOST_WAIT_H

#ifndef _WIN32

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A duration that started at start_ns on the host monotonic clock. `infinite`
 * is a flag, never an overflowing duration: an infinite budget waits with
 * pthread_cond_wait / pthread_mutex_lock and never reads the clock again. */
typedef struct ps3_wait_budget {
    uint64_t start_ns;
    uint64_t duration_ns;
    int      infinite;
} ps3_wait_budget;

/* Start a budget now. A finite duration of zero is already expired: the first
 * wait returns ETIMEDOUT without sleeping. Whether a guest zero means "do not
 * wait" or "wait forever" is the caller's policy (lv2: forever, so it passes
 * infinite=1). usec * 1000 saturates at UINT64_MAX, a very long FINITE wait; it
 * never wraps to a short one and never becomes infinite. */
ps3_wait_budget ps3_wait_budget_ns(uint64_t nsec, int infinite);
ps3_wait_budget ps3_wait_budget_us(uint64_t usec, int infinite);

/* Initialise a condition variable that ps3_cond_wait_monotonic may wait on.
 * Linux: a CLOCK_MONOTONIC condattr, so the absolute deadline handed to
 * pthread_cond_timedwait is on the clock the budget runs on; there is no
 * fallback to the default (REALTIME) clock. macOS: an ordinary condition
 * variable, whose relative wait takes a duration. Returns 0 or the pthread
 * error; on failure `cv` holds nothing to destroy. A condition variable that
 * is only ever waited on without a timeout needs no special initialisation. */
int ps3_cond_init_monotonic(pthread_cond_t* cv);

/* One wait on `cv` with `lock` held, bounded by `budget`. Returns
 *   0          woken: a signal, a broadcast or a spurious wake. The predicate is
 *              the caller's to re-test; the budget keeps running across the next
 *              call, it is not restarted;
 *   ETIMEDOUT  the budget is spent (and only then: a wait that the kernel ended
 *              early, or capped, is retried against what is left);
 *   other      the host refused (EINVAL, EPERM...). Never turned into success
 *              or into a timeout.
 * `lock` is held on every return. */
int ps3_cond_wait_monotonic(pthread_cond_t* cv, pthread_mutex_t* lock,
                            const ps3_wait_budget* budget);

/* pthread_mutex_lock bounded by `budget`: try, then back off (at most 200 us,
 * never more than what is left) and try again. Returns 0 with the mutex held,
 * ETIMEDOUT with it not held, or the pthread error. A try that succeeds at the
 * boundary counts as acquired. The native mutex is the only arbiter, so
 * recursive and error-checking mutexes keep their own rules. */
int ps3_mutex_lock_monotonic(pthread_mutex_t* lock, const ps3_wait_budget* budget);

/* A process-lifetime condition variable that has timed waiters.
 * PTHREAD_COND_INITIALIZER cannot be used for one: on Linux it binds the
 * default, REALTIME, clock. This defines `static pthread_cond_t* accessor(void)`,
 * which initialises the variable exactly once (pthread_once) and returns it, or
 * NULL if that initialisation failed (the failure is remembered, never retried
 * over a live variable).
 *
 * EVERY user goes through the accessor, signallers and broadcasters as well as
 * waiters: a producer may arrive before the first wait. */
#define PS3_STATIC_MONOTONIC_COND(accessor)                                   \
    static pthread_cond_t accessor##_storage;                                 \
    static pthread_once_t accessor##_once = PTHREAD_ONCE_INIT;                \
    static int            accessor##_rc;                                      \
    static void accessor##_init(void)                                         \
    { accessor##_rc = ps3_cond_init_monotonic(&accessor##_storage); }         \
    static pthread_cond_t* accessor(void)                                     \
    {                                                                         \
        pthread_once(&accessor##_once, accessor##_init);                      \
        return accessor##_rc == 0 ? &accessor##_storage : NULL;               \
    }

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* PS3RECOMP_HOST_WAIT_H */
