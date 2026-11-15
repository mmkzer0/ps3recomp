/*
 * include/ps3emu/host_gpu_gate.h: the gate that holds the RSX thread while the host forbids GPU use, the sections
 * that keep the host from cutting a submission in half, the checkpoint that stops a section from touching the GPU
 * while the gate is closed, and the pure decision a failed allocation takes.
 *
 * Nothing here sleeps to order events. A thread is known to be blocked because the gate counts the threads parked
 * in enter()/wait_active(), draining in suspend() and watching in wait_change() (host_gpu_gate_waiters / _drainers /
 * _watchers, under the gate's own lock), and the test spins on those counts; whether a thread has passed is its
 * own atomic flag. A thread that returned without blocking leaves the count at 0 and sets its flag, so the bounded
 * spin ends and the check names it instead of hanging. The only timed waits are the failure paths (a bound that
 * must run out); they are asserted on what they return, and on a lower bound measured on the monotonic clock
 * (never an upper bound: that is a scheduler statement).
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ps3emu/host_gpu_gate.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define SPIN_CAP 200000000u
#define NS_PER_MS 1000000ull
/* A bound no test run reaches while it works; a hang ends at the ctest timeout instead. */
#define LONG_NS (60ull * 1000ull * NS_PER_MS)

/* Gates are built with host_gpu_gate_init(), the constructor every platform has (the static initialiser exists
 * only where the condition variable can wait on the monotonic clock as created). */
#define NEW_GATE(g) host_gpu_gate g; CHECK(host_gpu_gate_init(&g) == 0)

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct worker {
    host_gpu_gate* gate;
    _Atomic int done;
    _Atomic int inside;      /* fn_blocked_section: the section is open */
    _Atomic int release;     /* fn_blocked_section: the blocking stage ends */
    _Atomic int past;        /* fn_blocked_section: the "next Metal call" was reached */
    int result;
    uint64_t arg;
    pthread_t thread;
} worker;

static int worker_done(worker* w) { return atomic_load_explicit(&w->done, memory_order_acquire); }

static void worker_start(worker* w, host_gpu_gate* g, void* (*fn)(void*), uint64_t arg)
{
    w->gate = g;
    w->arg = arg;
    w->result = -1;
    atomic_store(&w->done, 0);
    atomic_store(&w->inside, 0);
    atomic_store(&w->release, 0);
    atomic_store(&w->past, 0);
    CHECK(pthread_create(&w->thread, NULL, fn, w) == 0);
}

static void worker_join(worker* w) { CHECK(pthread_join(w->thread, NULL) == 0); }

/* wait_active() then done. */
static void* fn_wait_active(void* p)
{
    worker* w = (worker*)p;
    host_gpu_gate_wait_active(w->gate);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* enter(), done, and the section stays open until the test closes it (so the test can observe it open). */
static void* fn_enter(void* p)
{
    worker* w = (worker*)p;
    host_gpu_gate_enter(w->gate);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* suspend(arg ns): result is its return value. */
static void* fn_suspend(void* p)
{
    worker* w = (worker*)p;
    w->result = host_gpu_gate_suspend(w->gate, w->arg);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* wait_change(ticket in arg, a long bound): result is whether it changed. */
static void* fn_wait_change(void* p)
{
    worker* w = (worker*)p;
    w->result = host_gpu_gate_wait_change(w->gate, (uint32_t)w->arg, LONG_NS);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* A present section: enter, a blocking stage that ends when the test says (the in-flight semaphore wait), the
 * checkpoint, then the "next Metal call" (`past`), then leave. */
static void* fn_blocked_section(void* p)
{
    worker* w = (worker*)p;
    host_gpu_gate_enter(w->gate);
    atomic_store_explicit(&w->inside, 1, memory_order_release);
    while (!atomic_load_explicit(&w->release, memory_order_acquire)) sched_yield();
    host_gpu_gate_checkpoint(w->gate);
    atomic_store_explicit(&w->past, 1, memory_order_release);
    host_gpu_gate_leave(w->gate);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* A commit section: enter, the checkpoint passes with the gate open, the thread is then delayed (`release`), and
 * only afterwards does it "commit" (`past`) and leave. */
static void* fn_checkpointed_section(void* p)
{
    worker* w = (worker*)p;
    host_gpu_gate_enter(w->gate);
    host_gpu_gate_checkpoint(w->gate);                         /* open: passes */
    atomic_store_explicit(&w->inside, 1, memory_order_release);
    while (!atomic_load_explicit(&w->release, memory_order_acquire)) sched_yield();
    atomic_store_explicit(&w->past, 1, memory_order_release);  /* the commit */
    host_gpu_gate_leave(w->gate);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* A present's blocking stage the way the backend runs it: the section is left before the unbounded wait (the
 * in-flight semaphore, nextDrawable) and entered again after it; `inside` marks the thread as blocked in the wait. */
static void* fn_semaphore_section(void* p)
{
    worker* w = (worker*)p;
    host_gpu_gate_enter(w->gate);
    host_gpu_gate_leave(w->gate);
    atomic_store_explicit(&w->inside, 1, memory_order_release);
    while (!atomic_load_explicit(&w->release, memory_order_acquire)) sched_yield();
    host_gpu_gate_enter(w->gate);                              /* blocks while the gate is closed */
    atomic_store_explicit(&w->past, 1, memory_order_release);  /* the next Metal call */
    host_gpu_gate_leave(w->gate);
    atomic_store_explicit(&w->done, 1, memory_order_release);
    return NULL;
}

/* Until `count()` of the gate reaches `n`, or one of `ws` is already done (a bug the caller then reports). */
static void spin_until(host_gpu_gate* g, uint32_t (*count)(host_gpu_gate*), uint32_t n, worker* ws, unsigned nws)
{
    for (unsigned spin = 0; spin < SPIN_CAP; spin++) {
        if (count(g) == n) return;
        for (unsigned i = 0; i < nws; i++)
            if (worker_done(&ws[i])) return;
        sched_yield();
    }
    CHECK(!"the threads never blocked");
}

static void spin_flag(_Atomic int* flag)
{
    for (unsigned spin = 0; spin < SPIN_CAP; spin++) {
        if (atomic_load_explicit(flag, memory_order_acquire)) return;
        sched_yield();
    }
    CHECK(!"the flag never rose");
}

/* An open gate does not block, take the slow path or change state, however often it is asked. */
static void case_open_does_not_block(void)
{
    NEW_GATE(g);
    const uint32_t t0 = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_gate_is_active(&g));
    for (int i = 0; i < 1000; i++) {
        host_gpu_gate_wait_active(&g);
        host_gpu_gate_enter(&g);
        CHECK(host_gpu_gate_sections(&g) == 1);
        host_gpu_gate_checkpoint(&g);               /* open: one load, the section stays as it was */
        CHECK(host_gpu_gate_sections(&g) == 1);
        host_gpu_gate_leave(&g);
    }
    CHECK(host_gpu_gate_sections(&g) == 0);
    CHECK(host_gpu_gate_parked_total(&g) == 0);
    CHECK(host_gpu_gate_waiters(&g) == 0);
    CHECK(host_gpu_gate_ticket(&g) == t0);
}

/* Only a real transition moves the ticket: the host's will-resign and did-enter-background are one suspension,
 * and a did-become-active on an app that never suspended changes nothing. With no section open, suspend() is
 * drained at once. */
static void case_idempotent(void)
{
    NEW_GATE(g);
    const uint32_t t0 = host_gpu_gate_ticket(&g);
    host_gpu_gate_resume(&g);
    CHECK(host_gpu_gate_ticket(&g) == t0);

    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    const uint32_t t1 = host_gpu_gate_ticket(&g);
    CHECK(t1 != t0 && !host_gpu_gate_is_active(&g));
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    CHECK(host_gpu_gate_ticket(&g) == t1 && !host_gpu_gate_is_active(&g));

    host_gpu_gate_resume(&g);
    const uint32_t t2 = host_gpu_gate_ticket(&g);
    CHECK(t2 != t1 && t2 != t0 && host_gpu_gate_is_active(&g));
    host_gpu_gate_resume(&g);
    CHECK(host_gpu_gate_ticket(&g) == t2);
}

/* wait_active() blocks while the gate is suspended, and one resume releases it. */
static void case_wait_active_blocks_until_resume(void)
{
    NEW_GATE(g);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    worker w;
    worker_start(&w, &g, fn_wait_active, 0);
    spin_until(&g, host_gpu_gate_waiters, 1, &w, 1);
    CHECK(host_gpu_gate_waiters(&g) == 1);
    CHECK(!worker_done(&w));

    host_gpu_gate_resume(&g);
    worker_join(&w);
    CHECK(worker_done(&w));
    CHECK(host_gpu_gate_waiters(&g) == 0);
    CHECK(host_gpu_gate_parked_total(&g) == 1);
}

/* Every parked thread is released by one resume (a broadcast, not a single wake). */
static void case_releases_all_waiters(void)
{
    enum { N = 4 };
    NEW_GATE(g);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    worker w[N];
    for (int i = 0; i < N; i++) worker_start(&w[i], &g, fn_enter, 0);
    spin_until(&g, host_gpu_gate_waiters, N, w, N);
    CHECK(host_gpu_gate_waiters(&g) == N);
    for (int i = 0; i < N; i++) CHECK(!worker_done(&w[i]));
    CHECK(host_gpu_gate_sections(&g) == 0);      /* a thread that backs out of a closed gate holds no section */

    host_gpu_gate_resume(&g);
    for (int i = 0; i < N; i++) {
        worker_join(&w[i]);
        CHECK(worker_done(&w[i]));
    }
    CHECK(host_gpu_gate_sections(&g) == N);
    for (int i = 0; i < N; i++) host_gpu_gate_leave(&g);
    CHECK(host_gpu_gate_sections(&g) == 0);
    CHECK(host_gpu_gate_waiters(&g) == 0);
}

/* A section entered before the suspend delays it: suspend() has closed the gate and waits, and returns only once
 * the section leaves. */
static void case_open_section_delays_suspend(void)
{
    NEW_GATE(g);
    host_gpu_gate_enter(&g);
    worker s;
    worker_start(&s, &g, fn_suspend, LONG_NS);
    spin_until(&g, host_gpu_gate_drainers, 1, &s, 1);
    CHECK(host_gpu_gate_drainers(&g) == 1);
    CHECK(!host_gpu_gate_is_active(&g));         /* closed: nothing new can begin */
    CHECK(!worker_done(&s));                     /* ... but it has not returned: our section is open */
    CHECK(host_gpu_gate_sections(&g) == 1);

    host_gpu_gate_leave(&g);
    worker_join(&s);
    CHECK(s.result == 1);                        /* drained, not timed out */
    CHECK(host_gpu_gate_sections(&g) == 0);
    CHECK(host_gpu_gate_drainers(&g) == 0);
    host_gpu_gate_resume(&g);
    CHECK(host_gpu_gate_is_active(&g));
}

/* Entry after the suspend blocks until the resume, including while the suspend is still draining a section
 * opened before it: the late thread does not slip in beside the one being drained. */
static void case_entry_after_suspend_blocks(void)
{
    NEW_GATE(g);
    host_gpu_gate_enter(&g);                     /* the section being drained */
    worker s;
    worker_start(&s, &g, fn_suspend, LONG_NS);
    spin_until(&g, host_gpu_gate_drainers, 1, &s, 1);

    worker late;
    worker_start(&late, &g, fn_enter, 0);
    spin_until(&g, host_gpu_gate_waiters, 1, &late, 1);
    CHECK(!worker_done(&late));
    CHECK(host_gpu_gate_sections(&g) == 1);      /* only the old section: the late one backed out */

    host_gpu_gate_leave(&g);
    worker_join(&s);
    CHECK(s.result == 1);
    CHECK(!worker_done(&late));                   /* drained, and still closed */

    host_gpu_gate_resume(&g);
    worker_join(&late);
    CHECK(worker_done(&late));
    CHECK(host_gpu_gate_sections(&g) == 1);
    host_gpu_gate_leave(&g);
    CHECK(host_gpu_gate_sections(&g) == 0);
}

/* The bound: a section that does not leave in time makes suspend() return 0, the gate stays closed, and the
 * section can still leave afterwards. The section is held across the whole call, so the 0 does not depend on how
 * long the bound was. The bound is measured on the monotonic clock: at least what was asked passed. */
static void case_drain_bound(void)
{
    NEW_GATE(g);
    host_gpu_gate_enter(&g);
    const uint64_t t0 = mono_ns();
    CHECK(host_gpu_gate_suspend(&g, 20ull * NS_PER_MS) == 0);
    CHECK(mono_ns() - t0 >= 20ull * NS_PER_MS);
    CHECK(!host_gpu_gate_is_active(&g));
    CHECK(host_gpu_gate_sections(&g) == 1);
    CHECK(host_gpu_gate_drainers(&g) == 0);
    host_gpu_gate_leave(&g);
    CHECK(host_gpu_gate_sections(&g) == 0);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);   /* a second suspend sees nothing left to drain */
    host_gpu_gate_resume(&g);
}

/* The checkpoint: a section held past the drain deadline cannot issue its next GPU call with the gate closed.
 * suspend() times out and returns with the section live; the section's blocking stage then ends with the gate
 * still closed; at the checkpoint it leaves the section and waits, so the "next Metal call" is not reached until
 * resume(). */
static void case_checkpoint_stops_a_late_section(void)
{
    NEW_GATE(g);
    worker w;
    worker_start(&w, &g, fn_blocked_section, 0);
    spin_flag(&w.inside);
    CHECK(host_gpu_gate_sections(&g) == 1);

    CHECK(host_gpu_gate_suspend(&g, 1ull * NS_PER_MS) == 0);       /* the drain deadline passes: section still live */
    CHECK(host_gpu_gate_sections(&g) == 1);
    CHECK(!host_gpu_gate_is_active(&g));

    atomic_store(&w.release, 1);                                   /* the blocking stage ends, gate closed */
    spin_until(&g, host_gpu_gate_waiters, 1, &w, 1);
    CHECK(!atomic_load_explicit(&w.past, memory_order_acquire));   /* it has NOT reached the next Metal call */
    CHECK(host_gpu_gate_sections(&g) == 0);                        /* it left its section to wait */
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);                /* so a drain now completes at once */

    host_gpu_gate_resume(&g);
    worker_join(&w);
    CHECK(atomic_load_explicit(&w.past, memory_order_acquire));    /* ... and does after the resume */
    CHECK(host_gpu_gate_sections(&g) == 0);
}

/* The same stage ending before the suspend: nothing parks, the checkpoint is the open-gate load, and the drain
 * that follows waits for the section as usual. */
static void case_checkpoint_open_then_drain(void)
{
    NEW_GATE(g);
    worker w;
    worker_start(&w, &g, fn_blocked_section, 0);
    spin_flag(&w.inside);
    atomic_store(&w.release, 1);
    worker_join(&w);
    CHECK(atomic_load_explicit(&w.past, memory_order_acquire));
    CHECK(host_gpu_gate_parked_total(&g) == 0);
    CHECK(host_gpu_gate_sections(&g) == 0);
}

/* A worker paused after its checkpoint passed open, inside its section, delays the suspend until it has committed
 * and left: the suspend needs no timeout path, because nothing in a section waits for anything but the CPU. */
static void case_paused_section_delays_suspend_until_commit(void)
{
    NEW_GATE(g);
    worker w;
    worker_start(&w, &g, fn_checkpointed_section, 0);
    spin_flag(&w.inside);

    worker s;
    worker_start(&s, &g, fn_suspend, LONG_NS);
    spin_until(&g, host_gpu_gate_drainers, 1, &s, 1);
    CHECK(!worker_done(&s));                                   /* the suspend is waiting for the section */
    CHECK(!atomic_load_explicit(&w.past, memory_order_acquire));

    atomic_store(&w.release, 1);                               /* the delay ends: it commits, then leaves */
    worker_join(&w);
    worker_join(&s);
    CHECK(atomic_load_explicit(&w.past, memory_order_acquire));
    CHECK(s.result == 1);                                      /* drained by the leave, not by a timeout */
    CHECK(host_gpu_gate_sections(&g) == 0);
    host_gpu_gate_resume(&g);
}

/* A worker blocked in the semaphore wait (outside the section) does not delay the suspend at all, and cannot
 * start its next section until the resume. */
static void case_semaphore_wait_does_not_delay_suspend(void)
{
    NEW_GATE(g);
    worker w;
    worker_start(&w, &g, fn_semaphore_section, 0);
    spin_flag(&w.inside);                                      /* it is in the wait: no section open */
    CHECK(host_gpu_gate_sections(&g) == 0);

    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);            /* returns here and now: nothing to drain */
    CHECK(host_gpu_gate_drainers(&g) == 0);

    atomic_store(&w.release, 1);                               /* the wait ends with the gate closed */
    spin_until(&g, host_gpu_gate_waiters, 1, &w, 1);
    CHECK(!atomic_load_explicit(&w.past, memory_order_acquire));
    CHECK(host_gpu_gate_sections(&g) == 0);

    host_gpu_gate_resume(&g);
    worker_join(&w);
    CHECK(atomic_load_explicit(&w.past, memory_order_acquire));
    CHECK(host_gpu_gate_sections(&g) == 0);
}

/* Suspend and resume repeated, a fresh waiter parked and released each round: the gate is reusable and the count
 * of waits that ever parked is exact. */
static void case_suspend_resume_cycles(void)
{
    NEW_GATE(g);
    const uint32_t t0 = host_gpu_gate_ticket(&g);
    enum { ROUNDS = 200 };
    for (uint32_t round = 1; round <= ROUNDS; round++) {
        CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
        worker w;
        worker_start(&w, &g, fn_wait_active, 0);
        spin_until(&g, host_gpu_gate_waiters, 1, &w, 1);
        CHECK(!worker_done(&w));
        host_gpu_gate_resume(&g);
        worker_join(&w);
        CHECK(worker_done(&w));
        CHECK(host_gpu_gate_parked_total(&g) == round);
    }
    CHECK(host_gpu_gate_is_active(&g));
    CHECK(host_gpu_gate_waiters(&g) == 0);
    CHECK(host_gpu_gate_ticket(&g) != t0);
}

/* A redundant suspend while a waiter is parked wakes nothing and changes nothing. */
static void case_redundant_suspend_while_parked(void)
{
    NEW_GATE(g);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    worker w;
    worker_start(&w, &g, fn_enter, 0);
    spin_until(&g, host_gpu_gate_waiters, 1, &w, 1);
    const uint32_t down = host_gpu_gate_ticket(&g);
    for (int i = 0; i < 100; i++) CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    CHECK(host_gpu_gate_ticket(&g) == down);
    CHECK(!worker_done(&w));
    CHECK(host_gpu_gate_waiters(&g) == 1);
    host_gpu_gate_resume(&g);
    worker_join(&w);
    host_gpu_gate_leave(&g);
    CHECK(host_gpu_gate_sections(&g) == 0);
}

/* The grace an allocation gives the host's notification: wait_change() returns 0 when the ticket stays (having
 * waited at least the bound on the monotonic clock), and 1 as soon as a suspend (or a resume) moves it. */
static void case_wait_change(void)
{
    NEW_GATE(g);
    const uint32_t t0 = host_gpu_gate_ticket(&g);
    const uint64_t m0 = mono_ns();
    CHECK(host_gpu_gate_wait_change(&g, t0, 20ull * NS_PER_MS) == 0);   /* nothing changed: the bound ran out */
    CHECK(mono_ns() - m0 >= 20ull * NS_PER_MS);
    CHECK(host_gpu_gate_watchers(&g) == 0);

    worker c;
    worker_start(&c, &g, fn_wait_change, t0);
    spin_until(&g, host_gpu_gate_watchers, 1, &c, 1);
    CHECK(!worker_done(&c));
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    worker_join(&c);
    CHECK(c.result == 1);
    CHECK(host_gpu_gate_watchers(&g) == 0);

    /* Already changed: returns at once, before any wait. */
    CHECK(host_gpu_gate_wait_change(&g, t0, 1ull * NS_PER_MS) == 1);
    host_gpu_gate_resume(&g);
}

/* The decision a failed allocation takes: `before` is the ticket taken once the section was open, `after` the one
 * taken once the call returned nil, `retries` the lifecycle retries already spent on this allocation. */
static void case_alloc_verdict(void)
{
    NEW_GATE(g);

    /* Open before and after, nothing moved: the host may not have said so yet. Give its notification a grace. */
    uint32_t before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), 0) == HOST_GPU_ALLOC_GRACE);

    /* The gate went down during the call (the notification raced the allocation). */
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), 0) == HOST_GPU_ALLOC_WAIT_RETRY);

    /* It was already down when the call started, and still is. */
    before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), 0) == HOST_GPU_ALLOC_WAIT_RETRY);

    /* It was down when the call started and came back during it: the allocation ran in the dark. */
    host_gpu_gate_resume(&g);
    CHECK(host_gpu_gate_is_active(&g));
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), 0) == HOST_GPU_ALLOC_WAIT_RETRY);

    /* A whole suspend and resume inside the call: open at both ends, but the host did refuse in between. */
    before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    host_gpu_gate_resume(&g);
    CHECK(host_gpu_gate_is_active(&g));
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), 0) == HOST_GPU_ALLOC_WAIT_RETRY);

    /* The pure function itself, on bare tickets: bit 0 is suspended, the rest counts transitions. */
    CHECK(host_gpu_alloc_failed(4u, 4u, 0) == HOST_GPU_ALLOC_GRACE);
    CHECK(host_gpu_alloc_failed(5u, 5u, 0) == HOST_GPU_ALLOC_WAIT_RETRY);
    CHECK(host_gpu_alloc_failed(4u, 5u, 0) == HOST_GPU_ALLOC_WAIT_RETRY);
    CHECK(host_gpu_alloc_failed(5u, 6u, 0) == HOST_GPU_ALLOC_WAIT_RETRY);
    CHECK(host_gpu_alloc_failed(4u, 8u, 0) == HOST_GPU_ALLOC_WAIT_RETRY);
}

/* The retry is bounded, but only an ACTIVE failure after it stops. With the one retry spent, a failure whose
 * after-ticket shows the gate suspended is the host's: no stop, wait for the resume. */
static void case_retry_is_bounded(void)
{
    CHECK(HOST_GPU_ALLOC_MAX_RETRIES == 1u);
    static const uint32_t tickets[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0xFFFFFFFEu, 0xFFFFFFFFu };
    const unsigned n = sizeof tickets / sizeof tickets[0];
    for (unsigned retries = HOST_GPU_ALLOC_MAX_RETRIES; retries < HOST_GPU_ALLOC_MAX_RETRIES + 3u; retries++)
        for (unsigned i = 0; i < n; i++)
            for (unsigned j = 0; j < n; j++)
                CHECK(host_gpu_alloc_failed(tickets[i], tickets[j], retries) ==
                      (host_gpu_ticket_active(tickets[j]) ? HOST_GPU_ALLOC_STOP : HOST_GPU_ALLOC_WAIT_RETRY));

    /* The way the backend drives it: a failure with the gate steady gets one grace; when the notification then
     * arrives it is the one retry. */
    NEW_GATE(g);
    unsigned retries = 0;
    uint32_t before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), retries) == HOST_GPU_ALLOC_GRACE);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);                  /* the grace saw the notification */
    CHECK(host_gpu_gate_wait_change(&g, before, 1ull * NS_PER_MS) == 1);
    retries++;
    host_gpu_gate_resume(&g);

    /* The second attempt fails with the gate closed again (the app went inactive once more): not the device's.
     * No stop until the gate is active again, and the allowance is not spent by it. */
    before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_gate_suspend(&g, LONG_NS) == 1);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), retries) == HOST_GPU_ALLOC_WAIT_RETRY);
    host_gpu_gate_resume(&g);

    /* The third attempt fails with the gate steady and active: that one stops. */
    before = host_gpu_gate_ticket(&g);
    CHECK(host_gpu_alloc_failed(before, host_gpu_gate_ticket(&g), retries) == HOST_GPU_ALLOC_STOP);
}

int main(void)
{
    case_open_does_not_block();
    case_idempotent();
    case_wait_active_blocks_until_resume();
    case_releases_all_waiters();
    case_open_section_delays_suspend();
    case_entry_after_suspend_blocks();
    case_drain_bound();
    case_checkpoint_stops_a_late_section();
    case_checkpoint_open_then_drain();
    case_paused_section_delays_suspend_until_commit();
    case_semaphore_wait_does_not_delay_suspend();
    case_suspend_resume_cycles();
    case_redundant_suspend_while_parked();
    case_wait_change();
    case_alloc_verdict();
    case_retry_is_bounded();
    puts("host_gpu_gate: ok");
    return 0;
}
