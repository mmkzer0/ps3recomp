/*
 * ps3recomp - cellSync2 HLE implementation
 *
 * Semaphores use guest big-endian reservation state and cooperative fiber
 * waits. The unrelated legacy primitives still use host OS objects.
 */

#include "cellSync2.h"
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host pointer */
#include "../../runtime/platform/darwin_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "../spurs/cellFiber.h"
#include "../../runtime/memory/vm.h"          /* vm_range_ok: the guest arena rule */
#include "../../runtime/memory/reservation.h"
#include "ps3emu/endian.h"
#include "ps3emu/stop.h"
#include "ps3emu/fw_divergence.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <errno.h>
#include <time.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal layout overlaid on opaque buffers
 * -----------------------------------------------------------------------*/

typedef struct Sync2MutexInternal {
    u32 initialized;
    u32 recursive;
    u32 lockCount;    /* for recursive */
    u32 ownerThread;  /* simplified - not truly thread-safe ID */
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t mtx;
#endif
} Sync2MutexInternal;

typedef struct Sync2CondInternal {
    u32 initialized;
    Sync2MutexInternal* mutex;
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t cv;
#endif
} Sync2CondInternal;


typedef struct Sync2QueueInternal {
    u32 initialized;
    u32 head;
    u32 tail;
    u32 count;
    u32 depth;
    u32 elemSize;
    u8* buffer;
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE notFull;
    CONDITION_VARIABLE notEmpty;
#else
    pthread_mutex_t mtx;
    pthread_cond_t notFull;
    pthread_cond_t notEmpty;
#endif
} Sync2QueueInternal;

/* Compile-time size checks */
_Static_assert(sizeof(Sync2MutexInternal) <= CELL_SYNC2_MUTEX_SIZE,
               "Sync2MutexInternal too large");
_Static_assert(sizeof(Sync2CondInternal) <= CELL_SYNC2_COND_SIZE,
               "Sync2CondInternal too large");
_Static_assert(sizeof(Sync2QueueInternal) <= CELL_SYNC2_QUEUE_SIZE,
               "Sync2QueueInternal too large");

/* =========================================================================
 * Mutex
 * =====================================================================*/

s32 cellSync2MutexAttributeInitialize(CellSync2MutexAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2MutexAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    attr->recursive = 0;
    return CELL_OK;
}

s32 cellSync2MutexInit(CellSync2Mutex* mutex, const CellSync2MutexAttribute* attr)
{
    attr = GUEST_PTR(attr, const CellSync2MutexAttribute*);
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    memset(m, 0, sizeof(Sync2MutexInternal));
    m->recursive = attr ? attr->recursive : 0;
    m->initialized = 1;

#ifdef _WIN32
    InitializeCriticalSection(&m->cs);
#else
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    if (m->recursive)
        pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mtx, &mattr);
    pthread_mutexattr_destroy(&mattr);
#endif

    return CELL_OK;
}

s32 cellSync2MutexLock(CellSync2Mutex* mutex, u64 timeoutUsec)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    if (timeoutUsec == 0) {
        /* Non-blocking: try once */
#ifdef _WIN32
        if (!TryEnterCriticalSection(&m->cs))
            return CELL_SYNC2_ERROR_BUSY;
#else
        if (pthread_mutex_trylock(&m->mtx) != 0)
            return CELL_SYNC2_ERROR_BUSY;
#endif
    } else if (timeoutUsec == (u64)-1) {
        /* Infinite wait */
#ifdef _WIN32
        EnterCriticalSection(&m->cs);
#else
        pthread_mutex_lock(&m->mtx);
#endif
    } else {
        /* Timed wait: spin with yield until timeout */
#ifdef _WIN32
        ULONGLONG start = GetTickCount64();
        ULONGLONG deadline_ms = timeoutUsec / 1000;
        while (!TryEnterCriticalSection(&m->cs)) {
            if (GetTickCount64() - start >= deadline_ms)
                return CELL_SYNC2_ERROR_BUSY;
            SwitchToThread();
        }
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeoutUsec / 1000000);
        ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_mutex_timedlock(&m->mtx, &ts) != 0)
            return CELL_SYNC2_ERROR_BUSY;
#endif
    }

    return CELL_OK;
}

s32 cellSync2MutexTryLock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&m->cs))
        return CELL_SYNC2_ERROR_BUSY;
#else
    if (pthread_mutex_trylock(&m->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;
#endif

    return CELL_OK;
}

s32 cellSync2MutexUnlock(CellSync2Mutex* mutex)
{
    if (!mutex) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2MutexInternal* m = (Sync2MutexInternal*)mutex;
    if (!m->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->mtx);
#endif

    return CELL_OK;
}

/* =========================================================================
 * Condition Variable
 * =====================================================================*/

s32 cellSync2CondAttributeInitialize(CellSync2CondAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2CondAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    attr->maxWaiters = 32;
    return CELL_OK;
}

s32 cellSync2CondInit(CellSync2Cond* cond, CellSync2Mutex* mutex,
                      const CellSync2CondAttribute* attr)
{
    (void)attr;

    if (!cond || !mutex) return CELL_SYNC2_ERROR_NULL_POINTER;

    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    memset(c, 0, sizeof(Sync2CondInternal));
    c->mutex = (Sync2MutexInternal*)mutex;
    c->initialized = 1;

#ifdef _WIN32
    InitializeConditionVariable(&c->cv);
#else
    pthread_cond_init(&c->cv, NULL);
#endif

    return CELL_OK;
}

s32 cellSync2CondWait(CellSync2Cond* cond, u64 timeoutUsec)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    if (!SleepConditionVariableCS(&c->cv, &c->mutex->cs, ms))
        return (GetLastError() == ERROR_TIMEOUT)
               ? CELL_SYNC2_ERROR_TIMEDOUT
               : CELL_SYNC2_ERROR_STAT;
#else
    if (timeoutUsec == 0) {
        pthread_cond_wait(&c->cv, &c->mutex->mtx);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
        ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        int rc = pthread_cond_timedwait(&c->cv, &c->mutex->mtx, &ts);
        if (rc == ETIMEDOUT)
            return CELL_SYNC2_ERROR_TIMEDOUT;
    }
#endif

    return CELL_OK;
}

s32 cellSync2CondSignal(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    WakeConditionVariable(&c->cv);
#else
    pthread_cond_signal(&c->cv);
#endif

    return CELL_OK;
}

s32 cellSync2CondSignalAll(CellSync2Cond* cond)
{
    if (!cond) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2CondInternal* c = (Sync2CondInternal*)cond;
    if (!c->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    WakeAllConditionVariable(&c->cv);
#else
    pthread_cond_broadcast(&c->cv);
#endif

    return CELL_OK;
}

/* =========================================================================
 * Semaphore
 * =====================================================================*/

/* Semaphore ABI/state: libsync2 0200540C..5514, 02005000..533C.
 * Host state contains only suspended-call identities. Counts, reservations,
 * FIFO indices and records live in guest memory. Lock order is sem_lock,
 * fiber registry (notification only), reservation line; never switch a fiber
 * or run a guest callback while sem_lock or a reservation line is held.
 * Nonfiber wake tokens name host parked calls, never host pointers/OS objects.
 */
typedef struct SemWait {
    struct SemWait* next;
    u32 sem;
    u32 fiber;
    u64 token;
    int notified;
} SemWait;
static SemWait* sem_waits;
static u64 sem_next_token = 1;
#ifdef _WIN32
static SRWLOCK sem_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE sem_cv = CONDITION_VARIABLE_INIT;
#define SEM_LOCK() AcquireSRWLockExclusive(&sem_lock)
#define SEM_UNLOCK() ReleaseSRWLockExclusive(&sem_lock)
#define SEM_WAIT() SleepConditionVariableSRW(&sem_cv, &sem_lock, INFINITE, 0)
#define SEM_WAKE() WakeAllConditionVariable(&sem_cv)
#else
static pthread_mutex_t sem_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sem_cv = PTHREAD_COND_INITIALIZER;
#define SEM_LOCK() pthread_mutex_lock(&sem_lock)
#define SEM_UNLOCK() pthread_mutex_unlock(&sem_lock)
#define SEM_WAIT() pthread_cond_wait(&sem_cv, &sem_lock)
#define SEM_WAKE() pthread_cond_broadcast(&sem_cv)
#endif

static void sem_range(const char* api, const char* arg, u32 ea, u64 size)
{
    /* The arena rule lives in vm.h: a zero ppu_vm_size means the whole 32-bit
     * space, so it is not a zero-length limit, and readiness is not
     * "vm_base != NULL" (an identity mapping has vm_base == 0). */
    if (!vm_range_ok(ea, size))
        ps3_stop_guest_range(api, arg, ea, size, vm_guest_limit());
}
static u16 sem_rd16(u32 ea)
{
    u16 v; memcpy(&v, vm_base + ea, 2); return ps3_bswap16(v);
}
static u32 sem_rd32(u32 ea)
{
    u32 v; memcpy(&v, vm_base + ea, 4); return ps3_bswap32(v);
}
static u64 sem_load64(u32 ea)
{
    return ps3_bswap64(atomic_load_explicit((_Atomic u64*)(vm_base + ea), memory_order_acquire));
}
static void sem_store64(u8* p, u64 v)
{
    atomic_store_explicit((_Atomic u64*)p, ps3_bswap64(v), memory_order_release);
}
static PS3_NORETURN void sem_unsupported(const char* api, u32 sem, u32 config, const char* reason)
{
    char s[16], c[16];
    snprintf(s, sizeof s, "0x%X", sem); snprintf(c, sizeof c, "0x%X", config);
    const ps3_stop_field fields[] = {{"api", api}, {"sem", s}, {"config", c}, {"reason", reason}};
    ps3_stop(PS3_STOP_SYNC2_SEMAPHORE_UNSUPPORTED, "unsupported Sync2 semaphore operation",
             "Implement the pinned firmware callback/resource path before using this configuration.",
             fields, sizeof fields / sizeof fields[0], NULL);
}

s32 _cellSync2SemaphoreAttributeInitialize(u32 attr, u32 sdkVersion)
{
    /* fw libsync2 0x02005748 */
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    sem_range(__func__, "attr", attr, 128);
    memset(vm_base + attr, 0, 128);
    vm_write32(attr, sdkVersion);
    vm_write16(attr + 4, 0x010F);
    vm_write16(attr + 6, 1);
    memcpy(vm_base + attr + 8, "CellSync2Semaphore", sizeof "CellSync2Semaphore");
    return CELL_OK;
}

s32 cellSync2SemaphoreInitialize(u32 sem, u32 workBuffer, s32 initialValue, u32 attr)
{
    /* fw libsync2 0x020055D4, 0x0200564C */
    if (!sem || !attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    /* fw libsync2 0x02005654, 0x020056A8 */
    if ((sem & 127) || (workBuffer & 7)) return CELL_SYNC2_ERROR_ALIGN;
    sem_range(__func__, "attr", attr, 128);
    const u32 sdk = sem_rd32(attr);
    const u16 max = sem_rd16(attr + 6), mask = sem_rd16(attr + 4);
    /* fw libsync2 0x02004B54, 0x02004B78 */
    if (sdk - 0x270000u > 0xC0000u || max == 0 || max > 0x8000) return CELL_SYNC2_ERROR_INVAL;
    /* fw libsync2 0x02005680, 0x0200572C */
    if (max == 1 && workBuffer) return CELL_SYNC2_ERROR_INVAL;
    /* fw libsync2 0x02005688, 0x020056C4 */
    if (max > 1 && !workBuffer) return CELL_SYNC2_ERROR_NULL_POINTER;
    /* fw libsync2 0x02005694, 0x020056D4 */
    if (!mask || (mask & 0x3E30)) return CELL_SYNC2_ERROR_INVAL;
    sem_range(__func__, "sem", sem, 128);
    SEM_LOCK();
    for (SemWait* w = sem_waits; w; w = w->next)
        if (w->sem == sem) sem_unsupported(__func__, sem, 0, "reinitialize-blocked");
    memset(vm_base + sem, 0, 128);
    /* Both late failures happen after the firmware zeroes the object. The
     * queue helper rejects byte sizes above 0x40000 (02005F14..5F30). */
    if (initialValue < -0x800000 || initialValue > 0x7FFFFF || max == 0x8000) {
        SEM_UNLOCK();
        return CELL_SYNC2_ERROR_INVAL;
    }
    const u32 buffer = workBuffer ? workBuffer : sem + 0x48;
    const u32 bytes = 8u * (max + 1u);
    sem_range(__func__, "workBuffer", buffer, bytes);
    memset(vm_base + buffer, 0, bytes);
    vm_write64(sem, 0x8000000080000000ull);
    vm_write32(sem + 8, buffer);
    vm_write16(sem + 0xC, max + 1u);
    vm_write64(sem + 0x10, (u64)(u32)initialValue << 32);
    vm_write32(sem + 0x18, sdk);
    vm_write16(sem + 0x20, mask);
    vm_write16(sem + 0x22, max);
    memcpy(vm_base + sem + 0x28, vm_base + attr + 8, 31);
    SEM_UNLOCK();
    return CELL_OK;
}

s32 cellSync2SemaphoreGetCount(u32 sem, u32 countOut)
{
    /* fw libsync2 0x02004C24, 0x02004C38 */
    if (!sem || !countOut) return CELL_SYNC2_ERROR_NULL_POINTER;
    /* fw libsync2 0x02004C3C */
    if (sem & 127) return CELL_SYNC2_ERROR_ALIGN;
    sem_range(__func__, "sem", sem, 128);
    sem_range(__func__, "countOut", countOut, 4);
    vm_write32(countOut, (u32)(sem_load64(sem + 0x10) >> 32));
    return CELL_OK;
}

typedef struct SemOp {
    u32 count;
    u16 max;
    int action; /* 0 try, 1 acquire, 2 release, 3 acknowledge */
    s32 rc;
    u32 wakes;
    int blocked;
} SemOp;

static int sem_state_rmw(u8* line, void* user)
{
    SemOp* op = user;
    const u64 old = ps3_bswap64(atomic_load_explicit((_Atomic u64*)(line + 0x10), memory_order_relaxed));
    s32 count = (s32)(old >> 32);
    u32 state = (u32)old;
    if (op->action == 3) {
        if (op->max == 1) state &= 0x3FFFFFFFu;
        else state -= 0x10001u;
    } else if (op->action < 2) {
        if (count >= (s32)op->count) count -= (s32)op->count;
        else if (op->action == 0) { op->rc = CELL_SYNC2_ERROR_AGAIN; return 0; }
        else if (op->max == 1) {
            if (state & 0x40000000u) { op->rc = CELL_SYNC2_ERROR_BUSY; return 0; }
            /* 02005300..5324 reserves the available prefix and records debt,
             * not an OS semaphore's per-unit decrement loop. */
            state = 0x40000000u | (op->count - (u32)count);
            count = 0;
            op->blocked = 1;
        } else {
            if ((state & 0xFFFFu) == op->max) { op->rc = CELL_SYNC2_ERROR_NOMEM; return 0; }
            ++state;
            op->blocked = 1;
        }
    } else {
        const s64 sum = (s64)count + op->count;
        if (sum > 0x7FFFFF) { op->rc = CELL_SYNC2_ERROR_STAT; return 0; }
        count = (s32)sum;
        if (count > 0) {
            if (op->max == 1) {
                if ((state & 0xC0000000u) == 0x40000000u) {
                    const u32 debt = state & 0x3FFFFFFFu;
                    if ((u32)count < debt) { state -= (u32)count; count = 0; }
                    else { count -= (s32)debt; state = 0xC0000000u; op->wakes = 1; }
                }
            } else {
                const u32 waiting = (state & 0xFFFFu) - (state >> 16);
                op->wakes = waiting < (u32)count ? waiting : (u32)count;
                count -= (s32)op->wakes;
                state += op->wakes << 16;
            }
        }
    }
    sem_store64(line + 0x10, ((u64)(u32)count << 32) | state);
    return 1;
}

typedef struct SemWord { u32 ea; u64 value; } SemWord;
static int sem_word_rmw(u8* line, void* user)
{
    const SemWord* w = user;
    sem_store64(line + (w->ea & 127), w->value);
    return 1;
}
static void sem_put_word(u32 ea, u64 value)
{
    SemWord word = {ea, value};
    resv_host_rmw(ea, sem_word_rmw, &word);
}

/* Serial host calls use the firmware queue's stable (no in-flight producer/
 * consumer) representation. The two index words retain their phase bits;
 * each slot retains its generation when consumed (02005C2C..5C58). */
static void sem_queue_push(u32 sem, u16 type, u64 token)
{
    const u64 header = sem_load64(sem);
    const u32 tail = (u32)header, cap = sem_rd16(sem + 0xC);
    if (cap < 2 || (tail & 0xFFFFu) >= cap) sem_unsupported("Acquire", sem, 0, "queue-state");
    const u32 buffer = sem_rd32(sem + 8);
    sem_range("Acquire", "workBuffer", buffer, 8ull * cap);
    const u32 ea = buffer + 8u * (tail & 0xFFFFu);
    const u64 old = sem_load64(ea);
    const u64 generation = (((old >> 52) + 1u) & 0x1FFu) << 52;
    sem_put_word(ea, ((u64)(tail >> 31) << 63) | generation | ((u64)(type == 2) << 48) | token);
    u32 next = (tail & 0x80000000u) | ((tail + 1u) & 0xFFFFu);
    if ((next & 0xFFFFu) == cap) next = (tail ^ 0x80000000u) & 0x80000000u;
    sem_put_word(sem, (header & 0xFFFFFFFF00000000ull) | next);
}

static SemWait* sem_queue_pop(u32 sem)
{
    const u64 header = sem_load64(sem);
    const u32 head = (u32)(header >> 32), cap = sem_rd16(sem + 0xC);
    if (cap < 2 || (head & 0xFFFFu) >= cap) sem_unsupported("Release", sem, 0, "queue-state");
    const u32 buffer = sem_rd32(sem + 8);
    sem_range("Release", "workBuffer", buffer, 8ull * cap);
    const u32 ea = buffer + 8u * (head & 0xFFFFu);
    const u64 record = sem_load64(ea), token = record & 0x0000FFFFFFFFFFFFull;
    const u32 fiber = ((record >> 48) & 15u) == 1 ? (u32)token : 0;
    SemWait* found = NULL;
    for (SemWait* w = sem_waits; w; w = w->next)
        if (w->sem == sem && w->token == token && w->fiber == fiber && !w->notified) { found = w; break; }
    if (!found) sem_unsupported("Release", sem, 0, "waiter-state");
    const u64 phase = (u64)(head >> 31) << 62;
    sem_put_word(ea, (record & 0x9FF0000000000000ull) | phase | (1ull << 61));
    u32 next = (head & 0x80000000u) | ((head + 1u) & 0xFFFFu);
    if ((next & 0xFFFFu) == cap) next = (head ^ 0x80000000u) & 0x80000000u;
    sem_put_word(sem, ((u64)next << 32) | (u32)header);
    return found;
}

static s32 sem_front(const char* api, u32 sem, u32 config, u32* fiber)
{
    if (!sem) return CELL_SYNC2_ERROR_NULL_POINTER;
    if (sem & 127) return CELL_SYNC2_ERROR_ALIGN;
    if (config) sem_unsupported(api, sem, config, "custom-config");
    sem_range(api, "sem", sem, 128);
    const u16 mask = sem_rd16(sem + 0x20);
    *fiber = (mask & 2) ? (u32)(uintptr_t)cellFiberPpuSelf() : 0;
    if (!(mask & (*fiber ? 2 : 1))) return CELL_SYNC2_ERROR_THREAD_TYPE;
    return CELL_OK;
}

static s32 sem_acquire(const char* api, u32 sem, u32 count, u32 config, int blocking)
{
    u32 fiber = 0;
    s32 rc = sem_front(api, sem, config, &fiber);
    if (rc) return rc;
    const u16 max = sem_rd16(sem + 0x22);
    if (count > 0x7FFFFF || (blocking && max != 1 && count > 1)) return CELL_SYNC2_ERROR_INVAL;
    if (!count) return CELL_OK;
    SEM_LOCK();
    SemOp op = {count, max, blocking, CELL_OK, 0, 0};
    resv_host_rmw(sem, sem_state_rmw, &op);
    if (op.rc || !op.blocked) { SEM_UNLOCK(); return op.rc; }
    SemWait wait = {sem_waits, sem, fiber, fiber ? fiber : sem_next_token++, 0};
    sem_waits = &wait;
    sem_queue_push(sem, fiber ? 2 : 1, wait.token);
    if (fiber) {
        /* Always consume the notifier's signal, including release-before-wait.
         * Unrelated signals may return early; only this wait's notification
         * authorizes acknowledging its guest reservation. */
        do {
            SEM_UNLOCK();
            rc = cellFiberPpuWaitSignal();
            SEM_LOCK();
            if (rc) sem_unsupported(api, sem, 0, "fiber-wait");
        } while (!wait.notified);
    } else {
        while (!wait.notified) SEM_WAIT();
    }
    op.action = 3;
    resv_host_rmw(sem, sem_state_rmw, &op);
    for (SemWait** w = &sem_waits; *w; w = &(*w)->next)
        if (*w == &wait) { *w = wait.next; break; }
    SEM_UNLOCK();
    return CELL_OK;
}

s32 cellSync2SemaphoreAcquire(u32 sem, u32 count, u32 threadConfig)
{
    return sem_acquire(__func__, sem, count, threadConfig, 1);
}
s32 cellSync2SemaphoreTryAcquire(u32 sem, u32 count, u32 threadConfig)
{
    return sem_acquire(__func__, sem, count, threadConfig, 0);
}
s32 cellSync2SemaphoreRelease(u32 sem, u32 count, u32 notifierConfig)
{
    u32 fiber = 0;
    s32 rc = sem_front(__func__, sem, notifierConfig, &fiber);
    if (rc) return rc;
    /* FW 4.93 libsync2 Release (0x02004D08..0x02004D60): with no notifier config, thread types the default
     * notifiers (types 1, 2, 4, 8) do not cover fail with 0x80410C13 before the count bound. The HLE
     * accepts them; counted until a title is seen to reach it. */
    if (sem_rd16(sem + 0x20) & 0xF0u) PS3_FW_DIVERGENCE("S25");
    /* fw libsync2 0x02004D6C */
    if (count > 0x7FFFFF) return CELL_SYNC2_ERROR_INVAL;
    if (!count) return CELL_OK;
    SEM_LOCK();
    SemOp op = {count, sem_rd16(sem + 0x22), 2, CELL_OK, 0, 0};
    resv_host_rmw(sem, sem_state_rmw, &op);
    int wake_ppu = 0;
    for (u32 i = 0; i < op.wakes; ++i) {
        SemWait* w = sem_queue_pop(sem);
        w->notified = 1;
        if (w->fiber) {
            if (ps3_fiber_notify(w->fiber) != PS3_FIBER_NOTIFY_OK)
                sem_unsupported(__func__, sem, 0, "fiber-notify");
        } else {
            wake_ppu = 1;
        }
    }
    if (wake_ppu) SEM_WAKE();
    SEM_UNLOCK();
    return op.rc;
}

/* =========================================================================
 * Queue
 * =====================================================================*/

s32 cellSync2QueueAttributeInitialize(CellSync2QueueAttribute* attr)
{
    attr = GUEST_PTR(attr, CellSync2QueueAttribute*);
    if (!attr) return CELL_SYNC2_ERROR_NULL_POINTER;
    /* ponytail: writes the correct 128-byte ABI layout, but like the rest of this
     * module it does NOT translate the guest EA (GUEST_PTR) or byte-swap to
     * big-endian -- cellSync2QueueInit ignores attr, so nothing consumes these bytes
     * yet. Add GUEST_PTR + be_t writes here (and module-wide) if a title starts
     * reading the attribute back. */
    memset(attr, 0, sizeof(*attr));
    attr->maxPushWaiters = 32;
    attr->maxPopWaiters  = 32;
    return CELL_OK;
}

s32 cellSync2QueueInit(CellSync2Queue* queue, void* buffer,
                       u32 elemSize, u32 depth,
                       const CellSync2QueueAttribute* attr)
{
    buffer = GUEST_PTR(buffer, void*);
    (void)attr;

    if (!queue || !buffer) return CELL_SYNC2_ERROR_NULL_POINTER;
    if (elemSize == 0 || depth == 0) return CELL_SYNC2_ERROR_INVAL;

    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    memset(q, 0, sizeof(Sync2QueueInternal));
    q->depth = depth;
    q->elemSize = elemSize;
    q->buffer = (u8*)buffer;
    q->initialized = 1;

    memset(buffer, 0, (size_t)elemSize * depth);

#ifdef _WIN32
    InitializeCriticalSection(&q->cs);
    InitializeConditionVariable(&q->notFull);
    InitializeConditionVariable(&q->notEmpty);
#else
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->notFull, NULL);
    pthread_cond_init(&q->notEmpty, NULL);
#endif

    return CELL_OK;
}

s32 cellSync2QueuePush(CellSync2Queue* queue, const void* data,
                       u64 timeoutUsec)
{
    data = GUEST_PTR(data, const void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    while (q->count >= q->depth) {
        if (!SleepConditionVariableCS(&q->notFull, &q->cs, ms)) {
            LeaveCriticalSection(&q->cs);
            return CELL_SYNC2_ERROR_TIMEDOUT;
        }
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    WakeConditionVariable(&q->notEmpty);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->mtx);
    while (q->count >= q->depth) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notFull, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notFull, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueTryPush(CellSync2Queue* queue, const void* data)
{
    data = GUEST_PTR(data, const void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&q->cs))
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count >= q->depth) {
        LeaveCriticalSection(&q->cs);
        return CELL_SYNC2_ERROR_OVERFLOW;
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    WakeConditionVariable(&q->notEmpty);
    LeaveCriticalSection(&q->cs);
#else
    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count >= q->depth) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_OVERFLOW;
    }

    memcpy(q->buffer + (size_t)q->tail * q->elemSize, data, q->elemSize);
    q->tail = (q->tail + 1) % q->depth;
    q->count++;

    pthread_cond_signal(&q->notEmpty);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueuePop(CellSync2Queue* queue, void* data,
                      u64 timeoutUsec)
{
    data = GUEST_PTR(data, void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    EnterCriticalSection(&q->cs);
    DWORD ms = (timeoutUsec == 0) ? INFINITE : (DWORD)(timeoutUsec / 1000);
    while (q->count == 0) {
        if (!SleepConditionVariableCS(&q->notEmpty, &q->cs, ms)) {
            LeaveCriticalSection(&q->cs);
            return CELL_SYNC2_ERROR_TIMEDOUT;
        }
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    WakeConditionVariable(&q->notFull);
    LeaveCriticalSection(&q->cs);
#else
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        if (timeoutUsec == 0) {
            pthread_cond_wait(&q->notEmpty, &q->mtx);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec  += (time_t)(timeoutUsec / 1000000);
            ts.tv_nsec += (long)((timeoutUsec % 1000000) * 1000);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&q->notEmpty, &q->mtx, &ts) == ETIMEDOUT) {
                pthread_mutex_unlock(&q->mtx);
                return CELL_SYNC2_ERROR_TIMEDOUT;
            }
        }
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueTryPop(CellSync2Queue* queue, void* data)
{
    data = GUEST_PTR(data, void*);
    if (!queue || !data) return CELL_SYNC2_ERROR_NULL_POINTER;
    Sync2QueueInternal* q = (Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

#ifdef _WIN32
    if (!TryEnterCriticalSection(&q->cs))
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count == 0) {
        LeaveCriticalSection(&q->cs);
        return CELL_SYNC2_ERROR_EMPTY;
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    WakeConditionVariable(&q->notFull);
    LeaveCriticalSection(&q->cs);
#else
    if (pthread_mutex_trylock(&q->mtx) != 0)
        return CELL_SYNC2_ERROR_BUSY;

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return CELL_SYNC2_ERROR_EMPTY;
    }

    memcpy(data, q->buffer + (size_t)q->head * q->elemSize, q->elemSize);
    q->head = (q->head + 1) % q->depth;
    q->count--;

    pthread_cond_signal(&q->notFull);
    pthread_mutex_unlock(&q->mtx);
#endif

    return CELL_OK;
}

s32 cellSync2QueueSize(const CellSync2Queue* queue, u32* size)
{
    if (!queue || !size) return CELL_SYNC2_ERROR_NULL_POINTER;
    const Sync2QueueInternal* q = (const Sync2QueueInternal*)queue;
    if (!q->initialized) return CELL_SYNC2_ERROR_NOT_INITIALIZED;

    vm_write32((u32)(uintptr_t)size, q->count);
    return CELL_OK;
}
