/*
 * ps3recomp - Timer and time syscalls (implementation)
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* tm_gmtoff on glibc */
#endif
#include "sys_timer.h"
#include "sys_ppu_thread.h"   /* ppu_thread_note_block */
#include <time.h>
#include "sys_event.h"
#include "../memory/vm.h"
#include <stdlib.h>   /* getenv -- an implicit decl returns int, truncating the pointer */
#include <string.h>
#include <stdlib.h>
#include "ps3emu/log_first.h"
#include "../../include/ps3emu/ps3_log.h"
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <sys/event.h>
#endif
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#endif
#ifndef _WIN32
#include <sched.h>    /* sched_yield: the opt-in short-usleep yield */
#endif
#include "ps3emu/thread_name.h"
#ifndef _WIN32
#include "../platform/host_wait.h"   /* the period wait of the timer thread */
#include "ps3emu/stop.h"             /* ps3_stop_host_wait_error */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
sys_timer_info g_sys_timers[SYS_TIMER_MAX];

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

int64_t lv2_usec_deadline(uint64_t usec)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    return now.QuadPart +
        (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
}

int lv2_deadline_passed(int64_t deadline)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= deadline;
}
#endif

/* ---------------------------------------------------------------------------
 * PPU timebase (mftb/mftbu) -- THE guest clock.
 *
 * One global monotonic counter scaled to the PS3 timebase (79.8 MHz),
 * counted from host boot. The old lifter emission was a per-call-site
 * static that advanced 16667 ticks PER READ -- not a clock at all: every
 * guest timing loop (media pacers, throttles, profilers) computed garbage
 * from it, and each call site had a PRIVATE counter that only moved when
 * polled. Overflow-safe split multiply (rem < qpf, so
 * rem * 79.8e6 < ~8e14 << 2^63).
 * -----------------------------------------------------------------------*/
uint64_t ppu_timebase_now(void)
{
#ifdef _WIN32
    /* Counted from HOST boot, as the PS3 timebase counts from console boot
     * and as the POSIX path below does. A per-process anchor made every
     * launch start at 0, so the upper word read 0 for the first ~54 s and
     * clock-derived seeds repeated run to run. */
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = (uint64_t)now.QuadPart;
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * PS3_TIMEBASE_FREQ + (d % q) * PS3_TIMEBASE_FREQ / q;
#else
    const uint64_t ns = host_monotonic_ns();
    return (ns / 1000000000ull) * PS3_TIMEBASE_FREQ +
           (ns % 1000000000ull) * PS3_TIMEBASE_FREQ / 1000000000ull;
#endif
}

/* Microseconds of guest system time (sys_time_get_system_time), counted from
 * host boot on every host. cellGcmGetLastFlipTime stamps flips with it too:
 * titles pace frames as period - (system time - last flip time), so the two
 * must share clock and unit (the flip stamp used to be absolute QPC
 * nanoseconds, which made that difference meaningless and every such limiter
 * sleep its whole period). */
uint64_t ps3_system_time_us(void)
{
#ifdef _WIN32
    /* Time since host boot (#788), as on POSIX: anchoring at the first call
     * gave every launch the same small values and so the same RNG seeds. */
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = (uint64_t)now.QuadPart;
    uint64_t q = (uint64_t)s_qpc_freq.QuadPart;
    return (d / q) * 1000000ull + (d % q) * 1000000ull / q;
#else
    return host_monotonic_ns() / 1000ull;
#endif
}

static void write_be32(uint32_t addr, uint32_t val)
{
    uint32_t* p = (uint32_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 24) & 0xFF) | ((val >> 8) & 0xFF00) |
          ((val <<  8) & 0xFF0000) | ((val << 24) & 0xFF000000u);
#endif
    *p = val;
}

static void write_be64(uint32_t addr, uint64_t val)
{
    uint64_t* p = (uint64_t*)vm_to_host(addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
    val = ((val >> 56) & 0xFFULL) |
          ((val >> 40) & 0xFF00ULL) |
          ((val >> 24) & 0xFF0000ULL) |
          ((val >>  8) & 0xFF000000ULL) |
          ((val <<  8) & 0xFF00000000ULL) |
          ((val << 24) & 0xFF0000000000ULL) |
          ((val << 40) & 0xFF000000000000ULL) |
          ((val << 56) & 0xFF00000000000000ULL);
#endif
    *p = val;
}

#ifdef __APPLE__
/* ---------------------------------------------------------------------------
 * Precise usleep on macOS (ppu.precise_usleep)
 *
 * nanosleep, mach_wait_until and an ordinary kevent timer are all coalesced by
 * the kernel, which lets a timer fire late by a fraction of its length: a
 * quarter of the request up to a cap of 4-5 ms (p50 on an M4 Pro, macOS 27:
 * 29 us at 100 us, 0.51 ms at 2 ms, 2.5 ms at 10 ms, 4.0 ms at 16 ms). The
 * console's usleep is accurate, so a guest that paces itself with
 * usleep(period - elapsed) misses its deadline by the leeway. A kevent
 * EVFILT_TIMER with NOTE_CRITICAL is exempt from the coalescing: p50 overshoot
 * 20-50 us from 0.1 to 33 ms (scratch/m4/usleep/MEASURE.md in the recon tree).
 *
 * It is used instead of putting the thread into THREAD_TIME_CONSTRAINT_POLICY
 * for the length of the sleep, which is equally accurate but not reversible:
 * thread_policy_set(TIME_CONSTRAINT) drops the thread's QoS class (0x21 ->
 * 0x00), THREAD_STANDARD_POLICY does not bring it back, and
 * pthread_set_qos_class_self_np then fails with EPERM for the rest of the
 * thread's life. NOTE_CRITICAL changes no thread state, so there is nothing to
 * restore (the QoS class win32_compat.c's SetThreadPriority set stays as it
 * was).
 *
 * The kqueue is created and closed inside the call: no per-thread state, so no
 * fd to leak when a guest thread ends and nothing stale in a forked child. It
 * costs 2 extra syscalls, ~0.45 us in all against a sleep of at least 20 us.
 *
 * Below USLEEP_PRECISE_MIN_US nanosleep keeps the sleep: at 20 us the two are
 * within 4 us of each other (7 us over against 3.6 us), and at 1 us the kevent
 * timer is the slower (15 us over against 1.9 us).
 *
 * Windows (waitable timer, QPC busy tail) and Linux (nanosleep with the 50 us
 * default timerslack) keep their paths.
 * -----------------------------------------------------------------------*/
enum { USLEEP_PRECISE_MIN_US = 20 };
/* NOTE_NSECONDS carries the interval in an intptr_t. */
#define USLEEP_PRECISE_MAX_US ((uint64_t)INT64_MAX / 1000u)

static _Atomic uint64_t s_precise_usleeps;

uint64_t sys_timer_usleep_precise_count(void)
{
    return atomic_load_explicit(&s_precise_usleeps, memory_order_relaxed);
}

static int precise_usleep_enabled(void)
{
    /* The typed configuration is immutable once loaded: every thread stores
     * the same value. */
    static atomic_int cached;
    int value = atomic_load_explicit(&cached, memory_order_relaxed);
    if (!value) {
        value = ps3_cfg_ppu_precise_usleep() ? 2 : 1;
        atomic_store_explicit(&cached, value, memory_order_relaxed);
    }
    return value == 2;
}

/* A host aid, not guest-visible behaviour: a failure must not stop the title,
 * and must not be silent. The call that failed sleeps the rest of its time with
 * nanosleep, and the first failure of the process says so. */
static void precise_usleep_failed(const char* what, int err)
{
    static _Atomic int warned;
    if (atomic_exchange_explicit(&warned, 1, memory_order_relaxed)) return;
    PS3_LOG(PS3_LOG_INFO, "usleep", "precise-usleep-fallback: %s failed (errno %d: %s); sleeping with nanosleep "
                    "instead, which oversleeps by the host's timer leeway. Later failures are not reported "
                    "(ppu.precise_usleep=false selects nanosleep on purpose).\n",
            what, err, strerror(err));
}

/* Sleeps `ns` on a critical timer. Returns the nanoseconds still to sleep: 0
 * once the deadline taken at entry has passed, `ns` or less when the host
 * would not give a timer (the caller finishes with nanosleep). */
static uint64_t sleep_critical_timer(uint64_t ns)
{
    const uint64_t deadline = host_monotonic_ns() + ns;
    const int kq = kqueue();
    if (kq < 0) { precise_usleep_failed("kqueue", errno); return ns; }
    uint64_t left = ns;
    for (;;) {
        struct kevent change, fired;
        EV_SET(&change, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (intptr_t)left, NULL);
        const int n = kevent(kq, &change, 1, &fired, 1, NULL);
        if (n == 1 && !(fired.flags & EV_ERROR)) { left = 0; break; }
        const int err = n == 1 ? (int)fired.data : errno;
        /* A signal ends the wait early; the sleep runs to the deadline, as the
         * absolute-deadline contract of the other hosts' paths does. */
        const uint64_t now = host_monotonic_ns();
        left = now >= deadline ? 0 : deadline - now;
        if (n < 0 && err == EINTR && left) continue;
        if (left) precise_usleep_failed("kevent", err);
        break;
    }
    close(kq);
    return left;
}
#else
uint64_t sys_timer_usleep_precise_count(void) { return 0; }
#endif /* __APPLE__ */
#ifndef _WIN32
/* Host-side sleep for runtime threads that keep a cadence (the cellAudio mixer).
 * It takes the same coalescing-free timer as sys_timer_usleep, but it is not a
 * guest usleep, so it leaves s_precise_usleeps alone. */
void ps3_host_sleep_ns(uint64_t ns)
{
    if (ns == 0) return;
#ifdef __APPLE__
    if (ns >= (uint64_t)USLEEP_PRECISE_MIN_US * 1000u && ns <= (uint64_t)USLEEP_PRECISE_MAX_US * 1000u &&
        precise_usleep_enabled()) {
        ns = sleep_critical_timer(ns);
        if (!ns) return;
    }
#endif
    struct timespec ts;
    ts.tv_sec  = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}
#endif

#ifndef _WIN32
/* ---------------------------------------------------------------------------
 * Usleep-site seam (ppu.poll_sleep_coalesce)
 *
 * A guest limiter that polls usleep(30) against a deadline pays one syscall, two GCM pumps, one clock read and
 * one kernel timer per poll: 350-400 a frame, ~26000 host wakeups a second. The title can compute the deadline
 * the poll ends at; the toolkit cannot (it sees the sleeps, not the loop's exit test). The title registers the
 * return address of the poll's syscall with a callback that returns that deadline, and the toolkit sleeps to it
 * once. Everything the guest does around the sleep stays guest code: the branch that skips the wait on a late
 * frame, the exit test, the stamp it stores. Only the number of clock reads and the instant the loop ends move.
 *
 * Bounded by construction: a deadline is taken only inside (now, now + max_sleep_ns]. A wrong callback (a stale
 * or garbled word read from guest memory) then costs at most one bounded sleep, never a hang, and is reported.
 *
 * The deadline is on host_monotonic_ns, the clock every POSIX guest clock derives from. Windows keeps the poll:
 * its guest clock is QueryPerformanceCounter with a base of its own (ppu_sysprx.cpp), which a title cannot turn
 * into a host deadline without a base this file does not export. That is a documented gap, not a faked seam.
 * -----------------------------------------------------------------------*/
enum { USLEEP_SITES_MAX = 8 };
typedef struct {
    uint32_t               lr;
    uint64_t               max_sleep_ns;   /* a deadline site's bound; a flip-wait site's cap */
    ps3_usleep_deadline_fn deadline;       /* a deadline site: the poll's exit instant */
    ps3_usleep_guard_fn    guard;          /* a flip-wait site: set, with deadline NULL */
} usleep_site;
/* Written by the registering thread before any guest thread runs, then read-only; the count is published with
 * release so a reader that sees a slot sees it whole. */
static usleep_site s_sites[USLEEP_SITES_MAX];
static atomic_uint s_site_count;
static atomic_uint_fast64_t s_site_sleeps;
static atomic_uint_fast64_t s_flip_wait_sleeps;

static int site_register(uint32_t lr, uint64_t max_sleep_ns, ps3_usleep_deadline_fn deadline,
                         ps3_usleep_guard_fn guard)
{
    const unsigned n = atomic_load_explicit(&s_site_count, memory_order_relaxed);
    if (n >= USLEEP_SITES_MAX) return -1;
    for (unsigned i = 0; i < n; i++) if (s_sites[i].lr == lr) return -1;
    s_sites[n].lr = lr;
    s_sites[n].max_sleep_ns = max_sleep_ns;
    s_sites[n].deadline = deadline;
    s_sites[n].guard = guard;
    atomic_store_explicit(&s_site_count, n + 1, memory_order_release);
    return 0;
}

int ps3_usleep_site_register(uint32_t lr, uint64_t max_sleep_ns, ps3_usleep_deadline_fn deadline)
{
    if (!lr || !max_sleep_ns || !deadline) return -1;
    return site_register(lr, max_sleep_ns, deadline, NULL);
}

int ps3_usleep_flip_wait_register(uint32_t lr, uint64_t cap_ns, ps3_usleep_guard_fn guard)
{
    if (!lr || !cap_ns || !guard) return -1;
    return site_register(lr, cap_ns, NULL, guard);
}

uint64_t sys_timer_usleep_site_count(void)
{
    return (uint64_t)atomic_load_explicit(&s_site_sleeps, memory_order_relaxed);
}

uint64_t sys_timer_usleep_flip_wait_count(void)
{
    return (uint64_t)atomic_load_explicit(&s_flip_wait_sleeps, memory_order_relaxed);
}

/* The pump the lv2 dispatcher runs at a usleep syscall (ppu_loader.cpp) and the waker's installer, both in
 * cellGcmSys.c when it is linked. Weak, as ppu_hle.cpp holds ppu_gcm_pump: a link without GCM has nothing to pump
 * and nothing that posts. */
extern void ppu_gcm_pump(void) __attribute__((weak));
/* 1 once the flip status is CELL_GCM_FLIP_STATUS_DONE: what a flip-wait site sleeps until. */
extern int ppu_gcm_flip_done(void) __attribute__((weak));
extern void ppu_gcm_set_wake_hook(void (*hook)(void)) __attribute__((weak));

static void site_pump(void)
{
    if (ppu_gcm_pump) ppu_gcm_pump();
}

static void site_wait_failed(const char* what, int err)
{
    PS3_LOG_N(1, PS3_LOG_INFO, "usleep", "poll-sleep-wait-failed: %s failed (errno %d: %s); the seam sleeps this "
              "call without waking for GCM notifications, which are then delivered at the thread's next HLE call "
              "or syscall.\n", what, err, strerror(err));
}

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static void spin_until_ns(uint64_t deadline_ns)
{
    while (host_monotonic_ns() < deadline_ns) cpu_relax();
}

void ps3_host_sleep_until_ns(uint64_t deadline_ns, uint64_t spin_ns)
{
    const uint64_t sleep_to = deadline_ns > spin_ns ? deadline_ns - spin_ns : 0;
    const uint64_t now = host_monotonic_ns();
    if (now < sleep_to) ps3_host_sleep_ns(sleep_to - now);
    spin_until_ns(deadline_ns);
}

/* ---- the interruptible wait ----------------------------------------------------------------------------------
 * The seam sleeps a frame in one wait, so a GCM notification (vblank, flip, user command: ~128 user commands a
 * second in the title this was built for) posted meanwhile must end it, or its handler would wait for the next
 * HLE call on any thread instead of at most 35 us. The poster calls usleep_wake (installed into cellGcmSys.c);
 * the sleeper pumps and sleeps on to its deadline.
 *
 * No lost wake, on either host: the sleeper announces itself, fences, then pumps once; the poster publishes,
 * fences, then looks for a sleeper. Whichever side the fences order first, either the sleeper's pump sees the
 * work or the poster sees the sleeper and wakes it. A wake is an edge: the sleeper never loops on "something is
 * pending", because with gcm.handler_thread the pump leaves it pending for the interrupt thread to take.
 *
 * Both waits return 0 once the clock has passed `until_ns` (or, for a flip-wait, once its `done` predicate holds),
 * or -1 with no wake armed, in which case the caller sleeps plainly (a flip-wait takes the ordinary usleep). The
 * predicate is read after every pump, never instead of one: what makes it true (a pump marking the flip DONE, in
 * cellGcmSys.c) calls the waker itself, so a completion on any thread, the sleeper's own pump included, ends the
 * wait. The sleeper's own pump leaves one stale trigger in the macOS kqueue; the next wait takes it as a spurious
 * wake (one pump, one look at the predicate) and sleeps on. */
#ifdef __APPLE__
/* One kqueue, made at the first wait: an EVFILT_USER that the poster triggers, plus the NOTE_CRITICAL timer of
 * sleep_critical_timer (the only timer the kernel does not coalesce). A kqueue per sleep, as the guest's plain
 * usleep does, would give the poster nothing to trigger. It has one sleeper at a time (s_site_busy). */
enum { WAKE_USER_IDENT = 1, WAKE_TIMER_IDENT = 2 };
static atomic_int  s_wake_kq = -1;

static atomic_int  s_wake_waiting;

static int wake_kq(void)
{
    int kq = atomic_load_explicit(&s_wake_kq, memory_order_acquire);
    if (kq >= 0) return kq;
    kq = kqueue();
    if (kq < 0) { site_wait_failed("kqueue", errno); return -1; }
    (void)fcntl(kq, F_SETFD, FD_CLOEXEC);
    struct kevent ch;
    EV_SET(&ch, WAKE_USER_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(kq, &ch, 1, NULL, 0, NULL) < 0) { site_wait_failed("kevent", errno); close(kq); return -1; }
    atomic_store_explicit(&s_wake_kq, kq, memory_order_release);   /* only the flag's owner gets here */
    return kq;
}

static void usleep_wake(void)
{
    atomic_thread_fence(memory_order_seq_cst);
    if (!atomic_load_explicit(&s_wake_waiting, memory_order_relaxed)) return;
    struct kevent ch;
    EV_SET(&ch, WAKE_USER_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    (void)kevent(atomic_load_explicit(&s_wake_kq, memory_order_acquire), &ch, 1, NULL, 0, NULL);
}

/* `done`, when set, is the early exit of a flip-wait: looked at after every pump and before every sleep, so a
 * state change published before the sleeper announced itself is seen by the first look and one published after
 * it is seen by the poster, which then finds the sleeper (the two fences above). */
typedef int (*wait_done_fn)(void);

static int wake_sleep_until(uint64_t until_ns, wait_done_fn done)
{
    const int kq = wake_kq();   /* one sleeper at a time: usleep_site_sleep holds s_site_busy around this */
    if (kq < 0) return -1;
    atomic_store_explicit(&s_wake_waiting, 1, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    site_pump();
    int rc = 0, armed = 0;
    for (;;) {
        if (done && done()) break;
        const uint64_t now = host_monotonic_ns();
        if (now >= until_ns) break;
        const uint64_t left = until_ns - now;
        if (left < (uint64_t)USLEEP_PRECISE_MIN_US * 1000u) { ps3_host_sleep_ns(left); break; }
        const uint64_t chunk = left > (uint64_t)USLEEP_PRECISE_MAX_US * 1000u ? (uint64_t)USLEEP_PRECISE_MAX_US * 1000u : left;
        struct kevent ch, ev;
        EV_SET(&ch, WAKE_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (intptr_t)chunk, NULL);
        armed = 1;
        const int n = kevent(kq, &ch, 1, &ev, 1, NULL);
        if (n == 1 && (ev.flags & EV_ERROR)) { site_wait_failed("kevent", (int)ev.data); rc = -1; break; }
        if (n == 1 && ev.filter == EVFILT_TIMER) armed = 0;
        else if (n == 1) site_pump();           /* the poster's trigger: deliver, then sleep on to the deadline */
        else if (n < 0 && errno != EINTR) { site_wait_failed("kevent", errno); rc = -1; break; }
    }
    if (armed) {   /* left on a wake with the timer still armed: it must not fire into the next sleep */
        struct kevent del;
        EV_SET(&del, WAKE_TIMER_IDENT, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        (void)kevent(kq, &del, 1, NULL, 0, NULL);
    }
    atomic_store_explicit(&s_wake_waiting, 0, memory_order_relaxed);
    return rc;
}
#else
/* Linux and other POSIX hosts: a monotonic condition variable. Their timer leeway is the 50 us timerslack, which
 * the spin margin absorbs, so a timed condition wait is accurate enough and there is nothing to coalesce. */
static pthread_mutex_t s_wake_mx = PTHREAD_MUTEX_INITIALIZER;
PS3_STATIC_MONOTONIC_COND(wake_cv)
static uint64_t   s_wake_seq;       /* guarded by s_wake_mx */
static atomic_int s_wake_waiters;

static void usleep_wake(void)
{
    atomic_thread_fence(memory_order_seq_cst);
    if (!atomic_load_explicit(&s_wake_waiters, memory_order_relaxed)) return;
    pthread_cond_t* cv = wake_cv();
    if (!cv) return;   /* no waiter can exist: wake_sleep_until refuses without it */
    pthread_mutex_lock(&s_wake_mx);
    s_wake_seq++;
    pthread_cond_broadcast(cv);
    pthread_mutex_unlock(&s_wake_mx);
}

typedef int (*wait_done_fn)(void);   /* see the macOS wait: the flip-wait's early exit */

static int wake_sleep_until(uint64_t until_ns, wait_done_fn done)
{
    pthread_cond_t* cv = wake_cv();
    if (!cv) { site_wait_failed("pthread_cond_init", EINVAL); return -1; }
    atomic_fetch_add_explicit(&s_wake_waiters, 1, memory_order_seq_cst);
    atomic_thread_fence(memory_order_seq_cst);
    pthread_mutex_lock(&s_wake_mx);
    uint64_t seen = s_wake_seq;
    pthread_mutex_unlock(&s_wake_mx);
    site_pump();
    int rc = 0;
    for (;;) {
        if (done && done()) break;
        const uint64_t now = host_monotonic_ns();
        if (now >= until_ns) break;
        const ps3_wait_budget budget = ps3_wait_budget_ns(until_ns - now, 0);
        int w = 0;
        pthread_mutex_lock(&s_wake_mx);
        while (s_wake_seq == seen && (w = ps3_cond_wait_monotonic(cv, &s_wake_mx, &budget)) == 0) {}
        const int woken = s_wake_seq != seen;
        seen = s_wake_seq;
        pthread_mutex_unlock(&s_wake_mx);
        if (w != 0 && w != ETIMEDOUT) { site_wait_failed("pthread_cond_timedwait", w); rc = -1; break; }
        if (woken) site_pump();
    }
    atomic_fetch_sub_explicit(&s_wake_waiters, 1, memory_order_relaxed);
    return rc;
}
#endif /* __APPLE__ */

/* Sleeps to deadline_ns with the wake armed, then spins the last spin_ns. */
static void site_sleep_until(uint64_t deadline_ns, uint64_t spin_ns)
{
    const uint64_t sleep_to = deadline_ns > spin_ns ? deadline_ns - spin_ns : 0;
    if (wake_sleep_until(sleep_to, NULL) != 0) {
        const uint64_t now = host_monotonic_ns();
        if (now < sleep_to) ps3_host_sleep_ns(sleep_to - now);
    }
    spin_until_ns(deadline_ns);
}

/* The typed configuration is immutable once loaded: every thread stores the same values. State 0 = unread. */
static atomic_int      s_seam_state;
static atomic_uint_fast64_t s_seam_spin_ns;

static int seam_enabled(void)
{
    int state = atomic_load_explicit(&s_seam_state, memory_order_acquire);
    if (state) return state == 2;
    const int on = ps3_cfg_ppu_poll_sleep_coalesce() ? 1 : 0;
    atomic_store_explicit(&s_seam_spin_ns, (uint64_t)ps3_cfg_ppu_poll_sleep_spin_us() * 1000u, memory_order_relaxed);
    if (on && ppu_gcm_set_wake_hook) ppu_gcm_set_wake_hook(usleep_wake);
    atomic_store_explicit(&s_seam_state, on ? 2 : 1, memory_order_release);
    return on;
}

/* One seam sleep at a time in the process (the contract of ps3_usleep_site_register): the interruptible wait is a
 * single wake source that a second concurrent sleeper would either share unsafely or sit out of, deaf to the
 * notifications the first one's wake is for. The title's limiter is one thread. A second caller that arrives while
 * one sleeps, or one the delivered handler makes from inside the sleep, takes the ordinary usleep. The flag is
 * taken before the deadline callback, so a callback only ever runs in the caller that may then sleep. */
static atomic_flag s_site_busy = ATOMIC_FLAG_INIT;

static int usleep_site_sleep_held(ppu_context* ctx, const usleep_site* site)
{
    const uint64_t deadline = site->deadline(ctx);
    if (!deadline) return 0;
    const uint64_t now = host_monotonic_ns();
    if (deadline <= now) return 0;
    if (deadline - now > site->max_sleep_ns) {
        PS3_LOG_N(1, PS3_LOG_INFO, "usleep", "poll-sleep-bound: the site at lr=0x%08X asked for a deadline %llu us "
                  "away, beyond its bound of %llu us; this call sleeps as usleep(%llu) does. Later calls are not "
                  "reported.\n", site->lr, (unsigned long long)((deadline - now) / 1000u),
                  (unsigned long long)(site->max_sleep_ns / 1000u), (unsigned long long)LV2_ARG_U64(ctx, 0));
        return 0;
    }
    site_sleep_until(deadline, atomic_load_explicit(&s_seam_spin_ns, memory_order_relaxed));
    atomic_fetch_add_explicit(&s_site_sleeps, 1, memory_order_relaxed);
    return 1;
}

/* A flip-wait site: sleep until the flip status is DONE or the site's cap has passed. Returns 1 when the wait ran
 * and the guest may re-read the status, 0 to take the ordinary usleep (the guard declined, there is no GCM module
 * to ask or to wake us, or the host's wait failed: fail closed, the poll goes on exactly as without the seam). */
static int usleep_flip_wait_held(ppu_context* ctx, const usleep_site* site)
{
    if (!site->guard(ctx)) return 0;
    if (!ppu_gcm_flip_done || !ppu_gcm_set_wake_hook) return 0;
    if (wake_sleep_until(host_monotonic_ns() + site->max_sleep_ns, ppu_gcm_flip_done) != 0) return 0;
    atomic_fetch_add_explicit(&s_flip_wait_sleeps, 1, memory_order_relaxed);
    return 1;
}

/* 1 when the call was a registered site's and was slept to its deadline. */
static int usleep_site_sleep(ppu_context* ctx)
{
    const unsigned n = atomic_load_explicit(&s_site_count, memory_order_acquire);
    const usleep_site* site = NULL;
    for (unsigned i = 0; i < n; i++)
        if (s_sites[i].lr == (uint32_t)ctx->lr) { site = &s_sites[i]; break; }
    if (!site || !seam_enabled()) return 0;
    if (atomic_flag_test_and_set_explicit(&s_site_busy, memory_order_acquire)) {
        PS3_LOG_N(1, PS3_LOG_INFO, "usleep", "poll-sleep-contended: a second call at the site lr=0x%08X arrived while "
                  "another seam sleep was running; it sleeps as usleep(%llu) does (the seam serves one caller at a "
                  "time). Later calls are not reported.\n", site->lr, (unsigned long long)LV2_ARG_U64(ctx, 0));
        return 0;
    }
    const int slept = site->guard ? usleep_flip_wait_held(ctx, site) : usleep_site_sleep_held(ctx, site);
    atomic_flag_clear_explicit(&s_site_busy, memory_order_release);
    return slept;
}
#endif /* !_WIN32 */


/* ---------------------------------------------------------------------------
 * sys_timer_usleep
 *
 * r3 = microseconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_usleep(ppu_context* ctx)
{
    uint64_t usec = LV2_ARG_U64(ctx, 0);
    ppu_thread_note_block(ctx->thread_id);   /* PS3_PRIO_START */
    { extern unsigned long long ps3_qpc_us(void);
      PS3_LOG_N(60, PS3_LOG_INFO, "WAIT", "t=%lluus timer_usleep(%llu us) lr=0x%08llX cia=0x%08llX\n", ps3_qpc_us(),
        (unsigned long long)usec, (unsigned long long)ctx->lr, (unsigned long long)ctx->cia); }
    /* PS3_POLLTOP=<seconds>: name the usleep poll sites.
     *
     * A title that is "hung" is usually not blocked on anything -- every thread
     * is spinning on sys_timer_usleep waiting for a flag, and [BLOCKSUM] only
     * says "syscall 141, 12s". This histograms the callers over a window and
     * prints the top ones, so the answer is a guest address rather than a
     * count. Keyed on ctx->lr: a host-stack backtrace through lifted code has
     * no unwind tables and invents chains (see the Guitar Hero III writeup),
     * and lr is the only guest chain that survives.
     *
     * Deliberately unlocked: the counts are a diagnostic, and a lost increment
     * under a race cannot change which site is at the top of a 30k-per-second
     * poll. */
    { static long s_pt = -1;
      if (s_pt < 0) { const char* e = getenv("PS3_POLLTOP");
                      s_pt = e ? (long)strtoul(e, 0, 10) : 0;
                      if (s_pt < 0) s_pt = 0; }
      if (s_pt > 0) {
        enum { PT_MAX = 128 };
        static uint32_t pt_lr[PT_MAX]; static uint32_t pt_n[PT_MAX];
        static uint64_t pt_us[PT_MAX]; static int pt_used = 0;
        static uint32_t pt_tid[PT_MAX];
        static unsigned long long pt_win = 0;
        uint32_t lr = (uint32_t)ctx->lr;
        int i = 0;
        for (; i < pt_used; i++) if (pt_lr[i] == lr) break;
        if (i == pt_used && pt_used < PT_MAX) {
            pt_lr[pt_used] = lr;
            /* The thread matters as much as the site: "who is waiting" is the
             * half that says whether a poll is the engine idling or the main
             * thread stuck. */
            pt_tid[pt_used] = (uint32_t)ctx->thread_id;
            pt_used++;
        }
        if (i < PT_MAX) { pt_n[i]++; pt_us[i] += usec; }
        { extern unsigned long long ps3_qpc_us(void);
          unsigned long long now = ps3_qpc_us();
          if (!pt_win) pt_win = now;
          else if (now - pt_win >= (unsigned long long)s_pt * 1000000ull) {
            fprintf(stderr, "--- usleep poll sites (last %lds) ---\n", s_pt);
            for (int k = 0; k < 8; k++) {
              int best = -1;
              for (int j = 0; j < pt_used; j++)
                if (pt_n[j] && (best < 0 || pt_n[j] > pt_n[best])) best = j;
              if (best < 0) break;
              fprintf(stderr, "  %8u calls  %6llums slept  tid=%-3u lr=0x%08X\n",
                      pt_n[best], (unsigned long long)(pt_us[best] / 1000),
                      pt_tid[best], pt_lr[best]);
              pt_n[best] = 0;
            }
            fflush(stderr);
            for (int j = 0; j < pt_used; j++) { pt_n[j] = 0; pt_us[j] = 0; }
            pt_win = now;
          } } } }

    /* PS3_WAIT_OBJ=<lr-hex>: when a usleep spin is reached from this return
     * address, dump the registers and the object they point at. A poll loop
     * tells you WHERE it is spinning; this tells you WHAT it is spinning on,
     * which is the part you actually need to find who never releases it. */
    { static long s_wo = -1;
      if (s_wo < 0) { const char* e = getenv("PS3_WAIT_OBJ");
                      s_wo = e ? (long)strtoul(e, 0, 16) : 0; }
      if (s_wo && (uint32_t)ctx->lr == (uint32_t)s_wo) {
        static int _n = 0;
        if (PS3_LOG_FIRST(_n, 8)) {
          /* Dump the words at EVERY plausible object register, not just r29.
           * Which register holds the object is per-title -- r29 was right for
           * the title this was written for and is a spin COUNTER in Guitar
           * Hero III, where the object is in r30/r31. Printing one guess makes
           * the tool silently report the wrong memory as "the thing it waits
           * on", which is worse than printing nothing. */
          fprintf(stderr, "[wait-obj] lr=0x%08X sleep=%lluus", (uint32_t)ctx->lr,
                  (unsigned long long)usec);
          static const int regs[] = { 28, 29, 30, 31, 3 };
          for (unsigned ri = 0; ri < sizeof regs / sizeof regs[0]; ri++) {
            const int r = regs[ri];
            const uint32_t o = (uint32_t)ctx->gpr[r];
            fprintf(stderr, "  r%d=0x%08X", r, o);
            /* Only deref something that looks like a guest pointer: a small
             * integer is a count, and dumping "memory at 4" is noise. */
            if (vm_base && o >= 0x10000u) {
              fprintf(stderr, "[");
              for (int w = 0; w < 4; w++) {
                const uint8_t* q = vm_base + o + (unsigned)w * 4u;
                uint32_t v = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                             ((uint32_t)q[2] << 8)  | (uint32_t)q[3];
                fprintf(stderr, "%s%08X", w ? " " : "", v);
              }
              fprintf(stderr, "]");
            }
          }
          fprintf(stderr, "\n"); fflush(stderr);
        } } }

    /* POLLSITE: resolve the host chain of the 1ms poller (LBP bringup) to
     * guest functions -- names the stage that is starving. */
    { static int _ps = -1; if (_ps < 0) _ps = getenv("POLLSITE") ? 12 : 0;
      if (_ps > 0 && usec == 1000) { _ps--;
        extern void ppu_log_host_chain(const char*);
        ppu_log_host_chain("usleep1ms"); } }

#ifndef _WIN32
    /* A registered poll site sleeps to its own deadline in one wait (ppu.poll_sleep_coalesce); anything the seam
     * does not take falls through to the sleep below unchanged. */
    if (usleep_site_sleep(ctx)) return CELL_OK;
#endif

#ifdef _WIN32
    /* Use high-resolution sleep via waitable timer for better precision */
    if (usec >= 1000) {
        HANDLE timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -((LONGLONG)usec * 10); /* 100ns units, negative = relative */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
            CloseHandle(timer);
        } else {
            Sleep((DWORD)(usec / 1000));
        }
    } else if (usec > 0) {
        /* Sub-millisecond sleep. Sleep()/waitable timers round up to the
         * ~0.5-1 ms scheduler quantum, far too coarse for a guest that relies
         * on accurate microsecond pacing -- a short usleep() spin waiting on
         * another thread to make progress otherwise collapses to a single yield
         * and busy-spins at full speed (so the wait is effectively a no-op).
         * Busy-wait to a precise QPC deadline, yielding inside the spin
         * (SwitchToThread) so a sibling host thread the guest may be waiting on
         * still gets the core. Mirrors RPCS3's "Usleep Only" TSC busy-tail.
         *
         * ppu.usleep_hires_wait_us (default 0, off): a sub-millisecond sleep
         * of at least that many us waits on a high-resolution waitable timer
         * instead. The spin keeps a logical processor busy for the whole
         * sleep, and a thread that polls with ~800 us sleeps holds one all
         * the time; such a poller only needs to wake roughly on time. The
         * millisecond branch above keeps the ordinary waitable timer. */
        const uint32_t wait_us = ps3_cfg_ppu_usleep_hires_wait_us();
        if (wait_us > 0 && usec >= (uint64_t)wait_us) {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
            static __declspec(thread) HANDLE t_timer;
            if (!t_timer)
                t_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
            if (t_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -((LONGLONG)usec * 10);
                if (SetWaitableTimer(t_timer, &due, 0, NULL, NULL, FALSE)) {
                    WaitForSingleObject(t_timer, INFINITE);
                    return CELL_OK;
                }
            }
        }
        ensure_qpc_init();
        LARGE_INTEGER qpc_start, qpc_now;
        QueryPerformanceCounter(&qpc_start);
        const int64_t qpc_deadline = qpc_start.QuadPart +
            (int64_t)((usec * (uint64_t)s_qpc_freq.QuadPart) / 1000000ULL);
        do {
            SwitchToThread();
            QueryPerformanceCounter(&qpc_now);
        } while (qpc_now.QuadPart < qpc_deadline);
    }
#else
    /* ppu.usleep_spin_us (default 0, off): a usleep shorter than that many
     * microseconds yields to a CLOCK_MONOTONIC deadline instead of calling
     * nanosleep, as the Windows branch above does for every sub-millisecond
     * wait. Guests commonly poll a flag with usleep(1..10); a nanosleep that
     * short returns only after the kernel's timer slack (50 us by default on
     * Linux) plus a wakeup, so each poll sees the flag tens of microseconds
     * late. Yielding keeps the core available to the sibling thread the guest
     * is waiting on. Longer sleeps still nanosleep. CLOCK_MONOTONIC, not
     * host_monotonic_ns: the wait is relative and must elapse while the guest
     * clock is paused. */
    const uint32_t spin_us = ps3_cfg_ppu_usleep_spin_us();
    if (usec > 0 && usec < (uint64_t)spin_us) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t deadline = (uint64_t)now.tv_sec * 1000000000ull +
                                  (uint64_t)now.tv_nsec + usec * 1000ull;
        do {
            sched_yield();
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while ((uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec < deadline);
    } else if (usec > 0) {
        struct timespec ts;
        ts.tv_sec  = (time_t)(usec / 1000000);
        ts.tv_nsec = (long)((usec % 1000000) * 1000);
#ifdef __APPLE__
        if (usec >= USLEEP_PRECISE_MIN_US && usec <= USLEEP_PRECISE_MAX_US && precise_usleep_enabled()) {
            const uint64_t left = sleep_critical_timer(usec * 1000u);
            if (!left) {
                atomic_fetch_add_explicit(&s_precise_usleeps, 1, memory_order_relaxed);
                return CELL_OK;
            }
            ts.tv_sec  = (time_t)(left / 1000000000u);
            ts.tv_nsec = (long)(left % 1000000000u);
        }
#endif
        nanosleep(&ts, NULL);
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_sleep
 *
 * r3 = seconds
 * -----------------------------------------------------------------------*/
int64_t sys_timer_sleep(ppu_context* ctx)
{
    uint32_t sec = LV2_ARG_U32(ctx, 0);
    ppu_thread_note_block(ctx->thread_id);   /* PS3_PRIO_START */

#ifdef _WIN32
    Sleep(sec * 1000);
#else
    sleep(sec);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_current_time
 *
 * r3 = pointer to receive seconds (u64*)
 * r4 = pointer to receive nanoseconds (u64*)
 * -----------------------------------------------------------------------*/
/* LV2 144: r3/r4 point to signed BE32 minute offsets. The first is the
 * standard UTC offset; the second is the current seasonal adjustment.
 * Derive the adjustment instead of assuming every DST shift is 60 minutes. */
int64_t sys_time_get_timezone(ppu_context* ctx)
{
    uint32_t zone_addr = LV2_ARG_PTR(ctx, 0);
    uint32_t dst_addr = LV2_ARG_PTR(ctx, 1);
    if (!zone_addr || !dst_addr || zone_addr > UINT32_MAX - 3 || dst_addr > UINT32_MAX - 3)
        return (int32_t)CELL_EFAULT;

    int32_t standard_minutes, daylight_minutes;
#ifdef _WIN32
    /* Ask the system, not the C runtime: _get_timezone and _get_dstbias do not exist in the msvcrt.dll that a
     * MinGW-w64 toolchain with the msvcrt runtime links against. Bias is minutes west of UTC. */
    TIME_ZONE_INFORMATION tz;
    DWORD zone_id = GetTimeZoneInformation(&tz);
    if (zone_id == TIME_ZONE_ID_INVALID)
        return (int32_t)CELL_EAGAIN;
    standard_minutes = -(int32_t)(tz.Bias + tz.StandardBias);
    daylight_minutes = zone_id == TIME_ZONE_ID_DAYLIGHT ? -(int32_t)(tz.DaylightBias - tz.StandardBias) : 0;
#else
    time_t now = time(NULL);
    struct tm local;
    if (!localtime_r(&now, &local)) return (int32_t)CELL_EAGAIN;
    daylight_minutes = 0;
    if (local.tm_isdst > 0) {
        struct tm standard = local;
        standard.tm_isdst = 0;
        time_t standard_epoch = mktime(&standard);
        if (standard_epoch == (time_t)-1) return (int32_t)CELL_EAGAIN;
        daylight_minutes = (int32_t)(difftime(standard_epoch, now) / 60);
    }
    standard_minutes = (int32_t)(local.tm_gmtoff / 60) - daylight_minutes;
#endif
    write_be32(zone_addr, (uint32_t)standard_minutes);
    write_be32(dst_addr, (uint32_t)daylight_minutes);
    return CELL_OK;
}

int64_t sys_time_get_current_time(ppu_context* ctx)
{
    uint32_t sec_addr  = LV2_ARG_PTR(ctx, 0);
    uint32_t nsec_addr = LV2_ARG_PTR(ctx, 1);

    uint64_t sec, nsec;

#ifdef _WIN32
    ensure_qpc_init();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    /* Convert QPC to seconds + nanoseconds */
    sec  = (uint64_t)(now.QuadPart / s_qpc_freq.QuadPart);
    uint64_t remainder = (uint64_t)(now.QuadPart % s_qpc_freq.QuadPart);
    nsec = (remainder * 1000000000ULL) / (uint64_t)s_qpc_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    sec  = (uint64_t)ts.tv_sec;
    nsec = (uint64_t)ts.tv_nsec;
#endif

    if (sec_addr != 0) {
        write_be64(sec_addr, sec);
    }
    if (nsec_addr != 0) {
        write_be64(nsec_addr, nsec);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_time_get_timebase_frequency
 *
 * Returns the PS3 timebase frequency in r3.
 * -----------------------------------------------------------------------*/
int64_t sys_time_get_timebase_frequency(ppu_context* ctx)
{
    (void)ctx;
    return (int64_t)PS3_TIMEBASE_FREQ;
}

/* ---------------------------------------------------------------------------
 * Periodic timer thread
 * -----------------------------------------------------------------------*/

/* Forward declaration */
static void timer_send_event(sys_timer_info* t);

/* Microseconds on a monotonic host clock: the timer's expiration times. */
static uint64_t timer_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (uint64_t)(n.QuadPart / f.QuadPart) * 1000000ull +
           (uint64_t)(n.QuadPart % f.QuadPart) * 1000000ull / (uint64_t)f.QuadPart;
#else
    return host_monotonic_ns() / 1000ull;
#endif
}

#ifdef _WIN32
static DWORD WINAPI timer_thread_proc(LPVOID param)
{
    sys_timer_info* t = (sys_timer_info*)param;
    ps3_thread_name("lv2 timer");

    while (t->running) {
        DWORD ms = (DWORD)(t->period_usec / 1000);
        if (ms == 0) ms = 1;

        DWORD result = WaitForSingleObject(t->stop_event, ms);
        if (result == WAIT_OBJECT_0) {
            break; /* stop signaled */
        }

        if (t->running) {
            timer_send_event(t);
        }
    }

    return 0;
}
#else
static void* timer_thread_proc(void* param)
{
    sys_timer_info* t = (sys_timer_info*)param;

    pthread_mutex_lock(&t->mtx);
    while (!t->stop_flag) {
        /* One budget per period, kept across every wake that is not the stop
         * request: a false wake before the period is over neither delivers an
         * event early nor starts the period again. The next period starts only
         * after this one expired and its event was delivered. */
        const ps3_wait_budget period = ps3_wait_budget_us(t->period_usec, 0);
        int rc;
        do {
            rc = ps3_cond_wait_monotonic(&t->cv, &t->mtx, &period);
        } while (rc == 0 && !t->stop_flag);
        if (t->stop_flag) break;

        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&t->mtx);
            timer_send_event(t);
            pthread_mutex_lock(&t->mtx);
        } else {
            /* The host refused the wait: a timer that cannot wait would fire at once, forever. */
            ps3_stop_host_wait_error("lv2-timer", rc);
        }
    }
    pthread_mutex_unlock(&t->mtx);

    return NULL;
}
#endif

static void timer_send_event(sys_timer_info* t)
{
    if (t->event_queue_id <= 0 || t->event_queue_id > SYS_EVENT_QUEUE_MAX)
        return;

    /* lv2 (and RPCS3) deliver the connect's data1/data2 and, in data3, the
     * EXPIRATION TIME -- which advances every period. It was 0 here, so a
     * consumer that paces itself by the delta between ticks saw no time pass:
     * Twisted Metal's BoomRangBuss audio driver wakes on this timer, derives
     * how many blocks to service from data3, and serviced none, ever -- so
     * brb_StartSession timed out and every later BRB request hung.
     *
     * Queue admission pins the slot before advancing the expiration (and a full
     * queue drops the event): a destroy racing this periodic callback must not
     * reuse the queue's native lock. */
    (void)sys_event_queue_push_periodic_by_id((uint32_t)t->event_queue_id, t->source,
                                               t->data1, t->data2, &t->next_expire,
                                               t->period_usec);
}
#ifdef PS3_SYNC_TEST_HOOKS
void sys_timer_test_send_event(sys_timer_info* t)
{
    timer_send_event(t);
}
#endif

/* ---------------------------------------------------------------------------
 * sys_timer_create
 *
 * r3 = pointer to receive timer ID (u32*)
 * -----------------------------------------------------------------------*/
int64_t sys_timer_create(ppu_context* ctx)
{
    uint32_t id_out_addr = LV2_ARG_PTR(ctx, 0);

    int slot = -1;
    for (int i = 0; i < SYS_TIMER_MAX; i++) {
        if (!g_sys_timers[i].active) { slot = i; break; }
    }
    if (slot < 0)
        return (int64_t)(int32_t)CELL_EAGAIN;

    sys_timer_info* t = &g_sys_timers[slot];
    memset(t, 0, sizeof(*t));
    t->active  = 1;
    t->running = 0;
    t->event_queue_id = 0;

#ifdef _WIN32
    t->stop_event    = NULL;
    t->thread_handle = NULL;
    t->timer_handle  = NULL;
#else
    if (pthread_mutex_init(&t->mtx, NULL) != 0) {
        t->active = 0;
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    if (ps3_cond_init_monotonic(&t->cv) != 0) {   /* the period wait runs on the monotonic clock */
        pthread_mutex_destroy(&t->mtx);
        t->active = 0;
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    t->stop_flag = 0;
#endif

    uint32_t timer_id = (uint32_t)(slot + 1);
    if (id_out_addr != 0) {
        write_be32(id_out_addr, timer_id);
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_destroy
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_destroy(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    /* Stop if running */
    if (t->running) {
        t->running = 0;
#ifdef _WIN32
        if (t->stop_event) SetEvent(t->stop_event);
        if (t->thread_handle) {
            WaitForSingleObject(t->thread_handle, 3000);
            CloseHandle(t->thread_handle);
        }
        if (t->stop_event) CloseHandle(t->stop_event);
#else
        pthread_mutex_lock(&t->mtx);
        t->stop_flag = 1;
        pthread_cond_signal(&t->cv);
        pthread_mutex_unlock(&t->mtx);
        pthread_join(t->thread, NULL);
        pthread_mutex_destroy(&t->mtx);
        pthread_cond_destroy(&t->cv);
#endif
    }

    t->active = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_connect_event_queue
 *
 * r3 = timer_id
 * r4 = queue_id
 * r5 = source
 * r6 = data1
 * -----------------------------------------------------------------------*/
int64_t sys_timer_connect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);
    uint32_t queue_id = LV2_ARG_U32(ctx, 1);
    uint64_t source   = LV2_ARG_U64(ctx, 2);
    uint64_t data1    = LV2_ARG_U64(ctx, 3);
    uint64_t data2    = LV2_ARG_U64(ctx, 4);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (queue_id == 0 || queue_id > SYS_EVENT_QUEUE_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = (int32_t)queue_id;
    t->source         = source;
    t->data1          = data1;
    t->data2          = data2;

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_disconnect_event_queue
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_disconnect_event_queue(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    t->event_queue_id = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_start
 *
 * r3 = timer_id
 * r4 = base_time (absolute start time, 0 = now)
 * r5 = period_usec
 * -----------------------------------------------------------------------*/
int64_t sys_timer_start(ppu_context* ctx)
{
    uint32_t timer_id    = LV2_ARG_U32(ctx, 0);
    /* uint64_t base_time = LV2_ARG_U64(ctx, 1); -- ignored for now */
    uint64_t period      = LV2_ARG_U64(ctx, 2);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (t->running)
        return (int64_t)(int32_t)CELL_EBUSY;

    if (period == 0)
        return (int64_t)(int32_t)CELL_EINVAL;

    t->period_usec = period;
    t->running     = 1;
    t->next_expire = timer_now_us() + period;

#ifdef _WIN32
    t->stop_event    = CreateEventA(NULL, TRUE, FALSE, NULL);
    t->thread_handle = CreateThread(NULL, 0, timer_thread_proc, t, 0, NULL);
#else
    t->stop_flag = 0;
    if (pthread_create(&t->thread, NULL, timer_thread_proc, t) != 0) {
        /* The timer syscall has a caller: lv2's resource-exhaustion error is
         * CELL_EAGAIN (sys_timer_create in RPCS3 returns it when it cannot make
         * the timer). running stays 0 so sys_timer_stop/destroy never join a
         * pthread_t that pthread_create did not write. */
        t->running = 0;
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_timer_stop
 *
 * r3 = timer_id
 * -----------------------------------------------------------------------*/
int64_t sys_timer_stop(ppu_context* ctx)
{
    uint32_t timer_id = LV2_ARG_U32(ctx, 0);

    if (timer_id == 0 || timer_id > SYS_TIMER_MAX)
        return (int64_t)(int32_t)CELL_ESRCH;

    sys_timer_info* t = &g_sys_timers[timer_id - 1];
    if (!t->active)
        return (int64_t)(int32_t)CELL_ESRCH;

    if (!t->running)
        return CELL_OK;

    t->running = 0;

#ifdef _WIN32
    if (t->stop_event) SetEvent(t->stop_event);
    if (t->thread_handle) {
        WaitForSingleObject(t->thread_handle, 3000);
        CloseHandle(t->thread_handle);
        t->thread_handle = NULL;
    }
    if (t->stop_event) {
        CloseHandle(t->stop_event);
        t->stop_event = NULL;
    }
#else
    pthread_mutex_lock(&t->mtx);
    t->stop_flag = 1;
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->mtx);
    pthread_join(t->thread, NULL);
#endif

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 *
 * NOTE: There are syscall number collisions in the existing table:
 *   SYS_TIMER_USLEEP (141) == SYS_EVENT_FLAG_WAIT (141)
 *   SYS_TIMER_SLEEP  (142) == SYS_EVENT_FLAG_TRYWAIT (142)
 *   SYS_TIME_GET_CURRENT_TIME (145) == SYS_EVENT_FLAG_CANCEL (145)
 * The event flag handlers are registered separately and will win.
 * Timer sleep/time functions should be called via wrapper functions
 * or fixed syscall numbers should be assigned.
 * For now, we register the timer-specific syscalls that don't collide.
 * -----------------------------------------------------------------------*/
void sys_timer_init(lv2_syscall_table* tbl)
{
    memset(g_sys_timers, 0, sizeof(g_sys_timers));

#ifdef _WIN32
    ensure_qpc_init();
#endif

    lv2_syscall_register(tbl, SYS_TIMER_CREATE,                   sys_timer_create);
    lv2_syscall_register(tbl, SYS_TIMER_DESTROY,                  sys_timer_destroy);
    lv2_syscall_register(tbl, SYS_TIMER_START,                    sys_timer_start);
    lv2_syscall_register(tbl, SYS_TIMER_STOP,                     sys_timer_stop);
    lv2_syscall_register(tbl, SYS_TIMER_CONNECT_EVENT_QUEUE,      sys_timer_connect_event_queue);
    lv2_syscall_register(tbl, SYS_TIMER_DISCONNECT_EVENT_QUEUE,   sys_timer_disconnect_event_queue);

    /* These have conflicting numbers with event flag syscalls.
     * Register them here -- last registration wins. If events are
     * registered after timers, the event handlers will override.
     * The runtime should dispatch timer_usleep/sleep/get_current_time
     * via direct function calls instead. */
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEBASE_FREQUENCY, sys_time_get_timebase_frequency);
    lv2_syscall_register(tbl, SYS_TIME_GET_TIMEZONE, sys_time_get_timezone);

    /* Register these but be aware of collisions */
    lv2_syscall_register(tbl, SYS_TIMER_USLEEP,            sys_timer_usleep);
    lv2_syscall_register(tbl, SYS_TIMER_SLEEP,             sys_timer_sleep);
    lv2_syscall_register(tbl, SYS_TIME_GET_CURRENT_TIME,   sys_time_get_current_time);
}
