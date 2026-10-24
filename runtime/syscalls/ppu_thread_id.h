/*
 * ps3recomp - PPU thread ids
 *
 * A guest PPU thread id is handed out once and never again, and ids only go up in creation order (lv2 does the
 * same; RPCS3 too): a guest that keeps the id of a thread that has exited and was joined must never find a later
 * thread under it. The thread table (sys_ppu_thread.c) has PPU_THREAD_MAX slots that are reused, so an id is
 *
 *     id = serial * PPU_THREAD_MAX + slot + 1
 *
 * with `serial` counting creations (main, in slot 0, is serial 0 and id 1). Strictly increasing in creation
 * order (the serial step, 64, outweighs any slot difference, 63), unique, and the slot is recoverable from the id
 * alone, so every id-keyed lookup -- join, priority, rename, the profiler and in-flight diagnostic tables -- is
 * one array access plus a check that the slot still holds that id. No lock, no search.
 *
 * Guest ids stay below PPU_GUEST_TID_END; the host-initiated callback contexts take 0x7F000000..0x7FFEFFFF
 * (ppu_loader.cpp) and 0x7FFFFFFE is sys_ppu_thread_get_id's "no id", so the ranges cannot meet.
 */
#ifndef PPU_THREAD_ID_H
#define PPU_THREAD_ID_H

#include <stdint.h>

#define PPU_THREAD_SLOTS     64u              /* = PPU_THREAD_MAX (sys_ppu_thread.h); a power of two */
#define PPU_GUEST_TID_END    0x7F000000ull

static inline uint64_t ppu_tid_make(uint64_t serial, unsigned slot)
{
    return serial * PPU_THREAD_SLOTS + slot + 1u;
}

/* The table slot an id would live in, or -1 for an id no table thread can have (0, host-callback range). */
static inline int ppu_tid_slot(uint64_t id)
{
    return (id && id < PPU_GUEST_TID_END) ? (int)((id - 1u) & (PPU_THREAD_SLOTS - 1u)) : -1;
}

/* Table slot + 1 (what a thread id used to be): the index of the small per-thread diagnostic arrays. 0 = none. */
static inline unsigned ppu_tid_diag_index(uint64_t id)
{
    const int s = ppu_tid_slot(id);
    return s < 0 ? 0u : (unsigned)s + 1u;
}

#endif
