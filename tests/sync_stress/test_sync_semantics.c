/* lv2 sync primitives: the error returns and timeouts a guest depends on.
 *
 *   mutex      trylock of a non-recursive mutex the caller already holds is
 *              EDEADLK; the attribute's name is read from offset 0x20; a
 *              timeout past 2^40 us means "forever", it must not wrap into a
 *              deadline in the past.
 *   semaphore  destroy while a thread is blocked on it is EBUSY (the waiter was
 *              left spinning on a deleted lock); name at 0x18; huge timeout.
 *   rwlock     destroy while read- or write-held is EBUSY; unlock without a
 *              lock is EPERM; timed rlock/wlock return ETIMEDOUT (the timeout
 *              was ignored and the call blocked for good); concurrent creates
 *              get distinct ids; name at 0x18; huge timeout.
 *   cond       name at 0x10; a wait of a few milliseconds ends close to its
 *              timeout.
 *
 * Win32 host threads, through the shim off Windows, like the rest of this
 * suite. Standalone: links the sync sources and test_shims.c only. */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include "runtime/syscalls/sys_rwlock.h"
#include "runtime/syscalls/sys_semaphore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <timeapi.h>
#else
#  include <time.h>
#endif

uint8_t* vm_base;
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define CHECK_RC(call, want) do { int32_t _rc = (int32_t)(call); \
    if (_rc != (int32_t)(want)) { fprintf(stderr, "FAIL line %d: %s -> 0x%08X, want 0x%08X\n", \
        __LINE__, #call, (unsigned)_rc, (unsigned)(want)); failures++; } } while (0)

static HANDLE s_mutex_acquired, s_rwlock_acquired;
static HANDLE s_sem_waiting, s_sem_after_wake, s_sem_release_wake;
static HANDLE s_rw_poll_entered, s_rw_poll_release;
static volatile int s_hook_sem_id, s_pause_sem_after_wake;
static volatile int s_pause_rw_poll, s_abort_rw_poll;
#ifndef _WIN32
static volatile int s_fake_rw_clock;
static struct timespec s_fake_rw_now;

int ps3_sync_test_clock_gettime(clockid_t clock_id, struct timespec* ts)
{
    if (clock_id == CLOCK_MONOTONIC &&
        __atomic_load_n(&s_fake_rw_clock, __ATOMIC_ACQUIRE)) {
        *ts = s_fake_rw_now;
        return 0;
    }
    return clock_gettime(clock_id, ts);
}
#endif

int ps3_sync_test_hook(const char* point, uint32_t id)
{
    if (strcmp(point, "sem.wait.registered") == 0 &&
        id == (uint32_t)__atomic_load_n(&s_hook_sem_id, __ATOMIC_ACQUIRE)) {
        SetEvent(s_sem_waiting);
    } else if (strcmp(point, "sem.wait.after_wake") == 0 &&
               id == (uint32_t)__atomic_load_n(&s_hook_sem_id, __ATOMIC_ACQUIRE) &&
               __atomic_load_n(&s_pause_sem_after_wake, __ATOMIC_ACQUIRE)) {
        SetEvent(s_sem_after_wake);
        WaitForSingleObject(s_sem_release_wake, 20000);
    } else if (strcmp(point, "rw.poll") == 0 &&
               __atomic_load_n(&s_pause_rw_poll, __ATOMIC_ACQUIRE)) {
        SetEvent(s_rw_poll_entered);
        WaitForSingleObject(s_rw_poll_release, 20000);
        return __atomic_load_n(&s_abort_rw_poll, __ATOMIC_ACQUIRE) ? 2 : 1;
    }
    return 0;
}

typedef int64_t (*sc_fn)(ppu_context*);
static int32_t call(sc_fn f, uint32_t tid, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = tid;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    return (int32_t)f(&ctx);
}
static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void be_write(unsigned off, uint32_t v)
{
    vm_base[off] = (uint8_t)(v >> 24); vm_base[off + 1] = (uint8_t)(v >> 16);
    vm_base[off + 2] = (uint8_t)(v >> 8); vm_base[off + 3] = (uint8_t)v;
}
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

#define A_ID    0x200u    /* id out-value */
#define A_ATTR  0x300u    /* attribute block */

/* A call made on its own host thread: result and how long it took. */
typedef struct bg {
    sc_fn    fn;
    uint32_t tid;
    uint64_t a, b, c, d;
    int32_t  rc;
    double   ms;
    HANDLE   h;
} bg_t;
static DWORD WINAPI bg_main(void* p)
{
    bg_t* t = (bg_t*)p;
    const double t0 = now_ms();
    const int32_t rc = call(t->fn, t->tid, t->a, t->b, t->c, t->d);
    t->ms = now_ms() - t0;
    __atomic_store_n(&t->rc, rc, __ATOMIC_RELEASE);
    return 0;
}
static void bg_start(bg_t* t, sc_fn fn, uint32_t tid, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    t->fn = fn; t->tid = tid; t->a = a; t->b = b; t->c = c; t->d = d; t->ms = 0;
    __atomic_store_n(&t->rc, -1, __ATOMIC_RELEASE);
    t->h = CreateThread(NULL, 0, bg_main, t, 0, NULL);
}
static void bg_join(bg_t* t)
{
    WaitForSingleObject(t->h, 20000);
    CloseHandle(t->h);
}

/* Lock a mutex on this thread, keep it for `ms`, release it (it has to be the
 * same host thread, a CRITICAL_SECTION has an owner). */
static uint32_t s_mid;
static DWORD WINAPI hold_mutex(void* ms)
{
    call(sys_mutex_lock, 2, s_mid, 0, 0, 0);
    SetEvent(s_mutex_acquired);
    Sleep((DWORD)(uintptr_t)ms);
    call(sys_mutex_unlock, 2, s_mid, 0, 0, 0);
    return 0;
}
static uint32_t s_rid_hold;
static DWORD WINAPI hold_wlock(void* ms)
{
    call(sys_rwlock_wlock, 2, s_rid_hold, 0, 0, 0);
    SetEvent(s_rwlock_acquired);
    Sleep((DWORD)(uintptr_t)ms);
    call(sys_rwlock_wunlock, 2, s_rid_hold, 0, 0, 0);
    return 0;
}
static const uint64_t k_huge[] = { 1ull << 63, ~0ull, (1ull << 40) + 1 };


/* ----------------------------------------------------------------- mutex */
static void make_mutex(uint32_t recursive_flag)
{
    memset(vm_base + A_ATTR, 0, 0x30);
    be_write(A_ATTR + 0, SYS_SYNC_FIFO);
    be_write(A_ATTR + 4, recursive_flag);
    memcpy(vm_base + A_ATTR + 0x20, "MTXNAME", 8);
    memcpy(vm_base + A_ATTR + 0x18, "WRONG!!", 8);          /* where the name used to be read */
    CHECK_RC(call(sys_mutex_create, 1, A_ID, A_ATTR, 0, 0), CELL_OK);
    s_mid = be_read(A_ID);
}

static void test_mutex(void)
{
    make_mutex(SYS_SYNC_NOT_RECURSIVE);
    CHECK(memcmp(g_sys_mutexes[s_mid - 1].name, "MTXNAME", 8) == 0);

    CHECK_RC(call(sys_mutex_trylock, 1, s_mid, 0, 0, 0), CELL_OK);
    /* The owner again: EDEADLK, and the hold count is unchanged. */
    CHECK_RC(call(sys_mutex_trylock, 1, s_mid, 0, 0, 0), CELL_EDEADLK);
    CHECK(g_sys_mutexes[s_mid - 1].lock_count == 1);
    CHECK_RC(call(sys_mutex_lock, 1, s_mid, 0, 0, 0), CELL_EDEADLK);
    CHECK_RC(call(sys_mutex_unlock, 1, s_mid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_unlock, 1, s_mid, 0, 0, 0), CELL_EPERM);   /* no longer held */

    /* Held by another thread: EBUSY for a trylock. */
    ResetEvent(s_mutex_acquired);
    HANDLE h = CreateThread(NULL, 0, hold_mutex, (void*)(uintptr_t)150, 0, NULL);
    CHECK(WaitForSingleObject(s_mutex_acquired, 5000) == WAIT_OBJECT_0);
    CHECK_RC(call(sys_mutex_trylock, 1, s_mid, 0, 0, 0), CELL_EBUSY);
    WaitForSingleObject(h, 5000); CloseHandle(h);

    /* A timeout of 2^63 us (or all ones) waits for the holder; it must not wrap
     * into a deadline in the past and time out at once. */
    for (size_t i = 0; i < sizeof k_huge / sizeof k_huge[0]; i++) {
        ResetEvent(s_mutex_acquired);
        h = CreateThread(NULL, 0, hold_mutex, (void*)(uintptr_t)120, 0, NULL);
        CHECK(WaitForSingleObject(s_mutex_acquired, 5000) == WAIT_OBJECT_0);
        const double t0 = now_ms();
        const int32_t rc = call(sys_mutex_lock, 1, s_mid, k_huge[i], 0, 0);
        const double ms = now_ms() - t0;
        CHECK(rc == CELL_OK);
        CHECK(ms > 50.0);                 /* it really waited for the holder */
        if (rc == CELL_OK) call(sys_mutex_unlock, 1, s_mid, 0, 0, 0);
        WaitForSingleObject(h, 5000); CloseHandle(h);
    }
    CHECK_RC(call(sys_mutex_destroy, 1, s_mid, 0, 0, 0), CELL_OK);

    /* A recursive mutex nests under trylock. */
    make_mutex(SYS_SYNC_RECURSIVE);
    CHECK_RC(call(sys_mutex_trylock, 1, s_mid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_trylock, 1, s_mid, 0, 0, 0), CELL_OK);
    CHECK(g_sys_mutexes[s_mid - 1].lock_count == 2);
    CHECK_RC(call(sys_mutex_unlock, 1, s_mid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_unlock, 1, s_mid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_destroy, 1, s_mid, 0, 0, 0), CELL_OK);
}

/* ------------------------------------------------------------- semaphore */
static void test_semaphore(void)
{
    memset(vm_base + A_ATTR, 0, 0x30);
    be_write(A_ATTR + 0, SYS_SYNC_FIFO);
    memcpy(vm_base + A_ATTR + 0x18, "SEMNAME", 8);
    memcpy(vm_base + A_ATTR + 0x08, "WRONG!!", 8);
    CHECK_RC(call(sys_semaphore_create, 1, A_ID, A_ATTR, 0, 10), CELL_OK);
    const uint32_t sid = be_read(A_ID);
    CHECK(memcmp(g_sys_semaphores[sid - 1].name, "SEMNAME", 8) == 0);

    /* A thread blocked in wait: destroy is EBUSY, and the waiter still gets its
     * token from a post. */
    bg_t w;
    __atomic_store_n(&s_hook_sem_id, (int)sid, __ATOMIC_RELEASE);
    ResetEvent(s_sem_waiting);
    bg_start(&w, sys_semaphore_wait, 2, sid, 0, 0, 0);
    CHECK(WaitForSingleObject(s_sem_waiting, 20000) == WAIT_OBJECT_0);
    CHECK_RC(call(sys_semaphore_destroy, 1, sid, 0, 0, 0), CELL_EBUSY);
    CHECK_RC(call(sys_semaphore_post, 1, sid, 1, 0, 0), CELL_OK);
    bg_join(&w);
    CHECK(__atomic_load_n(&w.rc, __ATOMIC_ACQUIRE) == CELL_OK);

    /* Destroy cannot retire the native lock while a posted waiter is still
     * between its wake and return from wait(). */
    CHECK_RC(call(sys_semaphore_create, 1, 0x240, A_ATTR, 0, 10), CELL_OK);
    const uint32_t returning_sid = be_read(0x240);
    __atomic_store_n(&s_hook_sem_id, (int)returning_sid, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pause_sem_after_wake, 1, __ATOMIC_RELEASE);
    ResetEvent(s_sem_waiting);
    ResetEvent(s_sem_after_wake);
    ResetEvent(s_sem_release_wake);
    bg_start(&w, sys_semaphore_wait, 2, returning_sid, 0, 0, 0);
    CHECK(WaitForSingleObject(s_sem_waiting, 20000) == WAIT_OBJECT_0);
    CHECK_RC(call(sys_semaphore_post, 1, returning_sid, 1, 0, 0), CELL_OK);
    CHECK(WaitForSingleObject(s_sem_after_wake, 20000) == WAIT_OBJECT_0);
    CHECK_RC(call(sys_semaphore_destroy, 1, returning_sid, 0, 0, 0), CELL_EBUSY);
    SetEvent(s_sem_release_wake);
    bg_join(&w);
    CHECK(__atomic_load_n(&w.rc, __ATOMIC_ACQUIRE) == CELL_OK);
    __atomic_store_n(&s_pause_sem_after_wake, 0, __ATOMIC_RELEASE);
    CHECK_RC(call(sys_semaphore_destroy, 1, returning_sid, 0, 0, 0), CELL_OK);

    /* A huge timeout waits for the post (it used to wrap to "already late"). */
    __atomic_store_n(&s_hook_sem_id, (int)sid, __ATOMIC_RELEASE);
    for (size_t i = 0; i < sizeof k_huge / sizeof k_huge[0]; i++) {
        ResetEvent(s_sem_waiting);
        bg_start(&w, sys_semaphore_wait, 2, sid, k_huge[i], 0, 0);
        CHECK(WaitForSingleObject(s_sem_waiting, 20000) == WAIT_OBJECT_0);
        CHECK(__atomic_load_n(&w.rc, __ATOMIC_ACQUIRE) == -1);  /* still waiting */
        CHECK_RC(call(sys_semaphore_post, 1, sid, 1, 0, 0), CELL_OK);
        bg_join(&w);
        CHECK(__atomic_load_n(&w.rc, __ATOMIC_ACQUIRE) == CELL_OK);
    }

    /* Idle: destroyed. */
    CHECK_RC(call(sys_semaphore_destroy, 1, sid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_semaphore_destroy, 1, sid, 0, 0, 0), CELL_ESRCH);
}

/* ---------------------------------------------------------------- rwlock */
/* A timed read lock taken and released on one thread. */
static int32_t s_timed_rc;
static uint32_t s_rid;
static DWORD WINAPI timed_read_then_unlock(void* timeout_us)
{
    const int32_t rc = call(sys_rwlock_rlock, 2, s_rid,
                            (uint64_t)(uintptr_t)timeout_us, 0, 0);
    __atomic_store_n(&s_timed_rc, rc, __ATOMIC_RELEASE);
    if (rc == CELL_OK) call(sys_rwlock_runlock, 2, s_rid, 0, 0, 0);
    return 0;
}

static DWORD WINAPI create_many(void* p)
{
    const unsigned t = (unsigned)(uintptr_t)p;
    for (unsigned i = 0; i < 8; i++)
        call(sys_rwlock_create, 3, 0x1000 + t * 0x40 + i * 4, 0, 0, 0);
    return 0;
}

static void test_rwlock(void)
{
    memset(vm_base + A_ATTR, 0, 0x30);
    be_write(A_ATTR + 0, SYS_SYNC_FIFO);
    memcpy(vm_base + A_ATTR + 0x18, "RWNAME", 7);
    memcpy(vm_base + A_ATTR + 0x08, "WRONG!!", 8);
    CHECK_RC(call(sys_rwlock_create, 1, A_ID, A_ATTR, 0, 0), CELL_OK);
    const uint32_t rid = be_read(A_ID);
    CHECK(memcmp(g_sys_rwlocks[rid - 1].name, "RWNAME", 7) == 0);

    /* Held by readers or the writer: destroy is EBUSY. */
    CHECK_RC(call(sys_rwlock_rlock, 1, rid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_rwlock_destroy, 1, rid, 0, 0, 0), CELL_EBUSY);
    CHECK_RC(call(sys_rwlock_runlock, 1, rid, 0, 0, 0), CELL_OK);
    /* ...and an unlock with nothing held is EPERM, not a release of a lock
     * nobody owns. */
    CHECK_RC(call(sys_rwlock_runlock, 1, rid, 0, 0, 0), CELL_EPERM);
    CHECK_RC(call(sys_rwlock_wlock, 1, rid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_rwlock_destroy, 1, rid, 0, 0, 0), CELL_EBUSY);
    CHECK_RC(call(sys_rwlock_wunlock, 1, rid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_rwlock_wunlock, 1, rid, 0, 0, 0), CELL_EPERM);

    /* Timed acquires time out instead of blocking for good. The writer is this
     * thread; the contenders run on another. */
    CHECK_RC(call(sys_rwlock_wlock, 1, rid, 0, 0, 0), CELL_OK);
    bg_t t;
    bg_start(&t, sys_rwlock_wlock, 2, rid, 50000, 0, 0);
    bg_join(&t);
    CHECK(__atomic_load_n(&t.rc, __ATOMIC_ACQUIRE) == (int32_t)CELL_ETIMEDOUT);
    CHECK(t.ms >= 40.0 && t.ms < 3000.0);
    bg_start(&t, sys_rwlock_rlock, 2, rid, 50000, 0, 0);
    bg_join(&t);
    CHECK(__atomic_load_n(&t.rc, __ATOMIC_ACQUIRE) == (int32_t)CELL_ETIMEDOUT);
    CHECK(t.ms >= 40.0 && t.ms < 3000.0);
    CHECK_RC(call(sys_rwlock_wunlock, 1, rid, 0, 0, 0), CELL_OK);

    /* A reader holding it keeps a timed writer out, and a timed reader in. */
    CHECK_RC(call(sys_rwlock_rlock, 1, rid, 0, 0, 0), CELL_OK);
    bg_start(&t, sys_rwlock_wlock, 2, rid, 50000, 0, 0);
    bg_join(&t);
    CHECK(__atomic_load_n(&t.rc, __ATOMIC_ACQUIRE) == (int32_t)CELL_ETIMEDOUT);
    s_rid = rid;
    __atomic_store_n(&s_timed_rc, -1, __ATOMIC_RELEASE);
    HANDLE th1 = CreateThread(NULL, 0, timed_read_then_unlock, (void*)(uintptr_t)50000, 0, NULL);
    WaitForSingleObject(th1, 5000); CloseHandle(th1);
    CHECK(__atomic_load_n(&s_timed_rc, __ATOMIC_ACQUIRE) == CELL_OK);  /* readers share */
    CHECK_RC(call(sys_rwlock_runlock, 1, rid, 0, 0, 0), CELL_OK);
    /* A huge timeout waits for the writer to finish. */
    s_rid_hold = rid;
    for (size_t i = 0; i < sizeof k_huge / sizeof k_huge[0]; i++) {
        ResetEvent(s_rwlock_acquired);
        HANDLE h = CreateThread(NULL, 0, hold_wlock, (void*)(uintptr_t)120, 0, NULL);
        CHECK(WaitForSingleObject(s_rwlock_acquired, 5000) == WAIT_OBJECT_0);
        const double t0 = now_ms();
        const int32_t rc = call(sys_rwlock_rlock, 1, rid, k_huge[i], 0, 0);
        const double ms = now_ms() - t0;
        CHECK(rc == CELL_OK);
        CHECK(ms > 50.0);
        if (rc == CELL_OK) call(sys_rwlock_runlock, 1, rid, 0, 0, 0);
        WaitForSingleObject(h, 5000); CloseHandle(h);
    }

#ifndef _WIN32
    /* A lock released one nanosecond past the deadline must not win the next
     * poll. The fake monotonic clock and poll hook order this without sleeping. */
    CHECK_RC(call(sys_rwlock_wlock, 1, rid, 0, 0, 0), CELL_OK);
    s_rid = rid;
    __atomic_store_n(&s_timed_rc, -1, __ATOMIC_RELEASE);
    s_fake_rw_now.tv_sec = 100;
    s_fake_rw_now.tv_nsec = 0;
    __atomic_store_n(&s_fake_rw_clock, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pause_rw_poll, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_abort_rw_poll, 0, __ATOMIC_RELEASE);
    ResetEvent(s_rw_poll_entered);
    ResetEvent(s_rw_poll_release);
    th1 = CreateThread(NULL, 0, timed_read_then_unlock, (void*)(uintptr_t)100, 0, NULL);
    CHECK(WaitForSingleObject(s_rw_poll_entered, 20000) == WAIT_OBJECT_0);
    s_fake_rw_now.tv_nsec = 100001;
    CHECK_RC(call(sys_rwlock_wunlock, 1, rid, 0, 0, 0), CELL_OK);
    SetEvent(s_rw_poll_release);
    CHECK(WaitForSingleObject(th1, 5000) == WAIT_OBJECT_0);
    CloseHandle(th1);
    CHECK(__atomic_load_n(&s_timed_rc, __ATOMIC_ACQUIRE) == (int32_t)CELL_ETIMEDOUT);
    __atomic_store_n(&s_pause_rw_poll, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_fake_rw_clock, 0, __ATOMIC_RELEASE);
#endif

    /* Destroy must not retire an unlocked rwlock while a timed acquire is
     * between polls. */
    CHECK_RC(call(sys_rwlock_wlock, 1, rid, 0, 0, 0), CELL_OK);
    s_rid = rid;
    __atomic_store_n(&s_timed_rc, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pause_rw_poll, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_abort_rw_poll, 0, __ATOMIC_RELEASE);
    ResetEvent(s_rw_poll_entered);
    ResetEvent(s_rw_poll_release);
    th1 = CreateThread(NULL, 0, timed_read_then_unlock, (void*)(uintptr_t)500000, 0, NULL);
    CHECK(WaitForSingleObject(s_rw_poll_entered, 20000) == WAIT_OBJECT_0);
    CHECK_RC(call(sys_rwlock_wunlock, 1, rid, 0, 0, 0), CELL_OK);
    const int32_t destroy_rc = call(sys_rwlock_destroy, 1, rid, 0, 0, 0);
    CHECK(destroy_rc == (int32_t)CELL_EBUSY);
    if (destroy_rc != (int32_t)CELL_EBUSY)
        __atomic_store_n(&s_abort_rw_poll, 1, __ATOMIC_RELEASE);
    SetEvent(s_rw_poll_release);
    CHECK(WaitForSingleObject(th1, 5000) == WAIT_OBJECT_0);
    CloseHandle(th1);
    __atomic_store_n(&s_pause_rw_poll, 0, __ATOMIC_RELEASE);
    if (destroy_rc != (int32_t)CELL_EBUSY) return;  /* keep the mutant away from a destroyed pthread lock */
    CHECK(__atomic_load_n(&s_timed_rc, __ATOMIC_ACQUIRE) == CELL_OK);
    CHECK_RC(call(sys_rwlock_destroy, 1, rid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_rwlock_destroy, 1, rid, 0, 0, 0), CELL_ESRCH);

    /* Concurrent creates: every lock gets a slot of its own. */
    enum { NT = 6, PER = 8 };
    HANDLE th[NT];
    memset(vm_base + 0x1000, 0, NT * 0x40);
    for (unsigned i = 0; i < NT; i++)
        th[i] = CreateThread(NULL, 0, create_many, (void*)(uintptr_t)i, 0, NULL);
    for (unsigned i = 0; i < NT; i++) { WaitForSingleObject(th[i], 20000); CloseHandle(th[i]); }
    uint8_t seen[SYS_RWLOCK_MAX + 1];
    memset(seen, 0, sizeof seen);
    for (unsigned i = 0; i < NT; i++) {
        for (unsigned j = 0; j < PER; j++) {
            const uint32_t id = be_read(0x1000 + i * 0x40 + j * 4);
            CHECK(id >= 1 && id <= SYS_RWLOCK_MAX);
            if (id >= 1 && id <= SYS_RWLOCK_MAX) {
                CHECK(!seen[id]);              /* never two locks in one slot */
                seen[id] = 1;
                call(sys_rwlock_destroy, 1, id, 0, 0, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ cond */
static void test_cond(void)
{
    make_mutex(SYS_SYNC_NOT_RECURSIVE);
    memset(vm_base + A_ATTR, 0, 0x30);
    memcpy(vm_base + A_ATTR + 0x10, "CONDNAME", 8);
    memcpy(vm_base + A_ATTR + 0x08, "WRONG!!!", 8);
    CHECK_RC(call(sys_cond_create, 1, 0x210, s_mid, A_ATTR, 0), CELL_OK);
    const uint32_t cid = be_read(0x210);
    CHECK(memcmp(g_sys_conds[cid - 1].name, "CONDNAME", 8) == 0);

    /* A 2 ms wait ends near 2 ms. With the system timer at 1 ms (as the runtime
     * sets it) a measurement taken from GetTickCount64 came back in whole
     * ~15 ms ticks. The median of several waits, with a loose bound for a busy
     * machine. */
    double ms[9];
    CHECK_RC(call(sys_mutex_lock, 1, s_mid, 0, 0, 0), CELL_OK);
    for (int i = 0; i < 9; i++) {
        const double t0 = now_ms();
        const int32_t rc = call(sys_cond_wait, 1, cid, 2000, 0, 0);
        ms[i] = now_ms() - t0;
        CHECK(rc == (int32_t)CELL_ETIMEDOUT);
        CHECK(g_sys_mutexes[s_mid - 1].lock_count == 1);       /* re-acquired */
    }
    CHECK_RC(call(sys_mutex_unlock, 1, s_mid, 0, 0, 0), CELL_OK);
    for (int i = 1; i < 9; i++)                                  /* insertion sort */
        for (int j = i; j > 0 && ms[j] < ms[j - 1]; j--) { double x = ms[j]; ms[j] = ms[j - 1]; ms[j - 1] = x; }
    printf("cond: 2000 us timed wait, median %.2f ms (min %.2f, max %.2f)\n", ms[4], ms[0], ms[8]);
    CHECK(ms[0] >= 1.5);                 /* never before the timeout */
#ifdef __APPLE__
    CHECK(ms[4] < 40.0);   /* macOS CI runners coalesce timers: ~17 ms medians (see #595) */
#else
    CHECK(ms[4] < 10.0);
#endif

    CHECK_RC(call(sys_cond_destroy, 1, cid, 0, 0, 0), CELL_OK);
    CHECK_RC(call(sys_mutex_destroy, 1, s_mid, 0, 0, 0), CELL_OK);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 65536);
    if (!vm_base) return 2;
    s_mutex_acquired = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_rwlock_acquired = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_sem_waiting = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_sem_after_wake = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_sem_release_wake = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_rw_poll_entered = CreateEventA(NULL, TRUE, FALSE, NULL);
    s_rw_poll_release = CreateEventA(NULL, TRUE, FALSE, NULL);
#ifdef _WIN32
    timeBeginPeriod(1);      /* the runtime raises the timer resolution the same way */
#endif
    test_mutex();
    test_semaphore();
    test_rwlock();
    test_cond();
    CloseHandle(s_mutex_acquired); CloseHandle(s_rwlock_acquired);
    CloseHandle(s_sem_waiting); CloseHandle(s_sem_after_wake);
    CloseHandle(s_sem_release_wake);
    CloseHandle(s_rw_poll_entered); CloseHandle(s_rw_poll_release);
    printf("sync semantics: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
