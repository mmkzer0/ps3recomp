/*
 * ps3recomp - cellSync2 HLE
 *
 * Extended synchronization primitives with timeout and priority support.
 * Implemented using host OS primitives (Windows SRWLOCK/CONDITION_VARIABLE,
 * pthreads mutex/cond/sem on Unix).
 */

#ifndef PS3RECOMP_CELL_SYNC2_H
#define PS3RECOMP_CELL_SYNC2_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Firmware error family (legacy non-semaphore names retained).
 * -----------------------------------------------------------------------*/
#define CELL_SYNC2_ERROR_AGAIN                  ((s32)0x80410C01)
#define CELL_SYNC2_ERROR_INVAL                  ((s32)0x80410C02)
#define CELL_SYNC2_ERROR_NOMEM                  ((s32)0x80410C04)
#define CELL_SYNC2_ERROR_BUSY                   ((s32)0x80410C0A)
#define CELL_SYNC2_ERROR_STAT                   ((s32)0x80410C0F)
#define CELL_SYNC2_ERROR_ALIGN                  ((s32)0x80410C10)
#define CELL_SYNC2_ERROR_NULL_POINTER           ((s32)0x80410C11)
#define CELL_SYNC2_ERROR_THREAD_TYPE            ((s32)0x80410C12)
/* Unrelated legacy timeout/queue APIs are not firmware-corrected here. */
#define CELL_SYNC2_ERROR_TIMEDOUT               (CELL_ERROR_BASE_SYNC | 0x52)
#define CELL_SYNC2_ERROR_NOT_INITIALIZED        (CELL_ERROR_BASE_SYNC | 0x53)
#define CELL_SYNC2_ERROR_OVERFLOW               CELL_SYNC2_ERROR_STAT
#define CELL_SYNC2_ERROR_EMPTY                  (CELL_ERROR_BASE_SYNC | 0x55)

/* ---------------------------------------------------------------------------
 * CellSync2Mutex
 * -----------------------------------------------------------------------*/

/* Opaque handle -- internally holds OS mutex */
#define CELL_SYNC2_MUTEX_SIZE   128

typedef struct CellSync2Mutex {
    u8 _opaque[CELL_SYNC2_MUTEX_SIZE];
} CellSync2Mutex;

typedef struct CellSync2MutexAttribute {
    u32 maxWaiters;
    u8  recursive;
    u8  padding[3];
} CellSync2MutexAttribute;

s32 cellSync2MutexAttributeInitialize(CellSync2MutexAttribute* attr);
s32 cellSync2MutexInit(CellSync2Mutex* mutex, const CellSync2MutexAttribute* attr);
s32 cellSync2MutexLock(CellSync2Mutex* mutex, u64 timeoutUsec);
s32 cellSync2MutexTryLock(CellSync2Mutex* mutex);
s32 cellSync2MutexUnlock(CellSync2Mutex* mutex);

/* ---------------------------------------------------------------------------
 * CellSync2Cond -- condition variable
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_COND_SIZE    128

typedef struct CellSync2Cond {
    u8 _opaque[CELL_SYNC2_COND_SIZE];
} CellSync2Cond;

typedef struct CellSync2CondAttribute {
    u32 maxWaiters;
} CellSync2CondAttribute;

s32 cellSync2CondAttributeInitialize(CellSync2CondAttribute* attr);
s32 cellSync2CondInit(CellSync2Cond* cond, CellSync2Mutex* mutex,
                      const CellSync2CondAttribute* attr);
s32 cellSync2CondWait(CellSync2Cond* cond, u64 timeoutUsec);
s32 cellSync2CondSignal(CellSync2Cond* cond);
s32 cellSync2CondSignalAll(CellSync2Cond* cond);

/* ---------------------------------------------------------------------------
 * CellSync2Semaphore
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_SEMAPHORE_SIZE   128

typedef struct CellSync2Semaphore {
    u8 _opaque[CELL_SYNC2_SEMAPHORE_SIZE];
} CellSync2Semaphore;

/* Guest big-endian layout. Public semaphore arguments are guest EAs. */
typedef struct CellSync2SemaphoreAttribute {
    u32 sdkVersion;
    u16 threadTypes;
    u16 maxWaiters;
    char name[32];
    u8 reserved[88];
} CellSync2SemaphoreAttribute;

s32 _cellSync2SemaphoreAttributeInitialize(u32 attr, u32 sdkVersion);
s32 cellSync2SemaphoreEstimateBufferSize(u32 attr, u32 bufferSize);
s32 cellSync2SemaphoreInitialize(u32 sem, u32 workBuffer, s32 initialValue, u32 attr);
s32 cellSync2SemaphoreGetCount(u32 sem, u32 countOut);
s32 cellSync2SemaphoreAcquire(u32 sem, u32 count, u32 threadConfig);
s32 cellSync2SemaphoreTryAcquire(u32 sem, u32 count, u32 threadConfig);
s32 cellSync2SemaphoreRelease(u32 sem, u32 count, u32 notifierConfig);

/* ---------------------------------------------------------------------------
 * CellSync2Queue -- bounded queue with blocking and timeout
 * -----------------------------------------------------------------------*/

#define CELL_SYNC2_QUEUE_SIZE   256

typedef struct CellSync2Queue {
    u8 _opaque[CELL_SYNC2_QUEUE_SIZE];
} CellSync2Queue;

/* Real 128-byte ABI, verified against the What-if debug-build DWARF (dwarf_abi.py).
 * The old 8-byte {maxPushWaiters,maxPopWaiters} layout put both fields where the SDK
 * keeps sdkVersion/threadTypes; the waiters are u16 at +0x10/+0x12. */
typedef struct CellSync2QueueAttribute {
    u32  sdkVersion;
    u32  threadTypes;
    u32  elementSize;
    u32  depth;
    u16  maxPushWaiters;
    u16  maxPopWaiters;
    char name[32];
    u8   reserved[76];
} CellSync2QueueAttribute;

s32 cellSync2QueueAttributeInitialize(CellSync2QueueAttribute* attr);
s32 cellSync2QueueInit(CellSync2Queue* queue, void* buffer,
                       u32 elemSize, u32 depth,
                       const CellSync2QueueAttribute* attr);
s32 cellSync2QueuePush(CellSync2Queue* queue, const void* data,
                       u64 timeoutUsec);
s32 cellSync2QueueTryPush(CellSync2Queue* queue, const void* data);
s32 cellSync2QueuePop(CellSync2Queue* queue, void* data,
                      u64 timeoutUsec);
s32 cellSync2QueueTryPop(CellSync2Queue* queue, void* data);
s32 cellSync2QueueSize(const CellSync2Queue* queue, u32* size);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYNC2_H */
