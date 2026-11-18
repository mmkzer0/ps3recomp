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
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#endif

#ifdef __APPLE__
/* The raw host clock: mach_absolute_time in nanoseconds, paused or not. */
static uint64_t host_raw_ns(void)
{
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
}

/* The pause state is one word read through a seqlock, so a reader gets raw time and the state it was measured
 * against from the same stable interval of the lock, never one of each from either side of a transition.
 *
 *   running: s_state is the offset (total time paused so far); active time = raw - offset.
 *   paused:  s_state is HC_PAUSED | the active time frozen at the pause.
 *
 * Why not read the raw clock and a state word independently: a pause that lands between the two reads, or a
 * resume, makes one reader return a time that another thread's later read of the new state is behind (the GCM wait
 * clock stepped back by the gap, ms when the reader had been descheduled, until its two readings were joined). A
 * reader that finds the sequence unchanged around its raw read knows no transition happened in between. A writer
 * reads the raw clock after it has made the sequence odd, so every reader that completed on the old state read raw
 * no later than the writer, and one that sees the new state reads it no earlier.
 *
 * Writers (pause, resume) are rare and serialised by s_writer. Readers take no lock and store nothing: the unpaused
 * path is the raw read, a few atomic loads and a compare. A reader that meets a transition in flight (odd
 * sequence) yields, which waits out only the writer's few instructions. */
#define HC_PAUSED (1ull << 63)

static _Atomic uint64_t s_seq;     /* even: stable; odd: a transition is being written */
static _Atomic uint64_t s_state;
static pthread_mutex_t  s_writer = PTHREAD_MUTEX_INITIALIZER;

/* The sequence, read after the counter read that produced `raw`. A counter read is not a memory access, so a plain
 * re-read may be satisfied before it: a reader that completed on the old state could then carry a counter value
 * taken after the writer's own reading, a tick or two (41 ns) ahead of the frozen time the next reader returns.
 * Measured without this: reads stepping back by up to 84 ns across a pause, in 4 readers x 20000 toggles. */
static inline uint64_t host_clock_seq_after(uint64_t raw)
{
#if defined(__aarch64__)
    /* An address dependency on `raw` orders the load after the counter read (the form Linux's
     * arch_counter_enforce_ordering takes); an ISB there costs 4 ns on the hot path, this one about 1. */
    uint64_t seq, zero;
    __asm__ volatile("eor %1, %2, %2\n\tldr %0, [%3, %1]"
                     : "=&r"(seq), "=&r"(zero) : "r"(raw), "r"(&s_seq) : "memory");
    return seq;
#else
    (void)raw;
#  if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("lfence" ::: "memory");
#  else
    atomic_thread_fence(memory_order_seq_cst);
#  endif
    return atomic_load_explicit(&s_seq, memory_order_relaxed);
#endif
}

/* One transition of the state, the caller holding s_writer: running -> paused when `to_paused`, else back. */
static void host_clock_transition(int to_paused)
{
    const uint64_t seq = atomic_load_explicit(&s_seq, memory_order_relaxed);
    atomic_store_explicit(&s_seq, seq + 1, memory_order_relaxed);
    /* Full fence: the odd sequence is visible before the raw read below, so a reader that completed on the old
     * state (it saw an even sequence around its raw read) read raw no later than this writer does. */
    atomic_thread_fence(memory_order_seq_cst);
#if defined(__aarch64__)
    /* A DMB orders memory accesses, not the counter read in host_raw_ns: without the ISB the writer's raw could be
     * taken before its odd sequence is visible, the mirror of the reader's case above. Writers are rare. */
    __asm__ volatile("isb" ::: "memory");
#endif
    const uint64_t old = atomic_load_explicit(&s_state, memory_order_relaxed);
    const uint64_t raw = host_raw_ns();
    const uint64_t next = to_paused ? (HC_PAUSED | (raw - old))   /* running: old is the offset */
                                    : (raw - (old & ~HC_PAUSED)); /* paused: old carries the frozen time */
    atomic_store_explicit(&s_state, next, memory_order_relaxed);
    atomic_store_explicit(&s_seq, seq + 2, memory_order_release);
}

void host_clock_pause(void)
{
    pthread_mutex_lock(&s_writer);
    if (!(atomic_load_explicit(&s_state, memory_order_relaxed) & HC_PAUSED))
        host_clock_transition(1);
    pthread_mutex_unlock(&s_writer);
}

void host_clock_resume(void)
{
    pthread_mutex_lock(&s_writer);
    if (atomic_load_explicit(&s_state, memory_order_relaxed) & HC_PAUSED)
        host_clock_transition(0);
    pthread_mutex_unlock(&s_writer);
}

int host_clock_paused(void)
{
    return (atomic_load_explicit(&s_state, memory_order_acquire) & HC_PAUSED) != 0;
}
#endif /* __APPLE__ */

uint64_t host_monotonic_ns(void)
{
#ifdef __APPLE__
    for (;;) {
        const uint64_t s1 = atomic_load_explicit(&s_seq, memory_order_acquire);
        if (s1 & 1) { sched_yield(); continue; }
        /* Acquire: the second sequence read below cannot move above this load, and a state written inside a
         * transition makes that read see the transition. */
        const uint64_t st = atomic_load_explicit(&s_state, memory_order_acquire);
        /* One atomic word that was current when read: nothing to cross-check, and no raw read to place. */
        if (st & HC_PAUSED) return st & ~HC_PAUSED;
        const uint64_t raw = host_raw_ns();
        if (host_clock_seq_after(raw) == s1) return raw - st;
    }
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
