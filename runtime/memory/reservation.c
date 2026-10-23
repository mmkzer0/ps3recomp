/*
 * ps3recomp - the reservation model. See reservation.h for the rule, the
 * granule, who advances an epoch and what the model does not see.
 *
 * STATE
 *
 * One 64-bit cell per 128-byte line of the 32-bit guest address space: bit 0 is
 * the line lock, the rest is the epoch (so the epoch moves in steps of two and
 * resv_epoch() always reads an even number). Cells live in 4096-line chunks
 * (512 KiB of guest memory, 32 KiB of cells) allocated the first time a line in
 * the chunk is written through the model; the directory is 8192 pointers. That
 * keeps the table exact, with no hashing and no aliasing between lines, at a
 * cost proportional to the guest memory that has seen atomics.
 *
 * A held cell is epoch+1. A holder that stored releases with +1 (epoch+2, even
 * again), one that did not with -1; a bump from outside is +2 and composes with
 * both, so no advance is ever lost to a concurrent holder.
 */

#include "reservation.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/ps3emu/endian.h"

extern uint8_t* vm_base;

/* The SPU half (runtime/spu/spu_coherency.c). */
void spu_lockline_lock(void);
void spu_lockline_unlock(void);
void spu_coh_notify_write(uint32_t ea);

/* ===========================================================================
 * Line cells
 * ===========================================================================*/
#define RESV_LINE_SHIFT   7
#define RESV_CHUNK_SHIFT  12
#define RESV_CHUNK_LINES  (1u << RESV_CHUNK_SHIFT)
#define RESV_DIR_ENTRIES  (1u << (32 - RESV_LINE_SHIFT - RESV_CHUNK_SHIFT))

typedef _Atomic uint64_t resv_cell;
static resv_cell* _Atomic s_dir[RESV_DIR_ENTRIES];

static inline void resv_relax(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static resv_cell* cell_find(uint32_t ea)
{
    uint32_t line = ea >> RESV_LINE_SHIFT;
    resv_cell* chunk = atomic_load_explicit(&s_dir[line >> RESV_CHUNK_SHIFT],
                                            memory_order_acquire);
    return chunk ? &chunk[line & (RESV_CHUNK_LINES - 1u)] : NULL;
}

static resv_cell* cell_get(uint32_t ea)
{
    uint32_t line = ea >> RESV_LINE_SHIFT;
    resv_cell* _Atomic* slot = &s_dir[line >> RESV_CHUNK_SHIFT];
    resv_cell* chunk = atomic_load_explicit(slot, memory_order_acquire);
    if (!chunk) {
        resv_cell* fresh = (resv_cell*)calloc(RESV_CHUNK_LINES, sizeof(resv_cell));
        if (!fresh) {
            fprintf(stderr, "[resv] cannot allocate the epoch cells for line 0x%08X\n",
                    ea & ~(RESV_LINE_SIZE - 1u));
            fflush(stderr);
            abort(); /* a line without a cell would lose invalidations */
        }
        if (atomic_compare_exchange_strong_explicit(slot, &chunk, fresh,
                memory_order_acq_rel, memory_order_acquire))
            chunk = fresh;
        else
            free(fresh);
    }
    return &chunk[line & (RESV_CHUNK_LINES - 1u)];
}

uint64_t resv_epoch(uint32_t ea)
{
    resv_cell* c = cell_find(ea);
    return c ? atomic_load_explicit(c, memory_order_acquire) & ~(uint64_t)1 : 0;
}

uint64_t resv_line_lock(uint32_t ea)
{
    resv_cell* c = cell_get(ea);
    for (;;) {
        uint64_t s = atomic_load_explicit(c, memory_order_relaxed);
        if (!(s & 1) &&
            atomic_compare_exchange_weak_explicit(c, &s, s + 1,
                memory_order_acquire, memory_order_relaxed))
            return s;
        resv_relax();
    }
}

int resv_line_lock_at(uint32_t ea, uint64_t epoch)
{
    resv_cell* c = cell_get(ea);
    for (;;) {
        uint64_t s = atomic_load_explicit(c, memory_order_acquire);
        if (s & 1) { resv_relax(); continue; }   /* a holder: it may still move the epoch */
        if (s != epoch) return 0;
        if (atomic_compare_exchange_weak_explicit(c, &s, s + 1,
                memory_order_acquire, memory_order_relaxed))
            return 1;
    }
}

void resv_line_unlock(uint32_t ea, int wrote)
{
    resv_cell* c = cell_get(ea);
    if (wrote) atomic_fetch_add_explicit(c, 1, memory_order_release);
    else       atomic_fetch_sub_explicit(c, 1, memory_order_release);
}

void resv_bump(uint32_t ea)
{
    atomic_fetch_add_explicit(cell_get(ea), 2, memory_order_release);
}

/* ===========================================================================
 * The SPU mark: one bit per line over the whole address space (4 MiB of BSS,
 * of which only the pages around reserved lines are ever touched). The guest
 * address space is not one window: the ELF image, the sys_memory region at
 * 0x40000000 and the raw-SPU windows are all reservable, and a line outside a
 * hardcoded range would be silently non-coherent.
 * ===========================================================================*/
#define RESV_MARK_BYTES (1u << (32 - RESV_LINE_SHIFT - 3))

static _Atomic unsigned char s_marks[RESV_MARK_BYTES];

/* Zero until the first GETLLAR anywhere: keeps the PPU store paths off the
 * bitmap entirely for a title that runs no SPU code. A stale zero read by a
 * store racing the very first reservation costs at most that one event. */
static _Atomic int s_marks_armed;

void resv_mark_spu(uint32_t ea)
{
    uint32_t line = ea >> RESV_LINE_SHIFT;
    atomic_fetch_or_explicit(&s_marks[line >> 3], (unsigned char)(1u << (line & 7)),
                             memory_order_relaxed);
    atomic_store_explicit(&s_marks_armed, 1, memory_order_relaxed);
}

int resv_marked_spu(uint32_t ea)
{
    if (!atomic_load_explicit(&s_marks_armed, memory_order_relaxed)) return 0;
    uint32_t line = ea >> RESV_LINE_SHIFT;
    return (atomic_load_explicit(&s_marks[line >> 3], memory_order_relaxed) >> (line & 7)) & 1u;
}

/* ===========================================================================
 * Line copies
 * ===========================================================================*/
void resv_copy_out(void* dst, const void* line)
{
    uint64_t w[RESV_LINE_SIZE / 8];
    const resv_cell* src = (const resv_cell*)line;
    for (unsigned i = 0; i < RESV_LINE_SIZE / 8; i++)
        w[i] = atomic_load_explicit(&src[i], memory_order_relaxed);
    memcpy(dst, w, sizeof w);
}

void resv_copy_in(void* line, const void* src)
{
    uint64_t w[RESV_LINE_SIZE / 8];
    memcpy(w, src, sizeof w);
    resv_cell* dst = (resv_cell*)line;
    for (unsigned i = 0; i < RESV_LINE_SIZE / 8; i++)
        atomic_store_explicit(&dst[i], w[i], memory_order_relaxed);
}

/* ===========================================================================
 * PPU
 * ===========================================================================*/
static void ppu_reserve(ppu_context* ctx, uint32_t ea, int width, uint64_t raw, uint64_t epoch)
{
    ctx->reserve_addr  = ea;
    ctx->reserve_value = raw;
    ctx->reserve_epoch = epoch;
    ctx->reserve_valid = width;
}

/* The epoch is read BEFORE the value. A writer stores, then advances the
 * epoch; a reader that sees the new epoch therefore sees the new value, and
 * one that sees the old epoch with the new value holds a reservation that is
 * already dead. Never the reverse: an old value under the new epoch would let
 * a stale reservation commit. */
uint32_t resv_lwarx32(ppu_context* ctx, uint32_t ea)
{
    uint64_t epoch = resv_epoch(ea);
    uint32_t raw = atomic_load_explicit((_Atomic uint32_t*)(vm_base + ea), memory_order_acquire);
    ppu_reserve(ctx, ea, 4, raw, epoch);
    return ps3_bswap32(raw);
}

uint64_t resv_ldarx64(ppu_context* ctx, uint32_t ea)
{
    uint64_t epoch = resv_epoch(ea);
    uint64_t raw = atomic_load_explicit((_Atomic uint64_t*)(vm_base + ea), memory_order_acquire);
    ppu_reserve(ctx, ea, 8, raw, epoch);
    return ps3_bswap64(raw);
}

/* Validate and commit as one critical section on the line. On a line an SPU
 * has reserved the lock-line lock is taken first (the lock order), which is
 * also what keeps a host helper that edits the line under that lock from
 * interleaving, and the reserving SPUs are told afterwards. */
static int stcx(ppu_context* ctx, uint32_t ea, int width, uint64_t desired_raw)
{
    int ok = 0;
    if (ctx->reserve_valid == width && ctx->reserve_addr == ea) {
        const int marked = resv_marked_spu(ea);
        if (marked) spu_lockline_lock();
        if (resv_line_lock_at(ea, ctx->reserve_epoch)) {
            if (width == 4) {
                uint32_t expected = (uint32_t)ctx->reserve_value;
                ok = atomic_compare_exchange_strong_explicit(
                    (_Atomic uint32_t*)(vm_base + ea), &expected, (uint32_t)desired_raw,
                    memory_order_acq_rel, memory_order_acquire);
            } else {
                uint64_t expected = ctx->reserve_value;
                ok = atomic_compare_exchange_strong_explicit(
                    (_Atomic uint64_t*)(vm_base + ea), &expected, desired_raw,
                    memory_order_acq_rel, memory_order_acquire);
            }
            resv_line_unlock(ea, ok);
        }
        if (marked) {
            if (ok) spu_coh_notify_write(ea);
            spu_lockline_unlock();
        }
    }
    ctx->reserve_valid = 0;
    return ok;
}

int resv_stcx32(ppu_context* ctx, uint32_t ea, uint32_t val)
{
    return stcx(ctx, ea, 4, ps3_bswap32(val));
}

int resv_stcx64(ppu_context* ctx, uint32_t ea, uint64_t val)
{
    return stcx(ctx, ea, 8, ps3_bswap64(val));
}

void resv_keep_own(ppu_context* ctx, uint32_t ea)
{
    if (ctx && ctx->reserve_valid &&
        ((uint32_t)ctx->reserve_addr >> RESV_LINE_SHIFT) == (ea >> RESV_LINE_SHIFT))
        ctx->reserve_epoch = resv_epoch(ea);
}

/* The same critical section as stcx() above, minus the reservation to validate:
 * the line is held unconditionally and the epoch advances only if `fn` stored. */
int resv_host_rmw(uint32_t ea, resv_rmw_fn fn, void* user)
{
    const int marked = resv_marked_spu(ea);
    if (marked) spu_lockline_lock();
    resv_line_lock(ea);
    const int wrote = fn(vm_base + (ea & ~(RESV_LINE_SIZE - 1u)), user) != 0;
    resv_line_unlock(ea, wrote);
    if (marked) {
        if (wrote) spu_coh_notify_write(ea);
        spu_lockline_unlock();
    }
    return wrote;
}
