/*
 * ps3recomp - the host monotonic clock
 *
 * One clock for every duration the runtime measures on a POSIX host: the PPU
 * timebase (every lifted mftb), sys_time_get_system_time, the lv2 timers'
 * expiration times and the relative waits of runtime/platform/host_wait.h. It
 * lives apart from runtime/syscalls/sys_timer.c so a standalone fixture that
 * needs the clock (a compatibility shim, a library, a wait test) does not pull
 * in the whole lv2 timer implementation.
 *
 * Windows reads QueryPerformanceCounter where it needs a clock; there is
 * nothing to declare there.
 */
#ifndef PS3RECOMP_HOST_CLOCK_H
#define PS3RECOMP_HOST_CLOCK_H

#ifndef _WIN32

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds on the host monotonic clock that every guest clock derives from
 * (the timebase, sys_time_get_system_time, timer expirations), so they stay
 * consistent with each other. On macOS this is mach_absolute_time: the mftb
 * site is hot in game loops, and clock_gettime(CLOCK_MONOTONIC) there goes
 * through _mach_boottime_usec + gettimeofday on every read. On every other
 * POSIX host it IS CLOCK_MONOTONIC, which is also the clock a monotonic
 * condition variable measures (host_wait.c relies on that identity).
 *
 * The clock does not run while the host sleeps on either platform (macOS
 * mach_absolute_time, Linux CLOCK_MONOTONIC): a relative wait is measured in
 * awake time. No claim is made about what a PS3 would do across a suspend. */
uint64_t host_monotonic_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* PS3RECOMP_HOST_CLOCK_H */
