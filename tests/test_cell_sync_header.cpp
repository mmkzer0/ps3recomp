/* The in-tree C++ consumer of libs/sync/cellSync.h.
 *
 * The generated NID table (smoke/ppu_hle_nids.cpp) includes this header from
 * C++, and nothing else in the tree did, so a change that broke the C++ spelling
 * of the structures went unnoticed until a port regenerated its table. This TU
 * exists to compile: it also includes <stdatomic.h> first, the order that used
 * to fail inside <atomic>, and it pins what the C and C++ views must share.
 *
 * It links nothing: the C implementation is what performs the atomic operations.
 */
#include <stdatomic.h>   /* deliberately before cellSync.h */
#include "cellSync.h"

#include <cstddef>
#include <type_traits>

/* The guest-visible size: the C implementation's C11 atomic and the C++ view's
 * plain integer are both one 32-bit word (the header asserts the same pair for
 * both languages). */
static_assert(sizeof(CellSyncMutex) == 4, "CellSyncMutex is one 32-bit word");
static_assert(offsetof(CellSyncMutex, lock) == 0, "the lock is the whole word");
static_assert(alignof(CellSyncMutex) <= 4, "no wider alignment than the guest word");
static_assert(sizeof(CellSyncBarrier) == 12, "arrived, total, phase");
static_assert(sizeof(CellSyncLFQueue::head) == 4 && sizeof(CellSyncLFQueue::lock) == 4,
              "the lock-free queue's two atomics are 4 bytes");
static_assert(sizeof(CellSyncRwm::readers) == 4 && sizeof(CellSyncRwm::writer) == 4,
              "the reader-writer's two atomics are 4 bytes");
/* Plain data in C++: std::atomic would make these non-trivially-copyable and a
 * C++ caller could no longer pass one by value or in a struct. */
static_assert(std::is_trivially_copyable<CellSyncMutex>::value, "plain data");
static_assert(std::is_trivially_default_constructible<CellSyncBarrier>::value, "plain data");

/* The error numbers the C++ side sees are the C ones (0x80410100 base). */
static_assert(CELL_SYNC_ERROR_INVAL == (s32)0x80410102u, "INVAL");
static_assert(CELL_SYNC_ERROR_ALIGN == (s32)0x80410110u, "ALIGN");

int main()
{
    return sizeof(CellSyncMutex) == 4 ? 0 : 1;
}
