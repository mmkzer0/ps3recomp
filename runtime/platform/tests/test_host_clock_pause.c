/*
 * runtime/platform/host_clock.{h,c}: the pausable host clock (Apple).
 *
 * The contract a guest clock relies on: while the host has paused the app every read returns one value; after the
 * resume the clock continues from that value, so the paused span is not in it; pause and resume are idempotent;
 * and no reader ever sees time step back, whichever threads pause, resume and read.
 *
 * The raw reference is the host's own uptime clock (CLOCK_UPTIME_RAW, the clock host_monotonic_ns converts), read
 * around each call, so every bound below is a statement the paused clock must satisfy against it:
 *   - a span the test spent with the clock paused is excluded: active time grew by at most the raw time spent
 *     outside the pause, and by at least the raw time spent outside the pause the calls leave visible;
 *   - nothing here sleeps to order events: spans are spun on the raw clock, threads meet on atomics.
 * RAW_SLACK_NS covers the conversion of mach ticks to nanoseconds happening in two places.
 */
#ifndef __APPLE__
#error "the pause API exists on Apple hosts only (host_clock.h)"
#endif

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "host_clock.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define NS_PER_MS     1000000ull
#define RAW_SLACK_NS  2000ull

static uint64_t raw_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/* Spins until the raw clock has moved by `ns`. */
static void spin_raw(uint64_t ns)
{
    const uint64_t until = raw_ns() + ns;
    while (raw_ns() < until) {}
}

/* (a) Paused: every read is one value, however much real time passes. (b) The first read after the resume is the
 * frozen value plus only what elapsed since the resume began. */
static void test_freeze_and_continue(void)
{
    CHECK(!host_clock_paused());
    const uint64_t before = host_monotonic_ns();
    host_clock_pause();
    CHECK(host_clock_paused());

    const uint64_t frozen = host_monotonic_ns();
    CHECK(frozen >= before);
    const uint64_t pause_raw = raw_ns();
    while (raw_ns() - pause_raw < 20 * NS_PER_MS)
        CHECK(host_monotonic_ns() == frozen);
    CHECK(host_clock_paused());

    const uint64_t resume_begin = raw_ns();
    host_clock_resume();
    const uint64_t first = host_monotonic_ns();
    const uint64_t first_raw = raw_ns();
    CHECK(!host_clock_paused());

    CHECK(first >= frozen);
    /* The 20 ms the clock stood still are not in it: it moved at most as far as the raw clock did since the
     * resume call began. */
    CHECK(first - frozen <= first_raw - resume_begin + RAW_SLACK_NS);
    CHECK(first - frozen < 20 * NS_PER_MS);
}

/* (c) Pause/resume cycles accumulate: the active time over all of them excludes every paused span. */
static void test_accumulates(void)
{
    enum { CYCLES = 50 };
    const uint64_t active0 = host_monotonic_ns();
    const uint64_t raw0 = raw_ns();
    uint64_t paused_lo = 0;   /* paused for at least this much: from the pause call's return to the resume call */
    uint64_t paused_hi = 0;   /* ...at most this much: from before the pause call to after the resume call */
    uint64_t frozen_prev = 0;
    for (int i = 0; i < CYCLES; i++) {
        spin_raw(100 * 1000ull);                 /* some running time between pauses */
        const uint64_t hi0 = raw_ns();
        host_clock_pause();
        const uint64_t lo0 = raw_ns();
        const uint64_t frozen = host_monotonic_ns();
        CHECK(frozen >= frozen_prev);
        spin_raw(500 * 1000ull);
        CHECK(host_monotonic_ns() == frozen);
        const uint64_t lo1 = raw_ns();
        host_clock_resume();
        const uint64_t hi1 = raw_ns();
        paused_lo += lo1 - lo0;
        paused_hi += hi1 - hi0;
        frozen_prev = frozen;
    }
    const uint64_t active1 = host_monotonic_ns();
    const uint64_t raw1 = raw_ns();

    const uint64_t active = active1 - active0;
    const uint64_t raw = raw1 - raw0;
    CHECK(paused_lo >= CYCLES * 500 * 1000ull);
    CHECK(active + paused_lo <= raw + CYCLES * RAW_SLACK_NS);   /* the paused spans are not in the active time */
    CHECK(raw <= active + paused_hi + CYCLES * RAW_SLACK_NS);   /* and nothing else is missing from it */
}

/* (d) Idempotence: a second pause neither re-freezes later nor needs a second resume; a second resume neither
 * moves the clock nor jumps it. */
static void test_idempotent(void)
{
    host_clock_pause();
    const uint64_t frozen = host_monotonic_ns();
    spin_raw(2 * NS_PER_MS);
    host_clock_pause();                       /* paused already: not a new freeze point */
    CHECK(host_clock_paused());
    CHECK(host_monotonic_ns() == frozen);

    spin_raw(2 * NS_PER_MS);
    host_clock_resume();                      /* one resume undoes both pauses */
    CHECK(!host_clock_paused());
    const uint64_t r1 = host_monotonic_ns();
    CHECK(r1 >= frozen);
    CHECK(r1 - frozen < 2 * NS_PER_MS);

    const uint64_t before = raw_ns();
    host_clock_resume();                      /* running already: nothing to resume */
    CHECK(!host_clock_paused());
    const uint64_t r2 = host_monotonic_ns();
    const uint64_t after = raw_ns();
    CHECK(r2 >= r1);
    CHECK(r2 - r1 <= after - before + RAW_SLACK_NS);
}

/* (e) Readers never see time step back while one thread pauses and resumes thousands of times. */
enum { READERS = 4, TOGGLES = 20000 };

static _Atomic int g_ready;
static _Atomic int g_stop;
static _Atomic uint64_t g_back_steps;
static _Atomic uint64_t g_reads[READERS];

static void* reader_main(void* arg)
{
    const unsigned idx = (unsigned)(uintptr_t)arg;
    uint64_t prev = host_monotonic_ns();
    uint64_t n = 0;
    atomic_fetch_add_explicit(&g_ready, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        const uint64_t v = host_monotonic_ns();
        if (v < prev)
            atomic_fetch_add_explicit(&g_back_steps, 1, memory_order_relaxed);
        prev = v;
        n++;
    }
    atomic_store_explicit(&g_reads[idx], n, memory_order_relaxed);
    return NULL;
}

static void test_readers_never_step_back(void)
{
    pthread_t th[READERS];
    atomic_store(&g_ready, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_back_steps, 0);
    for (unsigned i = 0; i < READERS; i++)
        CHECK(pthread_create(&th[i], NULL, reader_main, (void*)(uintptr_t)i) == 0);
    while (atomic_load_explicit(&g_ready, memory_order_acquire) < READERS) sched_yield();

    for (int i = 0; i < TOGGLES; i++) {
        host_clock_pause();
        /* Hold it for a few reads' worth of time on every other cycle so readers observe the frozen value,
         * and not only the transitions. */
        if (i & 1) spin_raw(2000);
        host_clock_resume();
        if (!(i & 3)) spin_raw(1000);
    }

    atomic_store_explicit(&g_stop, 1, memory_order_release);
    for (unsigned i = 0; i < READERS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    for (unsigned i = 0; i < READERS; i++)
        CHECK(atomic_load(&g_reads[i]) > 0);
    CHECK(atomic_load(&g_back_steps) == 0);
    CHECK(!host_clock_paused());
}

int main(void)
{
    test_freeze_and_continue();
    test_accumulates();
    test_idempotent();
    test_readers_never_step_back();
    printf("test_host_clock_pause: ok\n");
    return 0;
}
