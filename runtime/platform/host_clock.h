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
 * awake time.
 *
 * Where the host can pause the app (Apple: iOS keeps mach time running while
 * it suspends or deactivates the process) the clock is ACTIVE time: the raw
 * clock less everything spent between host_clock_pause and host_clock_resume.
 * Guest clocks therefore stand still while the host has paused the app and
 * continue from where they stood, so a title does not see a jump of the
 * paused duration. Relative host waits (host_wait; Apple uses relative
 * condition-variable waits) are not paused: they still elapse, so a thread
 * waiting on a guest deadline wakes, re-checks it against the frozen clock and
 * sleeps again. A reader never sees the clock step back, whichever threads
 * pause, resume and read. */
uint64_t host_monotonic_ns(void);

#ifdef __APPLE__
/* Freeze / continue the clock. Idempotent (pausing a paused clock, resuming a
 * running one, does nothing), any thread, meant for lifecycle edges, not for
 * a hot path. While paused every read returns the same value: the active time
 * at the pause.
 *
 * Declared on Apple only. On every other POSIX host host_monotonic_ns must stay
 * CLOCK_MONOTONIC, because host_wait.c hands its values to a CLOCK_MONOTONIC
 * condition variable as absolute deadlines; a clock that stops while the
 * kernel's keeps going would make those deadlines wrong, so there is nothing
 * to call rather than a call that fails. */
void host_clock_pause(void);
void host_clock_resume(void);

/* 1 while paused. Any thread; one atomic load. */
int host_clock_paused(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* PS3RECOMP_HOST_CLOCK_H */
