/*
 * ps3recomp - PPU memory access for recompiled code
 *
 * All PS3 memory is big-endian.  These functions translate a 32-bit PS3
 * virtual address to a host pointer via vm_base, then perform a load or
 * store with the appropriate byte swap.
 *
 * Atomic operations (lwarx/stwcx and the SPU's GETLLAR/PUTLLC) are one model:
 * runtime/memory/reservation.h.
 */

#ifndef PPU_MEMORY_H
#define PPU_MEMORY_H

#include "../../include/ps3emu/endian.h"
#include "ppu_context.h"
#include "../spu/spu_coh_bitmap.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Base pointer -- set by the VM manager at startup.
 *
 * Host pointer to the beginning of the PS3 32-bit address space mapping.
 * All guest address translation is:  host_ptr = vm_base + guest_addr
 * -----------------------------------------------------------------------*/
extern uint8_t* vm_base;

/* ---------------------------------------------------------------------------
 * Address translation
 * -----------------------------------------------------------------------*/
/* Translate a guest pointer PARAMETER to a host pointer, preserving NULL.
 *
 * HLE entry points receive pointer arguments as raw 32-bit GUEST addresses
 * (ppu_hle.cpp passes the PPC register through untouched), so dereferencing
 * one directly hits whatever host address happens to share that number. That
 * is the most common bug in libs/ -- cellFsRead read an entire sound bank to
 * an untranslated address this way. Fifteen files had each pasted their own
 * copy of this macro; this is the one definition.
 *
 * NOTE this gives you a host pointer to BIG-ENDIAN memory. It is right for
 * byte buffers and for structs of u8, but a u32/u64/float field still needs
 * the vm_read and vm_write accessors below (or an explicit swap) -- the cast
 * alone does not fix endianness.
 */
#ifndef GUEST_PTR
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#endif

static inline void* vm_translate(uint32_t addr)
{
    return (void*)(vm_base + addr);
}

static inline uint8_t* vm_ptr8(uint32_t addr)  { return (uint8_t*)vm_translate(addr); }
static inline uint16_t* vm_ptr16(uint32_t addr) { return (uint16_t*)vm_translate(addr); }
static inline uint32_t* vm_ptr32(uint32_t addr) { return (uint32_t*)vm_translate(addr); }
static inline uint64_t* vm_ptr64(uint32_t addr) { return (uint64_t*)vm_translate(addr); }

/* ---------------------------------------------------------------------------
 * Loads -- read from big-endian guest memory, return host-endian value.
 * -----------------------------------------------------------------------*/
static inline uint8_t vm_read8(uint32_t addr)
{
    return *vm_ptr8(addr);
}

static inline uint16_t vm_read16(uint32_t addr)
{
    uint16_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap16(raw);
}

static inline uint32_t vm_read32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap32(raw);
}

static inline uint64_t vm_read64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    return ps3_bswap64(raw);
}

static inline float vm_read_f32(uint32_t addr)
{
    uint32_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap32(raw);
    float result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

static inline double vm_read_f64(uint32_t addr)
{
    uint64_t raw;
    memcpy(&raw, vm_ptr8(addr), sizeof(raw));
    raw = ps3_bswap64(raw);
    double result;
    memcpy(&result, &raw, sizeof(result));
    return result;
}

/* ---------------------------------------------------------------------------
 * Stores -- write host-endian value into big-endian guest memory.
 * -----------------------------------------------------------------------*/
/* Plain stores do not touch the reservation model unless the line is one an
 * SPU has reserved (VM_WRITE_COH below); see reservation.h, known limit 1. */
#ifdef __cplusplus
extern "C" {
#endif

/* PPU <-> SPU lock-line coherence (runtime/spu/spu_coherency.c). A PPU store to
 * a 128-byte line an SPU has reserved with GETLLAR has to serialize through the
 * SPU lock-line lock -- otherwise it can land inside a PUTLLC's compare and be
 * written straight back over -- and has to raise SPU_EVENT_LR, which is how an
 * SPURS idle service parked in `rdch SPU_RdEventStat` learns work was posted.
 * Declared here rather than pulling spu_coherency.h into every HLE module.
 *
 * The authority is the reservation model (spu_coh_is_reserved); upstream's
 * inline bitmap (spu_coh_bitmap.h) is not consulted. bitmap: pending block-2
 * proof. */
int         spu_coh_is_reserved(uint32_t addr);
void        spu_lockline_lock(void);
void        spu_lockline_unlock(void);
void        spu_coh_notify_write(uint32_t addr);
#ifdef __cplusplus
}
#endif

#define VM_WRITE_COH(addr, src, n)                                            \
    do {                                                                      \
        if (spu_coh_is_reserved((uint32_t)(addr))) {  /* bitmap: pending block-2 proof */ \
            spu_lockline_lock();                                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
            spu_coh_notify_write((uint32_t)(addr));                           \
            spu_lockline_unlock();                                            \
        } else {                                                              \
            memcpy(vm_ptr8((uint32_t)(addr)), (src), (n));                    \
        }                                                                     \
    } while (0)

/* ---------------------------------------------------------------------------
 * Write-watch hook for the INLINE writers.
 *
 * PPU_WWATCH (ppu_loader.cpp) logs stores to a watched address along with the guest
 * function responsible. Recompiled guest code calls the extern vm_write* family
 * there, so it is covered -- but everything in libs/ uses the inline family
 * below, and those stores were invisible to it. That makes "nothing writes this
 * address" a claim the watch cannot actually support, which is exactly the sort
 * of thing worth not being wrong about: an HLE writing a zero looks identical
 * to nobody writing at all.
 *
 * Cost is two compares against a pair that stay 0 unless PPU_WWATCH is set, on the
 * HLE path only -- guest stores never reach here.
 * -----------------------------------------------------------------------*/
extern uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width);

#define PS3_WW_CHECK(a, v, w)                                                 \
    do {                                                                      \
        if ((a) >= g_ww_lo && (a) < g_ww_hi)                                  \
            ps3_ww_report_inline((uint32_t)(a), (uint64_t)(v), (w));          \
    } while (0)

static inline void vm_write8(uint32_t addr, uint8_t val)
{
    PS3_WW_CHECK(addr, val, 1);
    VM_WRITE_COH(addr, &val, 1);
}

static inline void vm_write16(uint32_t addr, uint16_t val)
{
    PS3_WW_CHECK(addr, val, 2);
    uint16_t raw = ps3_bswap16(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write32(uint32_t addr, uint32_t val)
{
    PS3_WW_CHECK(addr, val, 4);
    uint32_t raw = ps3_bswap32(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write64(uint32_t addr, uint64_t val)
{
    PS3_WW_CHECK(addr, val, 8);
    uint64_t raw = ps3_bswap64(val);
    VM_WRITE_COH(addr, &raw, sizeof(raw));
}

static inline void vm_write_f32(uint32_t addr, float val)
{
    uint32_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write32(addr, tmp);
}

static inline void vm_write_f64(uint32_t addr, double val)
{
    uint64_t tmp;
    memcpy(&tmp, &val, sizeof(tmp));
    vm_write64(addr, tmp);
}

/* ---------------------------------------------------------------------------
 * Block copies (for string / bulk data movement)
 * -----------------------------------------------------------------------*/
static inline void vm_memcpy_from(void* host_dst, uint32_t guest_src, size_t len)
{
    memcpy(host_dst, vm_ptr8(guest_src), len);
}

/* A block store is a store to every 128-byte line it touches, so it breaks
 * an SPU reservation on any of them the same way vm_write32 does on one:
 * under the lock-line lock, with the lost-reservation event raised per line.
 * Only the lines that are reserved are notified, and a block that touches
 * none takes the plain path. */
static inline int vm_block_reserved(uint32_t guest_dst, size_t len)
{
    if (len == 0) return 0;
    uint32_t first = guest_dst & ~127u;
    uint32_t last  = (uint32_t)(guest_dst + (len - 1)) & ~127u;
    for (uint32_t line = first; ; line += 128u) {
        if (spu_coh_is_reserved(line)) return 1;
        if (line == last) break;
    }
    return 0;
}

static inline void vm_block_notify(uint32_t guest_dst, size_t len)
{
    uint32_t first = guest_dst & ~127u;
    uint32_t last  = (uint32_t)(guest_dst + (len - 1)) & ~127u;
    for (uint32_t line = first; ; line += 128u) {
        if (spu_coh_is_reserved(line)) spu_coh_notify_write(line);
        if (line == last) break;
    }
}

static inline void vm_memcpy_to(uint32_t guest_dst, const void* host_src, size_t len)
{
    if (vm_block_reserved(guest_dst, len)) {
        spu_lockline_lock();
        memcpy(vm_ptr8(guest_dst), host_src, len);
        vm_block_notify(guest_dst, len);
        spu_lockline_unlock();
    } else {
        memcpy(vm_ptr8(guest_dst), host_src, len);
    }
}

static inline void vm_memset(uint32_t guest_dst, int val, size_t len)
{
    if (vm_block_reserved(guest_dst, len)) {
        spu_lockline_lock();
        memset(vm_ptr8(guest_dst), val, len);
        vm_block_notify(guest_dst, len);
        spu_lockline_unlock();
    } else {
        memset(vm_ptr8(guest_dst), val, len);
    }
}

/* lwarx/ldarx and stwcx./stdcx. are resv_lwarx32 / resv_ldarx64 / resv_stcx32 /
 * resv_stcx64 (runtime/memory/reservation.h), one implementation for the
 * lifter's lowering and every harness. */
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PPU_MEMORY_H */
