/*
 * ps3recomp - the host monotonic clock (see host_clock.h)
 */
#ifndef _WIN32

#include "host_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <stdatomic.h>
#endif

uint64_t host_monotonic_ns(void)
{
#ifdef __APPLE__
    /* sys_timer_usleep's precise path reads this clock on every guest thread, so the lazy init is atomic: the
     * values are the same whoever stores them, but a plain store racing a plain load is a data race. */
    static _Atomic uint32_t numer, denom;
    uint32_t d = atomic_load_explicit(&denom, memory_order_acquire);
    uint32_t n = atomic_load_explicit(&numer, memory_order_relaxed);
    if (!d) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        n = tb.numer; d = tb.denom;
        atomic_store_explicit(&numer, n, memory_order_relaxed);
        atomic_store_explicit(&denom, d, memory_order_release);
    }
    const uint64_t t = mach_absolute_time();
    /* Split so t * numer cannot overflow (numer is 125 on Apple silicon). */
    return (t / d) * n + (t % d) * n / d;
#else
    struct timespec ts;
    /* A failed read must not become "time zero": every relative wait would then
     * expire at once or never. CLOCK_MONOTONIC cannot fail on a host that has
     * it, so a failure is a broken platform, and the process stops. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fputs("host_monotonic_ns: clock_gettime(CLOCK_MONOTONIC) failed\n", stderr);
        abort();
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#endif /* !_WIN32 */
