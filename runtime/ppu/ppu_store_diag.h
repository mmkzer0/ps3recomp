/*
 * ps3recomp - which guest-store write watches are armed
 *
 * The guest store family (vm_write8/16/32/64, ppu_loader.cpp) runs the write watches (PPU_WVAL, PPU_WWATCH, and
 * on Windows the PT detector) before a store. They are debugging aids, off in every real run, so the family
 * takes a short path that skips them while this word is zero and the sync-line probe (g_barrier_sync_watch) is
 * unset. The word gates work and publishes nothing, so it is plain storage read with relaxed loads: a watch
 * that arms itself sets its bit before the first store that has to see it, and a store racing the arm costs
 * at most that store. The store window's hook (ppu_set_store_hook) is NOT one of these bits: the RSX label
 * window is registered by every title that drives GCM, and the hook is part of the machine's ordering, so it
 * runs on both paths.
 */
#ifndef PPU_STORE_DIAG_H
#define PPU_STORE_DIAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t ppu_store_diag;

#define PPU_STORE_DIAG_UNREAD 0x1u   /* the watch environment has not been read yet (the first guest store reads it) */
#define PPU_STORE_DIAG_WATCH  0x2u   /* PPU_WVAL or PPU_WWATCH is set */
#define PPU_STORE_DIAG_PT     0x4u   /* PT is set (the Windows truncation detector) */

static inline uint32_t ppu_store_diag_load(void)
{
    return __atomic_load_n(&ppu_store_diag, __ATOMIC_RELAXED);
}
static inline void ppu_store_diag_arm(uint32_t bits)
{
    __atomic_fetch_or(&ppu_store_diag, bits, __ATOMIC_RELEASE);
}
static inline void ppu_store_diag_disarm(uint32_t bits)
{
    __atomic_fetch_and(&ppu_store_diag, ~bits, __ATOMIC_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* PPU_STORE_DIAG_H */
