/*
 * ps3recomp - Timer and time syscalls
 */

#ifndef SYS_TIMER_H
#define SYS_TIMER_H

#include "lv2_syscall_table.h"
#include "../ppu/ppu_context.h"
#include "../../include/ps3emu/ps3types.h"
#include "../../include/ps3emu/error_codes.h"

#include <stdint.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <pthread.h>
  #include <time.h>
  #include <unistd.h>
  #include "../platform/host_clock.h"   /* host_monotonic_ns, the clock every guest clock derives from */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* PS3 timebase frequency */
#define PS3_TIMEBASE_FREQ  79800000ULL

/* The guest mftb/mftbu clock: one global monotonic counter, host time scaled
 * to the PS3 timebase. Called from every lifted mftb site (ppu_lifter.py). */
uint64_t ppu_timebase_now(void);

/* Calls of sys_timer_usleep that slept on the macOS critical timer
 * (ppu.precise_usleep, sys_timer.c) and completed. It is how a test tells that
 * option off, a short sleep, or a host that refused the timer took nanosleep:
 * the thread state is the same on both paths. 0 on every other host. */
uint64_t sys_timer_usleep_precise_count(void);

#ifndef _WIN32
/* Sleep `ns` on the host for a runtime thread that keeps a cadence. On macOS
 * it takes the critical timer under the same rule as sys_timer_usleep
 * (ppu.precise_usleep, 20 us and up) without counting as a guest usleep;
 * otherwise nanosleep, resumed after a signal. */
void ps3_host_sleep_ns(uint64_t ns);

/* Sleep to `deadline_ns` on host_monotonic_ns: a timer sleep (as ps3_host_sleep_ns) to `deadline_ns - spin_ns`,
 * then a spin on the host clock to the deadline. A kernel timer wakes 20-50 us late on an M4 Pro, so a sleep to
 * the deadline itself ends after it; the spin buys the exact end for spin_ns of CPU. spin_ns 0 is a pure sleep.
 * Returns at once for a deadline that has passed. Not interruptible: the usleep seam's sleep also ends early to
 * deliver a GCM notification, this one does not. */
void ps3_host_sleep_until_ns(uint64_t deadline_ns, uint64_t spin_ns);

/* ---- usleep-site seam (ppu.poll_sleep_coalesce, default off) ----------------------------------------------
 * A title whose frame limiter polls usleep against a deadline it can compute registers the poll's syscall site; a
 * sys_timer_usleep from that site with the option on then sleeps once, to the deadline, instead of the guest's
 * request. POSIX hosts only: Windows keeps the poll (its guest clock has its own base, ppu_sysprx.cpp).
 *
 * `deadline` runs on every usleep whose guest return address is `lr`, on the calling guest thread, and returns the
 * absolute time on host_monotonic_ns at which the poll's own exit test would pass, or 0 for "no opinion" (guard
 * words not as expected, nothing to wait for). It must be quick and must not block or call into the guest. The
 * seam takes the deadline only if it lies in (now, now + max_sleep_ns]; 0, a deadline that has passed and one
 * beyond the bound all take the ordinary usleep(r3), and a deadline beyond the bound is logged once
 * ([usleep] poll-sleep-bound). While the seam sleeps, a pending GCM notification ends the wait early, is
 * delivered on that thread and the sleep resumes.
 *
 * One caller at a time: the whole seam, every registered site together, serves one sleeping call at a time in the
 * process (a title's limiter is one thread). A call that arrives while another sleeps, or one the delivered handler
 * makes from inside the sleep, takes the ordinary usleep(r3) without running its callback, and the first such call
 * is logged ([usleep] poll-sleep-contended).
 *
 * Register from one thread before the first guest thread runs (the table is read without a lock afterwards).
 * Returns 0, or -1 for an lr or bound of 0, a null callback, a duplicate lr or a full table (8 sites); nothing is
 * registered then. */
typedef uint64_t (*ps3_usleep_deadline_fn)(ppu_context* ctx);
int ps3_usleep_site_register(uint32_t lr, uint64_t max_sleep_ns, ps3_usleep_deadline_fn deadline);

/* sys_timer_usleep calls the seam slept to a deadline, for tests and diagnostics. */
uint64_t sys_timer_usleep_site_count(void);

/* ---- flip-wait sites (same option, same table, same one-sleeper rule) ----------------------------------------
 * A guest that waits for a flip with `do { status = cellGcmGetFlipStatus(); if (!status) break; usleep(n); }` has no
 * deadline to compute: what ends the loop is the flip status the toolkit holds (CELL_GCM_FLIP_STATUS_DONE, which a
 * GCM pump sets after the walker's cellGcmTickFlip). A title that has shown, from its binary, that a usleep site
 * is that loop and nothing else registers it here; a sys_timer_usleep from `lr` with the option on then sleeps until
 * the flip status is DONE or `cap_ns` has passed, whichever is first, instead of for the guest's argument, and
 * returns to the guest, which re-reads the status itself exactly as after any usleep. Nothing the guest reads or
 * writes is touched; what moves is the number of syscalls and the instant of the next read. The guest's next read
 * is by construction behind a pump (every HLE call and every syscall pumps first), so a flip completion the pump
 * would deliver is delivered before it.
 *
 * `guard` runs on every usleep whose guest return address is `lr`, on the calling guest thread, before anything
 * else, and returns nonzero only when the registers say this call is the loop the registration is for (syscall
 * number, the loop's own argument); 0 takes the ordinary usleep(r3). It must be quick, must not block and must not
 * call into the guest; it is where a title fails closed on a state it did not expect.
 *
 * The wait is also ended by any GCM notification (vblank, flip, user command), which is pumped on the sleeping
 * thread before the status is looked at again, so handlers are delivered as soon as a poll would deliver them; the
 * status is looked at after every pump, so a completion is never waited out. `cap_ns` bounds one call (a wake
 * that never comes, a host that stopped delivering flips, an app in the background) and is what keeps the guest
 * polling at 1/cap instead of 1/n. A flip that completed before the call began returns at once. The seam needs the
 * GCM module (the status and the waker): without it every such call takes the ordinary usleep.
 *
 * Returns 0, or -1 for an lr or cap of 0, a null guard, a duplicate lr or a full table (8 sites, shared with
 * ps3_usleep_site_register). Register from one thread before the first guest thread runs. */
typedef int (*ps3_usleep_guard_fn)(ppu_context* ctx);
int ps3_usleep_flip_wait_register(uint32_t lr, uint64_t cap_ns, ps3_usleep_guard_fn guard);

/* sys_timer_usleep calls a flip-wait site took, for tests and diagnostics. */
uint64_t sys_timer_usleep_flip_wait_count(void);
#endif

#ifdef _WIN32
/* Sub-millisecond timed-wait support for the lv2 sync primitives.
 * SleepConditionVariableCS / WaitForSingleObject floor a timed wait to 1 ms,
 * and the OS then rounds that up to the timer tick -- a 30 us guest timeout
 * inflates ~50x even with timeBeginPeriod(1). Waits under 1 ms must instead
 * poll their wake predicate and yield to a QueryPerformanceCounter deadline
 * (same approach sys_timer_usleep already uses). */
int64_t lv2_usec_deadline(uint64_t usec);      /* QPC value 'usec' from now */
int     lv2_deadline_passed(int64_t deadline); /* 1 once QPC >= deadline */
#endif

#define SYS_TIMER_MAX  64

typedef struct sys_timer_info {
    int      active;
    int      running;
    uint64_t period_usec;
    int32_t  event_queue_id;   /* connected event queue */
    uint64_t source;           /* event source value */
    uint64_t data1;            /* event data1 */
    uint64_t data2;            /* event data2 (connect's fifth argument) */
    uint64_t next_expire;      /* next expiration, us on the timer clock */

#ifdef _WIN32
    HANDLE   timer_handle;
    HANDLE   stop_event;
    HANDLE   thread_handle;
#else
    pthread_t       thread;
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    int             stop_flag;
#endif

} sys_timer_info;

extern sys_timer_info g_sys_timers[SYS_TIMER_MAX];

/* Syscall handlers */
int64_t sys_timer_usleep(ppu_context* ctx);
int64_t sys_timer_sleep(ppu_context* ctx);
int64_t sys_time_get_current_time(ppu_context* ctx);
int64_t sys_time_get_timezone(ppu_context* ctx);
int64_t sys_time_get_timebase_frequency(ppu_context* ctx);
int64_t sys_timer_create(ppu_context* ctx);
int64_t sys_timer_destroy(ppu_context* ctx);
int64_t sys_timer_start(ppu_context* ctx);
int64_t sys_timer_stop(ppu_context* ctx);
int64_t sys_timer_connect_event_queue(ppu_context* ctx);
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx);

/* Registration */
void sys_timer_init(lv2_syscall_table* tbl);

#ifdef __cplusplus
}
#endif

#endif /* SYS_TIMER_H */
