/*
 * ps3recomp - a gate in front of the host GPU while the host forbids it
 *
 * iOS denies GPU resource creation and command submission to an app that is
 * not active (the screen locked or dimmed, the app switcher, the background).
 * A backend that went on would see every allocation return nil and every
 * command buffer fail, drop the guest's draws, and, because the guest waits on
 * results that never come, never recover after the app returned. The gate
 * makes the RSX thread wait instead, and it is coordinated with the host's
 * notification so the host never takes the GPU away from a submission that is
 * half done:
 *
 *   - A thread brackets every unit of GPU work (an allocation, or a whole
 *     encode-and-commit from the semaphore wait through nextDrawable and
 *     commit) with enter() and leave(): a "section". enter() blocks while the
 *     gate is suspended.
 *   - The thread that learns the host is leaving (the UI thread, inside
 *     will-resign-active, while iOS still allows GPU work) calls suspend(): it
 *     closes the gate, then waits, bounded, for the sections already open to
 *     leave. Nothing enters after the gate closed, so when suspend() returns 1
 *     no thread is inside a section and none will start one until resume().
 *   - resume() reopens the gate and releases every thread parked in enter().
 *
 * While the gate is suspended the thread that called enter() blocks, so the
 * FIFO is not consumed and the guest's graphics stall with it. Nothing is
 * dropped and nothing is guessed.
 *
 * A gate is an object, not a global: the backend owns one, the unit test builds
 * its own. It is header-only on purpose: the Metal backend is also compiled
 * whole into fixtures (#include "rsx_metal_backend.m") whose link lines list
 * exactly the sources they need, and a gate in its own translation unit would
 * have to be added to every one of them for no gain. C and Objective-C only
 * (the state is C11 _Atomic).
 *
 * Contract:
 *   - enter()/leave() on an open gate are one atomic add each and one load: no
 *     lock. wait_active() is one acquire load.
 *   - Sections do not nest on one thread: a thread inside a section that
 *     entered again while suspend() waits for it would wait for itself (the
 *     bound would end it, a second late). The backend's in-section code calls
 *     the Metal API directly.
 *   - A section must never wait for the thread that calls suspend(): it would
 *     wait for a thread that waits for it. The bound ends the stand-off
 *     (suspend() returns 0 and the caller logs it); it does not make it right.
 *   - suspend() and resume() are idempotent. Only a real transition changes the
 *     ticket, so the host's overlapping notifications (will-resign-active, then
 *     did-enter-background) are one suspension.
 *   - A thread that must not stall (the UI thread) never calls enter() or
 *     wait_active(). suspend() on it is bounded by `timeout_ns`.
 */
#ifndef PS3RECOMP_HOST_GPU_GATE_H
#define PS3RECOMP_HOST_GPU_GATE_H

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#endif

typedef struct host_gpu_gate {
    /* Bit 0: suspended. Bits 1..31: how many times the gate changed state. Written under `lock`, read without. */
    _Atomic uint32_t word;
    /* Threads inside a section. Incremented by enter() before it looks at `word`, and read by suspend() after it
     * wrote `word` (both seq_cst): either enter() sees the gate closed and backs out, or suspend() sees the
     * section and waits for it. There is no third outcome. */
    _Atomic uint32_t sections;
    /* Threads parked in enter()/wait_active(), threads inside suspend() waiting for sections to leave, threads
     * inside wait_change(), and waits that ever parked. All change under `lock`; a test reads them to know a
     * thread is blocked without sleeping, and to know the open path never parked. */
    _Atomic uint32_t waiters;
    _Atomic uint32_t drainers;
    _Atomic uint32_t watchers;
    _Atomic uint32_t parked_total;
#ifdef _WIN32
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t lock;
    pthread_cond_t cv;
#endif
} host_gpu_gate;

/* Static initialiser, where the platform's condition variable can wait on the monotonic clock as created: Apple
 * (its relative timed wait is monotonic) and Windows. Elsewhere a condition variable must be created with
 * CLOCK_MONOTONIC (pthread_condattr_setclock), so the gate is built by host_gpu_gate_init(). */
#ifdef _WIN32
#  define HOST_GPU_GATE_INIT { 0, 0, 0, 0, 0, 0, SRWLOCK_INIT, CONDITION_VARIABLE_INIT }
#elif defined(__APPLE__)
#  define HOST_GPU_GATE_INIT { 0, 0, 0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
#endif

/* Build a gate at run time; works on every platform. Returns 0, or the pthread error (the gate is then unusable). */
static inline int host_gpu_gate_init(host_gpu_gate* g)
{
    atomic_init(&g->word, 0);
    atomic_init(&g->sections, 0);
    atomic_init(&g->waiters, 0);
    atomic_init(&g->drainers, 0);
    atomic_init(&g->watchers, 0);
    atomic_init(&g->parked_total, 0);
#ifdef _WIN32
    InitializeSRWLock(&g->lock);
    InitializeConditionVariable(&g->cv);
    return 0;
#else
    int rc = pthread_mutex_init(&g->lock, NULL);
    if (rc != 0) return rc;
#  if defined(__APPLE__)
    return pthread_cond_init(&g->cv, NULL);
#  else
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&g->cv, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#  endif
#endif
}

static inline void host_gpu_gate__lock(host_gpu_gate* g)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&g->lock);
#else
    pthread_mutex_lock(&g->lock);
#endif
}

static inline void host_gpu_gate__unlock(host_gpu_gate* g)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g->lock);
#else
    pthread_mutex_unlock(&g->lock);
#endif
}

static inline void host_gpu_gate__broadcast(host_gpu_gate* g)
{
#ifdef _WIN32
    WakeAllConditionVariable(&g->cv);
#else
    pthread_cond_broadcast(&g->cv);
#endif
}

/* The monotonic clock every bound is measured on: a wall-clock step (NTP, a manual change, a VM resume) must not
 * shorten or stretch the drain or the grace. CLOCK_MONOTONIC counts across sleep on Darwin, as the relative wait
 * below does not; a bound that ran out while the device slept is simply over, which is the safe side. */
static inline uint64_t host_gpu_gate__now_ns(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64() * 1000000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* The wait point, `lock` held. deadline_ns == UINT64_MAX waits without a deadline. Returns 0 when woken (the
 * caller re-tests its predicate) and ETIMEDOUT once `deadline_ns` (absolute, from host_gpu_gate__now_ns) passed. */
static inline int host_gpu_gate__wait(host_gpu_gate* g, uint64_t deadline_ns)
{
#ifdef _WIN32
    DWORD ms = INFINITE;
    if (deadline_ns != UINT64_MAX) {
        const uint64_t now = host_gpu_gate__now_ns();
        ms = deadline_ns > now ? (DWORD)((deadline_ns - now) / 1000000ull + 1ull) : 0;
    }
    return SleepConditionVariableSRW(&g->cv, &g->lock, ms, 0) ? 0 : ETIMEDOUT;
#else
    if (deadline_ns == UINT64_MAX) return pthread_cond_wait(&g->cv, &g->lock);
#  if defined(__APPLE__)
    /* Apple has no pthread_condattr_setclock; its relative wait runs on the monotonic clock. */
    const uint64_t now = host_gpu_gate__now_ns();
    if (deadline_ns <= now) return ETIMEDOUT;
    struct timespec rel;
    rel.tv_sec = (time_t)((deadline_ns - now) / 1000000000ull);
    rel.tv_nsec = (long)((deadline_ns - now) % 1000000000ull);
    const int rc = pthread_cond_timedwait_relative_np(&g->cv, &g->lock, &rel);
    /* The relative wait and the clock above are separate timers: a return that is a hair early is a wake, and the
     * caller's predicate loop waits again, so a bound is never shorter than asked. */
    return (rc == ETIMEDOUT && host_gpu_gate__now_ns() < deadline_ns) ? 0 : rc;
#  else
    struct timespec ts;   /* the condition variable was created on CLOCK_MONOTONIC (host_gpu_gate_init) */
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    return pthread_cond_timedwait(&g->cv, &g->lock, &ts);
#  endif
#endif
}

/* The state word: a snapshot for host_gpu_alloc_failed() and host_gpu_ticket_active(). */
static inline uint32_t host_gpu_gate_ticket(host_gpu_gate* g)
{
    return atomic_load_explicit(&g->word, memory_order_acquire);
}

static inline int host_gpu_ticket_active(uint32_t ticket)
{
    return (ticket & 1u) == 0;
}

static inline int host_gpu_gate_is_active(host_gpu_gate* g)
{
    return host_gpu_ticket_active(host_gpu_gate_ticket(g));
}

/* The host gave the GPU back. Releases every thread parked in enter() or wait_active() and every wait_change().
 * Idempotent. */
static inline void host_gpu_gate_resume(host_gpu_gate* g)
{
    host_gpu_gate__lock(g);
    const uint32_t cur = atomic_load_explicit(&g->word, memory_order_relaxed);
    if (cur & 1u) {
        atomic_store_explicit(&g->word, ((cur >> 1) + 1u) << 1, memory_order_seq_cst);
        host_gpu_gate__broadcast(g);
    }
    host_gpu_gate__unlock(g);
}

/* The host stopped letting this process use the GPU. Closes the gate, then waits up to `timeout_ns` for the
 * sections already open to leave. Returns 1 when none is open any more (and none can open before resume()), 0
 * when the bound ran out first; the gate stays closed either way. Idempotent: a second call on a closed gate
 * only waits for the drain again. Callable from any thread that is not itself inside a section. */
static inline int host_gpu_gate_suspend(host_gpu_gate* g, uint64_t timeout_ns)
{
    host_gpu_gate__lock(g);
    const uint32_t cur = atomic_load_explicit(&g->word, memory_order_relaxed);
    if (!(cur & 1u)) {
        atomic_store_explicit(&g->word, (((cur >> 1) + 1u) << 1) | 1u, memory_order_seq_cst);
        host_gpu_gate__broadcast(g);   /* a wait_change() on the old ticket */
    }
    const uint64_t deadline = host_gpu_gate__now_ns() + timeout_ns;
    atomic_fetch_add_explicit(&g->drainers, 1u, memory_order_relaxed);
    int drained = 1;
    while (atomic_load_explicit(&g->sections, memory_order_seq_cst) != 0) {
        if (host_gpu_gate__wait(g, deadline) == ETIMEDOUT) {
            drained = atomic_load_explicit(&g->sections, memory_order_seq_cst) == 0;
            break;
        }
    }
    atomic_fetch_sub_explicit(&g->drainers, 1u, memory_order_relaxed);
    host_gpu_gate__unlock(g);
    return drained;
}

/* Returns at once while the gate is active; blocks until resume() while it is suspended. A barrier only: it opens
 * no section, so the gate can close again the instant it returns. Use enter() around work the host must not cut. */
static inline void host_gpu_gate_wait_active(host_gpu_gate* g)
{
    if (host_gpu_ticket_active(atomic_load_explicit(&g->word, memory_order_acquire))) return;
    host_gpu_gate__lock(g);
    atomic_fetch_add_explicit(&g->parked_total, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&g->waiters, 1u, memory_order_relaxed);
    while (!host_gpu_ticket_active(atomic_load_explicit(&g->word, memory_order_relaxed)))
        host_gpu_gate__wait(g, UINT64_MAX);
    atomic_fetch_sub_explicit(&g->waiters, 1u, memory_order_relaxed);
    host_gpu_gate__unlock(g);
}

/* Close a section. When it was the last one and a suspend() is draining, wake it: the broadcast is made under
 * the lock, after the decrement, so a drainer that tested `sections` before the decrement is already waiting. */
static inline void host_gpu_gate_leave(host_gpu_gate* g)
{
    if (atomic_fetch_sub_explicit(&g->sections, 1u, memory_order_seq_cst) == 1u &&
        !host_gpu_ticket_active(atomic_load_explicit(&g->word, memory_order_seq_cst))) {
        host_gpu_gate__lock(g);
        host_gpu_gate__broadcast(g);
        host_gpu_gate__unlock(g);
    }
}

/* Open a section: returns once the gate is active and this thread is counted inside it. Blocks while the gate
 * is suspended, including while a suspend() is still draining the sections opened before it. */
static inline void host_gpu_gate_enter(host_gpu_gate* g)
{
    for (;;) {
        atomic_fetch_add_explicit(&g->sections, 1u, memory_order_seq_cst);
        if (host_gpu_ticket_active(atomic_load_explicit(&g->word, memory_order_seq_cst))) return;
        host_gpu_gate_leave(g);          /* the gate closed first: not inside after all */
        host_gpu_gate_wait_active(g);
    }
}

/* A checkpoint for a thread that is inside a section, at the point where a blocking stage (a semaphore wait, a
 * drawable wait) has just ended and the next call would touch the GPU. Open gate: one load. Closed gate: the
 * thread leaves its section (so a suspend() draining it is released) and enters again, which blocks until
 * resume(). A section can therefore never issue a GPU call while the gate is closed, whatever the drain did: a
 * suspend() whose bound ran out has left this section open, and the thread stops here, not in Metal. Anything the
 * thread holds across the checkpoint (a drawable, a half-encoded command buffer) stays valid or fails like any
 * resource that waited out a pause. */
static inline void host_gpu_gate_checkpoint(host_gpu_gate* g)
{
    if (host_gpu_ticket_active(atomic_load_explicit(&g->word, memory_order_seq_cst))) return;
    host_gpu_gate_leave(g);
    host_gpu_gate_enter(g);
}

/* Waits up to `timeout_ns` for the ticket to differ from `ticket`. Returns 1 when it did (the host's
 * notification arrived), 0 when the bound ran out. */
static inline int host_gpu_gate_wait_change(host_gpu_gate* g, uint32_t ticket, uint64_t timeout_ns)
{
    host_gpu_gate__lock(g);
    const uint64_t deadline = host_gpu_gate__now_ns() + timeout_ns;
    atomic_fetch_add_explicit(&g->watchers, 1u, memory_order_relaxed);
    while (atomic_load_explicit(&g->word, memory_order_relaxed) == ticket)
        if (host_gpu_gate__wait(g, deadline) == ETIMEDOUT) break;
    const int changed = atomic_load_explicit(&g->word, memory_order_relaxed) != ticket;
    atomic_fetch_sub_explicit(&g->watchers, 1u, memory_order_relaxed);
    host_gpu_gate__unlock(g);
    return changed;
}

static inline uint32_t host_gpu_gate__count(host_gpu_gate* g, _Atomic uint32_t* what)
{
    host_gpu_gate__lock(g);
    const uint32_t n = atomic_load_explicit(what, memory_order_relaxed);
    host_gpu_gate__unlock(g);
    return n;
}

/* Observation, for tests: how many threads are parked, draining, watching, ever parked, or inside a section. */
static inline uint32_t host_gpu_gate_waiters(host_gpu_gate* g) { return host_gpu_gate__count(g, &g->waiters); }
static inline uint32_t host_gpu_gate_drainers(host_gpu_gate* g) { return host_gpu_gate__count(g, &g->drainers); }
static inline uint32_t host_gpu_gate_watchers(host_gpu_gate* g) { return host_gpu_gate__count(g, &g->watchers); }
static inline uint32_t host_gpu_gate_parked_total(host_gpu_gate* g) { return host_gpu_gate__count(g, &g->parked_total); }
static inline uint32_t host_gpu_gate_sections(host_gpu_gate* g)
{
    return atomic_load_explicit(&g->sections, memory_order_seq_cst);
}

/* What a failed GPU allocation means, decided from the two tickets taken around the call (the first once the
 * section was open, the second after the call returned nil) and the lifecycle retries already spent on this
 * allocation:
 *   - the gate is suspended in the after-ticket: the host's, whatever was spent. WAIT_RETRY: wait for the gate to
 *     be active, open a new section, and try again. Only an active failure can be the device's, so this never
 *     stops, and a retry does not use up the allowance.
 *   - the gate is active and the one retry was already spent: STOP. One lifecycle race is believable; a second
 *     failure with the gate open is not, and an unbounded retry would turn a refusing device into a hang.
 *   - the gate is active but moved during the call (the tickets differ: it closed and opened again, or was closed
 *     when the call began): the host's notification raced the allocation. WAIT_RETRY.
 *   - the gate stood active across the whole call: the host may still not have said so. Metal can refuse before
 *     the UI thread's will-resign-active has run, so GRACE: wait a bounded time for the ticket to change
 *     (host_gpu_gate_wait_change); if it does, that is a WAIT_RETRY, if not the refusal is the device's: STOP.
 * Pure: no state, so the decision is testable without a GPU. */
typedef enum host_gpu_alloc_verdict {
    HOST_GPU_ALLOC_STOP = 0,
    HOST_GPU_ALLOC_WAIT_RETRY = 1,
    HOST_GPU_ALLOC_GRACE = 2
} host_gpu_alloc_verdict;

#define HOST_GPU_ALLOC_MAX_RETRIES 1u

static inline host_gpu_alloc_verdict host_gpu_alloc_failed(uint32_t ticket_before, uint32_t ticket_after,
                                                           unsigned retries_done)
{
    if (!host_gpu_ticket_active(ticket_after)) return HOST_GPU_ALLOC_WAIT_RETRY;
    if (retries_done >= HOST_GPU_ALLOC_MAX_RETRIES) return HOST_GPU_ALLOC_STOP;
    if (ticket_before != ticket_after) return HOST_GPU_ALLOC_WAIT_RETRY;
    return HOST_GPU_ALLOC_GRACE;
}

#endif /* PS3RECOMP_HOST_GPU_GATE_H */
