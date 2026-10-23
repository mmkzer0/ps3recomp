/*
 * ps3recomp - the reservation model: one invalidation rule for PPU
 * lwarx/ldarx + stwcx./stdcx. and SPU GETLLAR + PUTLLC/PUTLLUC/PUTQLLUC.
 *
 * WHAT A RESERVATION IS HERE
 *
 * Every 128-byte line of guest memory has an EPOCH, a counter that advances
 * each time a store lands on the line through a path that reports it. A
 * reservation is the pair (epoch, value) read together by lwarx / GETLLAR; the
 * conditional store (stwcx., stdcx., PUTLLC) succeeds only if the epoch is
 * unchanged AND the reserved bytes are unchanged, and the check and the store
 * are one critical section on the line (resv_line_lock_at ... resv_line_unlock).
 * The epoch is what an equal-value store cannot fake: an SPU PUTLLC that
 * rewrites the bytes it found, or a stwcx. that stores the value it loaded,
 * still advances it, so the other side's reservation is dead.
 *
 * GRANULE
 *
 * The line (128 B) is the unit for BOTH sides. That is an implementation
 * granularity, not a claim about the architectural PPU reservation granule
 * (OQ-E in the adoption matrix is open): a conditional store anywhere in the
 * line kills every reservation on the line, including a PPU thread's
 * reservation on a different word of it (the PPU-to-PPU rule used to be per
 * word). A distinct line is never affected: epochs are exact per line, there is
 * no hashing, so a write to L+128 cannot kill a reservation on L.
 *
 * WHO ADVANCES THE EPOCH (a store "landing" on the line)
 *
 *   stwcx./stdcx. success            resv_line_unlock(ea, 1) in resv_stcx32/64
 *   PUTLLC success, PUTLLUC/PUTQLLUC resv_line_unlock(ea, 1) in spu_channels.c
 *   anything that calls               spu_coh_notify_write[_except]: SPU DMA PUT
 *   spu_coh_notify_write              and SDCRZ into an SPU-marked line, PPU
 *                                     plain stores into an SPU-marked line
 *                                     (VM_WRITE_COH), and the host helpers that
 *                                     edit a taskset or SPURS line under the
 *                                     lock-line lock
 *   resv_host_rmw                     a host-side update of a line (the JQ2
 *                                     port line), under the same rule as a
 *                                     stdcx. that lands
 *
 * KNOWN LIMITS (the hole is stated, not hidden; AM-10 owns the fix)
 *
 *   1. A plain PPU store (stw/std/stb..., the lifter's inline stores) into a
 *      line that no SPU has ever reserved does not advance the epoch. It is
 *      caught only by the value compare, so a same-value plain store does not
 *      kill a PPU or SPU reservation. Closing that needs a hook on every plain
 *      store (a page-flag or epoch fast path), which is the AM-10 design
 *      question and out of scope here. Plain stores into a line some SPU HAS
 *      reserved take the slow path and do advance the epoch.
 *   2. SPU DMA PUT/SDCRZ into a line that no SPU has reserved, but a PPU thread
 *      has, is the same case: value compare only. Scanning for PPU marks would
 *      put a bit test per line on every bulk PUT.
 *   3. The first reservation of a line by an SPU races a PPU stwcx. that does
 *      not yet see the line marked: PUTLLC still fails (the epoch moved), but
 *      the SPU is not sent SPU_EVENT_LR for that one store.
 *   4. A store through a pointer the runtime hands out (vm_ptr*, a host memcpy)
 *      that does not report itself is invisible, as for any plain store.
 *
 * LOCK ORDER
 *
 *   spu_lockline_lock (global)  ->  line epoch lock (this file)
 *
 * and never the reverse. A PPU stwcx. on a line no SPU has reserved takes only
 * the line lock; on a marked line it takes both, in that order.
 */

#ifndef PS3_RESERVATION_H
#define PS3_RESERVATION_H

#include <stdint.h>

#include "../ppu/ppu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RESV_LINE_SIZE 128u

/* ---- line epoch and line lock ------------------------------------------ */

/* The line's current epoch. Lock-free; never allocates; never waits. */
uint64_t resv_epoch(uint32_t ea);

/* Exclusive hold of the line for a read or a store that cannot fail on epoch
 * (GETLLAR, PUTLLUC). Returns the epoch at the moment of the hold. */
uint64_t resv_line_lock(uint32_t ea);

/* Exclusive hold iff the epoch is still `epoch`; returns 0, holding nothing,
 * when a store has landed since. Spins while another holder is inside. */
int resv_line_lock_at(uint32_t ea, uint64_t epoch);

/* Release. `wrote` != 0 advances the epoch: the holder stored to the line. */
void resv_line_unlock(uint32_t ea, int wrote);

/* A store landed on the line outside a hold (a plain store reported by its
 * writer, a DMA PUT). Advances the epoch; safe against a concurrent holder. */
void resv_bump(uint32_t ea);

/* ---- the reserved-by-an-SPU mark ---------------------------------------- */

/* GETLLAR marks its line, so the PPU store paths know to serialize through the
 * lock-line lock and report the store. Marks are never cleared: a line the
 * SPURS kernel reserves is reserved again microseconds later. */
void resv_mark_spu(uint32_t ea);

/* Is the line marked? One load of a global that stays zero until the first
 * GETLLAR, so a title that runs no SPU code pays one predictable branch. */
int resv_marked_spu(uint32_t ea);

/* ---- 128-byte line copies ----------------------------------------------- */

/* Copy a line out of / into guest memory as sixteen relaxed 64-bit atomics, so
 * a concurrent lwarx/ldarx or stwcx./stdcx. on a word of the line is a
 * race-free access (it is one, on real hardware). `line` is the host address
 * of the 128-byte aligned line; the other side may be unaligned. The caller
 * holds the line (resv_line_lock*) when the copy must not tear against a
 * conditional store. */
void resv_copy_out(void* dst, const void* line);
void resv_copy_in(void* line, const void* src);

/* ---- PPU: lwarx/ldarx, stwcx./stdcx. ------------------------------------- */

/* The context fields used: reserve_addr (EA), reserve_value (the raw
 * big-endian bits loaded), reserve_epoch, and reserve_valid (0 none, 4 or 8:
 * the width of the load that made it). */

/* Load and reserve. Returns the host-order value. */
uint32_t resv_lwarx32(ppu_context* ctx, uint32_t ea);
uint64_t resv_ldarx64(ppu_context* ctx, uint32_t ea);

/* Store conditional. Returns 1 if the store landed. Consumes the reservation
 * either way. Fails when no reservation of the same width and address is
 * held, when a store has landed on the line since, or when the reserved bytes
 * changed. */
int resv_stcx32(ppu_context* ctx, uint32_t ea, uint32_t val);
int resv_stcx64(ppu_context* ctx, uint32_t ea, uint64_t val);

/* A plain store by the reserving thread ITSELF into its reserved line, already
 * reported to the model (spu_coh_notify_write, lock-line lock held), does not
 * cost it the reservation: it moves onto the line's new epoch, as an SPU's own
 * DMA PUT does. Other threads' stores into the line still kill it, and the
 * value compare still sees a change to the reserved bytes. Called by the
 * loader's store path with the storing thread's context. */
void resv_keep_own(ppu_context* ctx, uint32_t ea);

/* ---- host read-modify-write ----------------------------------------------- */

/* Edits one 128-byte line as a single critical section of the model: what a
 * successful stwcx./stdcx. is to a PPU thread, for host code that updates a
 * line a guest (PPU or SPU) also reserves, such as the JQ2 port line. `fn`
 * receives the host address of the 128-byte aligned line and returns nonzero
 * iff it stored to it; it must neither block nor take another model lock, and
 * must touch the line through atomics on whole aligned words (a concurrent
 * reserved load, GETLLAR's copy, reads it unlocked).
 *
 * Same lock rule as a stcx. (see LOCK ORDER): on a line an SPU has reserved the
 * lock-line lock is taken first, and the reserving SPUs are told afterwards;
 * on any other line only the line lock is taken. A call whose `fn` reports no
 * store leaves the epoch alone, so it kills no reservation. The line is held
 * unconditionally (there is no reservation to validate). Returns whether `fn`
 * stored (0 or 1). */
typedef int (*resv_rmw_fn)(uint8_t* line, void* user);
int resv_host_rmw(uint32_t ea, resv_rmw_fn fn, void* user);

#ifdef __cplusplus
}
#endif

#endif /* PS3_RESERVATION_H */
