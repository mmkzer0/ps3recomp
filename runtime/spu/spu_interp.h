/* spu_interp.h — SPU interpreter: the correctness floor for the SPU recompiler.
 *
 * Statically lifted functions are the fast path. Computed/indirect branches
 * (`bi $reg`) and code that only exists at runtime — DMA'd overlays (LBP),
 * self-modified code (spu_0004's `stqd; sync; bi $3` trampoline at LS 0xC0) —
 * have no static bytes to lift. On a lifted-function-table miss we interpret
 * from live local store instead of falling off a cliff.
 *
 * The interpreter shares the EXACT spu_context (gpr, ls, channels) the lifted
 * code uses and calls the SAME spu_<mnemonic> helpers, so control and state
 * cross the boundary transparently.
 */
#ifndef SPU_INTERP_H
#define SPU_INTERP_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A lifted SPU function: void f(spu_context*). Matches the lifter's signature. */
typedef void (*spu_lifted_fn)(spu_context*);

/* Resolve this LSA using runtime ownership: bounded resident code (including
 * authoritative misses), then task, overlay, and primary image. This is the
 * same resolver used by indirect dispatch. NULL ctx retains wildcard lookup. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

/* The function-unit replay can select the original lifted reference or the
 * native override registered for one image/address pair. */
spu_lifted_fn spu_registered_function_variant(uint32_t lsa, int image_id, int native_override);

/* Invoke a decoded leaf as a host callee, preserving SPU return semantics. */
void spu_run_lifted_leaf(spu_lifted_fn entry, spu_context* ctx);

/* Interpret from `start_lsa` until the SPU stops (`stop`/`stopd`/halt) or a
 * host-fatal decode. Handles all control flow internally. Returns the stop
 * code (0 if none). ctx->pc holds the address that stopped execution. */
uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);
/* As spu_interp_run, but also hands back at stop_lsa (0 = no such point): the
 * return point of the drain the caller goes back to (spu_smc_microstep). */
uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa);

/* This thread's interpreter nesting depth: the number of spu_interp_run* calls live on its host stack.
 * Only the outermost run (depth 1) may stop at an observer yield (spu_observe.h). A non-local exit
 * (spu_halt to spu_run_with_halt) skips the decrement of every run it jumps over, so the landing pad
 * saves the depth before it arms and restores it after a halt; nothing else needs these. */
int  spu_interp_depth_get(void);
void spu_interp_depth_set(int depth);

/* The single dispatch point every computed branch routes through. If `target`
 * is a registered lifted entry, runs it; otherwise interprets from `target`.
 * The lifter emits `bi $reg` / unresolved indirect branches as a call here. */
void spu_dispatch(spu_context* ctx, uint32_t target);

#ifdef __cplusplus
}
#endif
#endif /* SPU_INTERP_H */
