/*
 * ps3recomp - cellSync HLE
 *
 * SPU-safe synchronization primitives: mutex, barrier, reader-writer,
 * bounded queue, and lock-free queue.  Implemented with C11 atomics.
 */

#ifndef PS3RECOMP_CELL_SYNC_H
#define PS3RECOMP_CELL_SYNC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

/* The generated C++ NID table (smoke/ppu_hle_nids.cpp) includes this header but
 * never touches a field: only the C implementation performs the atomic
 * operations. C++ therefore spells the 4-byte fields as plain integers rather
 * than including <atomic>, which would be pulled into every include site and
 * cannot follow <stdatomic.h> (libstdc++'s kill_dependency macro from C++11 to
 * C++20 makes the later <atomic> fail). The layout assert below holds both
 * languages to the same guest-visible fields. */
#ifdef __cplusplus
#define CELL_SYNC_ATOMIC_UINT unsigned
#define CELL_SYNC_ATOMIC_INT  int
#else
#include <stdatomic.h>
#define CELL_SYNC_ATOMIC_UINT atomic_uint
#define CELL_SYNC_ATOMIC_INT  atomic_int
#endif

/* The implementation and the generated C++ table must agree on the field
 * widths: a 4-byte lock-free atomic on the C side, the same 4 bytes on the C++
 * side. */
typedef char cell_sync_atomic_layout[
    (sizeof(CELL_SYNC_ATOMIC_UINT) == 4 && sizeof(CELL_SYNC_ATOMIC_INT) == 4) ? 1 : -1];

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_ERROR_AGAIN                   (CELL_ERROR_BASE_SYNC | 0x01)
#define CELL_SYNC_ERROR_INVAL                   (CELL_ERROR_BASE_SYNC | 0x02)
#define CELL_SYNC_ERROR_NOSYS                   (CELL_ERROR_BASE_SYNC | 0x03)
#define CELL_SYNC_ERROR_NOMEM                   (CELL_ERROR_BASE_SYNC | 0x04)
#define CELL_SYNC_ERROR_SRCH                    (CELL_ERROR_BASE_SYNC | 0x05)
#define CELL_SYNC_ERROR_NOENT                   (CELL_ERROR_BASE_SYNC | 0x06)
#define CELL_SYNC_ERROR_BUSY                    (CELL_ERROR_BASE_SYNC | 0x0A)
#define CELL_SYNC_ERROR_STAT                    (CELL_ERROR_BASE_SYNC | 0x0F)
#define CELL_SYNC_ERROR_ALIGN                   (CELL_ERROR_BASE_SYNC | 0x10)
#define CELL_SYNC_ERROR_NULL_POINTER            (CELL_ERROR_BASE_SYNC | 0x11)
#define CELL_SYNC_ERROR_PERM                    (CELL_ERROR_BASE_SYNC | 0x09)
#define CELL_SYNC_ERROR_OVERFLOW                (CELL_ERROR_BASE_SYNC | 0x12)
#define CELL_SYNC_ERROR_EMPTY                   (CELL_ERROR_BASE_SYNC | 0x13)

/* ---------------------------------------------------------------------------
 * CellSyncMutex -- 32-bit spinlock
 * -----------------------------------------------------------------------*/
typedef struct CellSyncMutex {
    CELL_SYNC_ATOMIC_UINT lock;
} CellSyncMutex;

s32 cellSyncMutexInitialize(CellSyncMutex* mutex);
s32 cellSyncMutexLock(CellSyncMutex* mutex);
s32 cellSyncMutexTryLock(CellSyncMutex* mutex);
s32 cellSyncMutexUnlock(CellSyncMutex* mutex);

/* ---------------------------------------------------------------------------
 * CellSyncBarrier -- counter-based barrier
 * -----------------------------------------------------------------------*/
typedef struct CellSyncBarrier {
    CELL_SYNC_ATOMIC_UINT arrived;
    u32         total;
    CELL_SYNC_ATOMIC_UINT phase;
} CellSyncBarrier;

s32 cellSyncBarrierInitialize(CellSyncBarrier* barrier, u16 totalCount);
s32 cellSyncBarrierNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryNotify(CellSyncBarrier* barrier);
s32 cellSyncBarrierWait(CellSyncBarrier* barrier);
s32 cellSyncBarrierTryWait(CellSyncBarrier* barrier);

/* ---------------------------------------------------------------------------
 * CellSyncRwm -- reader-writer memory
 * -----------------------------------------------------------------------*/
typedef struct CellSyncRwm {
    CELL_SYNC_ATOMIC_INT  readers;
    CELL_SYNC_ATOMIC_UINT writer;
    u32         size;
    void*       buffer;
} CellSyncRwm;

s32 cellSyncRwmInitialize(CellSyncRwm* rwm, void* buffer, u32 size);
s32 cellSyncRwmRead(CellSyncRwm* rwm, void* dst);
s32 cellSyncRwmTryRead(CellSyncRwm* rwm, void* dst);
s32 cellSyncRwmWrite(CellSyncRwm* rwm, const void* src);
s32 cellSyncRwmTryWrite(CellSyncRwm* rwm, const void* src);

/* ---------------------------------------------------------------------------
 * CellSyncQueue -- bounded queue
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_QUEUE_MAX_DEPTH   1024

/* Opaque: guest big-endian layout, accessed by EA -- see cellSync.c. */
typedef struct CellSyncQueue {
    u8 raw[32];
} CellSyncQueue;

s32 cellSyncQueueInitialize(CellSyncQueue* queue, void* buffer,
                            u32 size, u32 depth);
s32 cellSyncQueuePush(CellSyncQueue* queue, const void* data);
s32 cellSyncQueueTryPush(CellSyncQueue* queue, const void* data);
s32 cellSyncQueuePop(CellSyncQueue* queue, void* data);
s32 cellSyncQueueTryPop(CellSyncQueue* queue, void* data);
s32 cellSyncQueuePeek(CellSyncQueue* queue, void* data);
s32 cellSyncQueueSize(CellSyncQueue* queue, u32* size);
s32 cellSyncQueueClear(CellSyncQueue* queue);

/* ---------------------------------------------------------------------------
 * CellSyncLFQueue -- lock-free queue
 * -----------------------------------------------------------------------*/
#define CELL_SYNC_LFQUEUE_DIR_PUSH  0
#define CELL_SYNC_LFQUEUE_DIR_POP   1

typedef struct CellSyncLFQueue {
    CELL_SYNC_ATOMIC_UINT head;
    CELL_SYNC_ATOMIC_UINT tail;
    CELL_SYNC_ATOMIC_UINT count;
    u32         depth;
    u32         elemSize;
    u8*         buffer;
    u32         direction;
    CELL_SYNC_ATOMIC_UINT lock;
} CellSyncLFQueue;

s32 cellSyncLFQueueInitialize(CellSyncLFQueue* queue, void* buffer,
                               u32 size, u32 depth, u32 direction,
                               void* eaSignal);
s32 cellSyncLFQueuePush(CellSyncLFQueue* queue, const void* data);
s32 cellSyncLFQueueTryPush(CellSyncLFQueue* queue, const void* data);
s32 cellSyncLFQueuePop(CellSyncLFQueue* queue, void* data);
s32 cellSyncLFQueueTryPop(CellSyncLFQueue* queue, void* data);
s32 cellSyncLFQueueGetDirection(const CellSyncLFQueue* queue, u32* dir);
s32 cellSyncLFQueueDepth(const CellSyncLFQueue* queue, u32* depth);
s32 cellSyncLFQueueSize(CellSyncLFQueue* queue, u32* size);
s32 cellSyncLFQueueClear(CellSyncLFQueue* queue);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SYNC_H */
