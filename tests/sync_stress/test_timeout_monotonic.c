/*
 * The timed waits of the lv2 sync objects, driven through an expiry on the
 * REAL object code with a scripted clock and scripted native waits.
 *
 * sys_event.c, sys_semaphore.c, sys_cond.c and sys_mutex.c are the real ones.
 * Only the bottom of the wait is replaced: instead of runtime/platform/
 * host_wait.c this links tests' host_wait_fake.c, which IS host_wait.c compiled
 * with a scripted monotonic clock and scripted native waits (see
 * host_wait_fake.h). A native wait releases the object's own lock, runs a hook
 * (a push, a post, a signal), moves the clock and takes the lock back, exactly
 * as a condition wait does, so the object's bookkeeping runs under its real
 * lock. Nothing sleeps and nothing depends on the host's speed.
 *
 * Pinned, per object: the guest timeout (microseconds) reaches the native wait
 * as that duration; a wake that is not an arrival goes back to sleep for what
 * is left; expiry is CELL_ETIMEDOUT with the waiter counts balanced and nothing
 * consumed; an arrival at the instant of expiry follows the object's existing
 * policy (queue and condition: taken; flag and semaphore: the timeout wins);
 * timeout 0 is the infinite wait, whatever the clock says; a host refusal is
 * CELL_EFAULT with the object left usable, never a timeout and never a lock
 * taken. A condition wait that expires still re-takes the guest mutex at its
 * full recursion depth.
 *
 * Built twice (timeout_monotonic_abs / _rel), one per native backend.
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include "runtime/syscalls/sys_event.h"
#include "runtime/syscalls/sys_mutex.h"
#include "runtime/syscalls/sys_semaphore.h"
#include "runtime/platform/tests/host_wait_fake.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

uint8_t* vm_base;

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{ (void)image; (void)image_size; (void)args_ea; return 0; }
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
uint32_t yz_thread_current_id(void) { return 0; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define CHECK_RC(call, want) do { int32_t _rc = (int32_t)(call); \
    if (_rc != (int32_t)(want)) { fprintf(stderr, "FAIL line %d: %s -> 0x%08X, want 0x%08X\n", \
        __LINE__, #call, (unsigned)_rc, (unsigned)(want)); failures++; } } while (0)

#define US 1000ull
#define MS 1000000ull
#define SEC 1000000000ull
#define BASE (1000ull * SEC)
#define WALL (1700000000ull * SEC)

typedef int64_t (*sc_fn)(ppu_context*);
static ppu_context g_ctx;                                 /* the last call's registers: r4..r7 carry a receive's event */

static int32_t call(sc_fn f, uint64_t tid, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
    /* A local context: a hook that runs inside a wait makes its own call and must not reuse the live one. */
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = tid;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d; ctx.gpr[7] = e;
    const int32_t rc = (int32_t)f(&ctx);
    g_ctx = ctx;
    return rc;
}
static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t be_read64(unsigned off) { return (uint64_t)be_read(off) << 32 | be_read(off + 4); }

static void begin(void) { hw_fake_reset(BASE, WALL); }

static void finish(void)
{
    CHECK(hw_fake_failures() == 0);
    CHECK(hw_fake_steps_left() == 0);
}

static void push(int kind, int rc, uint64_t advance, uint64_t want_rel, hw_fake_hook hook, void* arg)
{
    hw_fake_step s;
    memset(&s, 0, sizeof s);
    s.kind = kind; s.rc = rc; s.advance_ns = advance; s.want_rel_ns = want_rel;
    s.hook = hook; s.hook_arg = arg;
    hw_fake_push(&s);
}
static unsigned timed_calls(void) { hw_fake_stats st; hw_fake_get_stats(&st); return st.timed_calls; }
static unsigned infinite_calls(void) { hw_fake_stats st; hw_fake_get_stats(&st); return st.infinite_calls; }

/* ---- event queue --------------------------------------------------------- */
static uint32_t make_queue(void)
{
    CHECK_RC(call(sys_event_queue_create, 1, 0x200, 0, 0x1234, 8, 0), CELL_OK);
    return be_read(0x200);
}
static void push_event_hook(void* q) { CHECK(sys_event_queue_push_by_id((uint32_t)(uintptr_t)q, 0xAA, 1, 2, 3) == 0); }

static void test_queue(void)
{
    const uint32_t q = make_queue();
    sys_event_queue_info* qi = &g_sys_event_queues[q - 1];

    /* A timeout of 1000 us is a 1 ms request, and its expiry is ETIMEDOUT with the count balanced. */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 1 * MS, 1 * MS, NULL, NULL);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_ETIMEDOUT);
    CHECK(qi->waiters == 0 && qi->count == 0 && qi->active);
    CHECK(hw_fake_now() - BASE == 1 * MS);
    finish();

    /* Wakes that find the queue empty do not extend the timeout: five 100 us false wakes, then the last 500 us. */
    begin();
    for (unsigned i = 0; i < 5; i++) push(HW_FAKE_TIMED, 0, 100 * US, (1000 - 100 * i) * US, NULL, NULL);
    push(HW_FAKE_TIMED, ETIMEDOUT, 500 * US, 500 * US, NULL, NULL);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_ETIMEDOUT);
    CHECK(hw_fake_now() - BASE == 1 * MS && qi->waiters == 0);
    finish();

    /* An event pushed while the receiver sleeps is delivered in registers r4..r7 and the memory buffer. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 1 * MS, push_event_hook, (void*)(uintptr_t)q);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_OK);
    CHECK(g_ctx.gpr[4] == 0xAA && g_ctx.gpr[5] == 1 && g_ctx.gpr[6] == 2 && g_ctx.gpr[7] == 3);
    CHECK(qi->count == 0 && qi->waiters == 0);
    finish();

    /* An event that lands at the instant of expiry is still taken: the loop re-tests the count first. */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 1 * MS, 1 * MS, push_event_hook, (void*)(uintptr_t)q);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_OK);
    CHECK(qi->count == 0 && qi->waiters == 0);
    finish();

    /* Timeout 0 is infinite: the infinite native wait, not an immediate expiry, however far the clock goes. */
    begin();
    push(HW_FAKE_INFINITE, 0, 3600ull * SEC, HW_FAKE_ANY, push_event_hook, (void*)(uintptr_t)q);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 0, 0, 0), CELL_OK);
    CHECK(timed_calls() == 0 && infinite_calls() == 1 && qi->waiters == 0);
    finish();

    /* A host refusal is CELL_EFAULT (not a timeout), the waiter is unregistered and the queue still works. */
    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 1 * MS, NULL, NULL);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_EFAULT);
    CHECK(qi->waiters == 0 && qi->count == 0);
    push(HW_FAKE_INFINITE, EINVAL, 0, HW_FAKE_ANY, NULL, NULL);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 0, 0, 0), CELL_EFAULT);
    CHECK(qi->waiters == 0);
    finish();
    begin();
    CHECK(sys_event_queue_push_by_id(q, 0xBB, 4, 5, 6) == 0);
    CHECK_RC(call(sys_event_queue_receive, 2, q, 0x300, 1000, 0, 0), CELL_OK);   /* ready: no wait at all */
    CHECK(g_ctx.gpr[4] == 0xBB && timed_calls() == 0);
    finish();

    CHECK_RC(call(sys_event_queue_destroy, 1, q, 0, 0, 0, 0), CELL_OK);          /* idle: a plain destroy works */
}

/* ---- event flag ---------------------------------------------------------- */
static uint32_t g_flag_id;
static void set_flag_hook(void* a) { (void)a; CHECK_RC(call(sys_event_flag_set, 9, g_flag_id, 0x1, 0, 0, 0), CELL_OK); }

static void test_flag(void)
{
    CHECK_RC(call(sys_event_flag_create, 1, 0x210, 0, 0, 0, 0), CELL_OK);
    g_flag_id = be_read(0x210);
    sys_event_flag_info* f = &g_sys_event_flags[g_flag_id - 1];

    /* Expiry: ETIMEDOUT, the current pattern written to the result address, nothing cleared, count balanced. */
    CHECK_RC(call(sys_event_flag_set, 9, g_flag_id, 0x8, 0, 0, 0), CELL_OK);
    memset(vm_base + 0x300, 0xEE, 8);
    begin();
    push(HW_FAKE_TIMED, 0, 200 * US, 1 * MS, NULL, NULL);                        /* a false wake first */
    push(HW_FAKE_TIMED, ETIMEDOUT, 800 * US, 800 * US, NULL, NULL);
    CHECK_RC(call(sys_event_flag_wait, 2, g_flag_id, 0x1, SYS_EVENT_FLAG_WAIT_OR | SYS_EVENT_FLAG_WAIT_CLEAR, 0x300, 1000), CELL_ETIMEDOUT);
    CHECK(be_read64(0x300) == 0x8 && f->pattern == 0x8 && f->waiters == 0);
    CHECK(hw_fake_now() - BASE == 1 * MS);
    finish();

    /* The bit arrives while the waiter sleeps: satisfied, matched bits cleared, pattern reported. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 1 * MS, set_flag_hook, NULL);
    CHECK_RC(call(sys_event_flag_wait, 2, g_flag_id, 0x1, SYS_EVENT_FLAG_WAIT_OR | SYS_EVENT_FLAG_WAIT_CLEAR, 0x300, 1000), CELL_OK);
    CHECK(be_read64(0x300) == 0x9 && f->pattern == 0x8 && f->waiters == 0);
    finish();

    /* The bit arrives at the instant of expiry: the timeout wins, as the flag always did; the bit stays set. */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 1 * MS, 1 * MS, set_flag_hook, NULL);
    CHECK_RC(call(sys_event_flag_wait, 2, g_flag_id, 0x1, SYS_EVENT_FLAG_WAIT_OR, 0x300, 1000), CELL_ETIMEDOUT);
    CHECK((f->pattern & 1) == 1 && f->waiters == 0);
    finish();
    CHECK_RC(call(sys_event_flag_clear, 9, g_flag_id, ~0x1ull, 0, 0, 0), CELL_OK);

    /* Timeout 0 is infinite. */
    begin();
    push(HW_FAKE_INFINITE, 0, 3600ull * SEC, HW_FAKE_ANY, set_flag_hook, NULL);
    CHECK_RC(call(sys_event_flag_wait, 2, g_flag_id, 0x1, SYS_EVENT_FLAG_WAIT_OR, 0x300, 0), CELL_OK);
    CHECK(timed_calls() == 0 && infinite_calls() == 1 && f->waiters == 0);
    finish();
    CHECK_RC(call(sys_event_flag_clear, 9, g_flag_id, ~0x1ull, 0, 0, 0), CELL_OK);

    /* A host refusal. */
    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 1 * MS, NULL, NULL);
    CHECK_RC(call(sys_event_flag_wait, 2, g_flag_id, 0x1, SYS_EVENT_FLAG_WAIT_OR, 0x300, 1000), CELL_EFAULT);
    CHECK(f->waiters == 0);
    finish();

    CHECK_RC(call(sys_event_flag_destroy, 1, g_flag_id, 0, 0, 0, 0), CELL_OK);
}

/* ---- semaphore ----------------------------------------------------------- */
static uint32_t g_sem_id;
static void post_sem_hook(void* a) { (void)a; CHECK_RC(call(sys_semaphore_post, 9, g_sem_id, 1, 0, 0, 0), CELL_OK); }

static void test_semaphore(void)
{
    CHECK_RC(call(sys_semaphore_create, 1, 0x220, 0, 0, 10, 0), CELL_OK);
    g_sem_id = be_read(0x220);
    sys_semaphore_info* s = &g_sys_semaphores[g_sem_id - 1];

    /* Expiry after a false wake: ETIMEDOUT, no token consumed, the waiter count balanced. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 500 * US, NULL, NULL);
    push(HW_FAKE_TIMED, ETIMEDOUT, 400 * US, 400 * US, NULL, NULL);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 500, 0, 0, 0), CELL_ETIMEDOUT);
    CHECK(s->value == 0 && s->waiters == 0);
    CHECK(hw_fake_now() - BASE == 500 * US);
    finish();

    /* A post while the waiter sleeps: the token is taken. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 500 * US, post_sem_hook, NULL);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 500, 0, 0, 0), CELL_OK);
    CHECK(s->value == 0 && s->waiters == 0);
    finish();

    /* A post at the instant of expiry: the timeout wins (the semaphore always did) and the token stays. */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 500 * US, 500 * US, post_sem_hook, NULL);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 500, 0, 0, 0), CELL_ETIMEDOUT);
    CHECK(s->value == 1 && s->waiters == 0);
    finish();
    CHECK_RC(call(sys_semaphore_trywait, 2, g_sem_id, 0, 0, 0, 0), CELL_OK);      /* the token was not lost */

    /* Timeout 0 is infinite. */
    begin();
    push(HW_FAKE_INFINITE, 0, 3600ull * SEC, HW_FAKE_ANY, post_sem_hook, NULL);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 0, 0, 0, 0), CELL_OK);
    CHECK(timed_calls() == 0 && infinite_calls() == 1 && s->value == 0 && s->waiters == 0);
    finish();

    /* A host refusal, then the semaphore still works. */
    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 500 * US, NULL, NULL);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 500, 0, 0, 0), CELL_EFAULT);
    CHECK(s->waiters == 0 && s->value == 0);
    finish();
    begin();
    CHECK_RC(call(sys_semaphore_post, 9, g_sem_id, 1, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_semaphore_wait, 2, g_sem_id, 500, 0, 0, 0), CELL_OK);
    CHECK(timed_calls() == 0);
    finish();

    CHECK_RC(call(sys_semaphore_destroy, 1, g_sem_id, 0, 0, 0, 0), CELL_OK);
}

/* ---- condition variable -------------------------------------------------- */
static uint32_t g_cond_id;
static void signal_cond_hook(void* a) { (void)a; CHECK_RC(call(sys_cond_signal, 9, g_cond_id, 0, 0, 0, 0), CELL_OK); }


/* Does ANOTHER host thread get the guest mutex's host lock? It must not while the guest owns the mutex: the
 * guest-visible owner and depth are bookkeeping, the host lock is what excludes. */
static void* try_lock_probe(void* p)
{
    pthread_mutex_t* m = (pthread_mutex_t*)p;
    if (pthread_mutex_trylock(m) == 0) { pthread_mutex_unlock(m); return (void*)1; }
    return NULL;
}
static int other_thread_gets_lock(sys_mutex_info* m)
{
    pthread_t t;
    void* got = NULL;
    if (pthread_create(&t, NULL, try_lock_probe, &m->mtx) != 0) return -1;
    pthread_join(t, &got);
    return got != NULL;
}

static void test_cond(void)
{
    /* A recursive guest mutex held twice: the wait must give both levels back and take both again. */
    memset(vm_base + 0x400, 0, 64);
    vm_base[0x400 + 4 + 3] = SYS_SYNC_RECURSIVE;
    CHECK_RC(call(sys_mutex_create, 1, 0x230, 0x400, 0, 0, 0), CELL_OK);
    const uint32_t mid = be_read(0x230);
    CHECK_RC(call(sys_cond_create, 1, 0x234, mid, 0, 0, 0), CELL_OK);
    g_cond_id = be_read(0x234);
    sys_mutex_info* m = &g_sys_mutexes[mid - 1];
    sys_cond_info* c = &g_sys_conds[g_cond_id - 1];
    const uint64_t tid = 5;
    CHECK_RC(call(sys_mutex_lock, tid, mid, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_lock, tid, mid, 0, 0, 0, 0), CELL_OK);
    CHECK(m->lock_count == 2 && m->owner_tid == tid);

    /* Expiry after a false wake: ETIMEDOUT, the mutex back at depth 2 for this thread, no waiter left behind. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 1 * MS, NULL, NULL);
    push(HW_FAKE_TIMED, ETIMEDOUT, 900 * US, 900 * US, NULL, NULL);
    CHECK_RC(call(sys_cond_wait, tid, g_cond_id, 1000, 0, 0, 0), CELL_ETIMEDOUT);
    CHECK(m->lock_count == 2 && m->owner_tid == tid && m->waiters == 0 && c->waiters == NULL);
    CHECK(other_thread_gets_lock(m) == 0);                 /* the host lock is held again, at depth */
    CHECK(hw_fake_now() - BASE == 1 * MS);
    finish();

    /* A signal while it sleeps: success, same bookkeeping. */
    begin();
    push(HW_FAKE_TIMED, 0, 100 * US, 1 * MS, signal_cond_hook, NULL);
    CHECK_RC(call(sys_cond_wait, tid, g_cond_id, 1000, 0, 0, 0), CELL_OK);
    CHECK(m->lock_count == 2 && m->owner_tid == tid && c->waiters == NULL);
    finish();

    /* A signal at the instant of expiry: the waiter was selected, so the wait succeeds (the loop tests the record first). */
    begin();
    push(HW_FAKE_TIMED, ETIMEDOUT, 1 * MS, 1 * MS, signal_cond_hook, NULL);
    CHECK_RC(call(sys_cond_wait, tid, g_cond_id, 1000, 0, 0, 0), CELL_OK);
    CHECK(m->lock_count == 2 && c->waiters == NULL);
    finish();

    /* Timeout 0 is infinite. */
    begin();
    push(HW_FAKE_INFINITE, 0, 3600ull * SEC, HW_FAKE_ANY, signal_cond_hook, NULL);
    CHECK_RC(call(sys_cond_wait, tid, g_cond_id, 0, 0, 0, 0), CELL_OK);
    CHECK(timed_calls() == 0 && infinite_calls() == 1 && m->lock_count == 2);
    finish();

    /* A host refusal is CELL_EFAULT, and the guest mutex is still re-taken at full depth. */
    begin();
    push(HW_FAKE_TIMED, EINVAL, 0, 1 * MS, NULL, NULL);
    CHECK_RC(call(sys_cond_wait, tid, g_cond_id, 1000, 0, 0, 0), CELL_EFAULT);
    CHECK(m->lock_count == 2 && m->owner_tid == tid && m->waiters == 0 && c->waiters == NULL);
    finish();

    /* Two unlocks balance the two locks; a third is not the caller's. */
    CHECK_RC(call(sys_mutex_unlock, tid, mid, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_unlock, tid, mid, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_unlock, tid, mid, 0, 0, 0, 0), CELL_EPERM);
    CHECK_RC(call(sys_cond_destroy, 1, g_cond_id, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_destroy, 1, mid, 0, 0, 0, 0), CELL_OK);
}

/* ---- mutex --------------------------------------------------------------- */
static void test_mutex(void)
{
    CHECK_RC(call(sys_mutex_create, 1, 0x240, 0, 0, 0, 0), CELL_OK);
    const uint32_t mid = be_read(0x240);
    sys_mutex_info* m = &g_sys_mutexes[mid - 1];
    const uint64_t* sleeps;

    /* Held by another guest thread: a 1 ms lock backs off (never beyond 200 us, never past the budget) and
     * expires as ETIMEDOUT with the waiter count balanced and the owner unchanged. */
    begin();
    CHECK_RC(call(sys_mutex_lock, 5, mid, 0, 0, 0, 0), CELL_OK);                   /* the holder; timeout 0: the native lock */
    CHECK(hw_fake_mutex_locks() == 1 && hw_fake_mutex_tries() == 0);
    CHECK_RC(call(sys_mutex_lock, 6, mid, 1000, 0, 0, 0), CELL_ETIMEDOUT);
    CHECK(m->waiters == 0 && m->owner_tid == 5 && m->lock_count == 1);
    const uint64_t elapsed = hw_fake_now() - BASE;
    CHECK(elapsed >= 1 * MS && elapsed <= 1 * MS + 5000);
    const unsigned n = hw_fake_mutex_sleeps(&sleeps);
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) { CHECK(sleeps[i] <= 200 * US); total += sleeps[i]; }
    CHECK(n > 5 && total <= 1 * MS);
    CHECK(hw_fake_failures() == 0);

    /* Released: the next timed lock is the first try, no wait. */
    CHECK_RC(call(sys_mutex_unlock, 5, mid, 0, 0, 0, 0), CELL_OK);
    begin();
    CHECK_RC(call(sys_mutex_lock, 6, mid, 1000, 0, 0, 0), CELL_OK);
    CHECK(hw_fake_mutex_tries() == 1 && hw_fake_mutex_sleeps(&sleeps) == 0 && m->owner_tid == 6 && m->waiters == 0);
    CHECK_RC(call(sys_mutex_unlock, 6, mid, 0, 0, 0, 0), CELL_OK);

    /* A host refusal is CELL_EFAULT: the lock is not taken, the owner is not recorded, the count is balanced. */
    static const int bad[] = { EINVAL };
    begin();
    hw_fake_mutex_script(bad, 1);
    CHECK_RC(call(sys_mutex_lock, 6, mid, 1000, 0, 0, 0), CELL_EFAULT);
    CHECK(m->waiters == 0 && m->owner_tid == 0 && m->lock_count == 0);
    CHECK(hw_fake_failures() == 0);
    begin();                                                                         /* the script is spent: real tries again */
    CHECK_RC(call(sys_mutex_lock, 6, mid, 1000, 0, 0, 0), CELL_OK);                  /* and the mutex is still usable */
    CHECK_RC(call(sys_mutex_unlock, 6, mid, 0, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_destroy, 1, mid, 0, 0, 0, 0), CELL_OK);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 65536);
    if (!vm_base) return 2;
    test_queue();
    test_flag();
    test_semaphore();
    test_cond();
    test_mutex();
    printf("timed lv2 waits on the monotonic budget, %s backend: %s\n",
           hw_fake_backend_is_relative() ? "relative" : "absolute", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
