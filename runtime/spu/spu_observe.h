/*
 * ps3recomp - SPU execution observer
 *
 * A per-context, programmatic hook set for tools that drive an SPU context
 * themselves (research harnesses, oracles, differential tests). It is not a
 * diagnostic switch: the observer is installed on a context and is off
 * everywhere else, so nothing about a normal run changes and there are no
 * environment knobs or globals.
 *
 * Every hook site costs one predictable null check when no observer is set.
 * Any NULL callback is skipped. Channels and MFC are shared by lifted and
 * interpreted code, so those callbacks fire for both; on_insn fires only in
 * the interpreter, immediately before it executes an instruction there.
 *
 * Yield contract (on_insn only):
 *   Returning nonzero stops the OUTERMOST spu_interp_run()/
 *   spu_interp_run_until() call with SPU_INTERP_YIELDED, ctx->pc equal to the
 *   instruction that was about to execute, and no other architectural state
 *   consumed. spu_interp_run(ctx, ctx->pc) resumes exactly there.
 *
 *   The interpreter reaches instructions in its own flat loop, but a nested
 *   run can be entered from lifted code (spu_dispatch() and the SMC microstep
 *   call it from a live C frame). A yield there cannot unwind that host stack
 *   without losing guest state, so it is reported on stderr and abort()s
 *   rather than pretending to have stopped. An observer-driven context must
 *   therefore be run from the outermost entry, which is what a harness does.
 */
#ifndef SPU_OBSERVE_H
#define SPU_OBSERVE_H

#include "spu_context.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One MFC command as the SPU enqueued it, read from the staging registers at
 * the MFC_Cmd write. For list commands (`lsa` operand in LS-list form), `ea`
 * is the value the SPU put in EAH:EAL -- the list's local-store address -- and
 * `size` is the list size. */
typedef struct spu_obs_mfc {
    uint32_t cmd;                    /* MFC opcode (0x40 GET, 0x20 PUT, ...) */
    uint32_t tag;                    /* 0..31 */
    uint32_t lsa;                    /* LS address */
    uint64_t ea;                     /* effective address (EAH:EAL) */
    uint32_t size;                   /* bytes */
} spu_obs_mfc;

typedef struct spu_observer {
    void* user;
    /* Before executing the instruction at `pc`. Nonzero = yield; see above. */
    int  (*on_insn)(void* user, spu_context* ctx, uint32_t pc);
    /* Every MFC command at enqueue, atomic commands included. */
    void (*on_mfc)(void* user, spu_context* ctx, const spu_obs_mfc* cmd);
    /* An atomic command completed; `atomic_status` is the value MFC_RdAtomicStat
     * now returns (4 GETLLAR complete, 0 PUTLLC success, 1 PUTLLC failure,
     * 2 PUTLLUC complete). */
    void (*on_atomic)(void* user, spu_context* ctx, uint32_t cmd, uint64_t ea,
                      uint32_t atomic_status);
    /* rdch/wrch, once the value is known. is_write: 1 = wrch, 0 = rdch. */
    void (*on_channel)(void* user, spu_context* ctx, int is_write, uint32_t ch,
                       uint32_t value);
} spu_observer;

/* Never a real stop code: those are the 14-bit `stop` operand. */
#define SPU_INTERP_YIELDED 0xFFFFFFFFu

/* Install or clear (NULL) the observer. Per context; `obs` must outlive use. */
static inline void spu_observer_set(spu_context* ctx, const spu_observer* obs)
{
    if (ctx) ctx->observer = obs;
}

/* ---- hook helpers: the null check each site pays, nothing more ---- */
static inline void spu_obs_mfc_enqueue(spu_context* ctx, uint32_t cmd)
{
    const spu_observer* o = ctx->observer;
    if (!o || !o->on_mfc) return;
    spu_obs_mfc m;
    m.cmd  = cmd;
    m.tag  = ctx->mfc_tag & 0x1Fu;
    m.lsa  = ctx->mfc_lsa;
    m.ea   = ((uint64_t)ctx->mfc_eah << 32) | ctx->mfc_eal;
    m.size = ctx->mfc_size;
    o->on_mfc(o->user, ctx, &m);
}

static inline void spu_obs_atomic(spu_context* ctx, uint32_t cmd, uint64_t ea,
                                  uint32_t atomic_status)
{
    const spu_observer* o = ctx->observer;
    if (o && o->on_atomic) o->on_atomic(o->user, ctx, cmd, ea, atomic_status);
}

static inline void spu_obs_channel(spu_context* ctx, int is_write, uint32_t ch,
                                   uint32_t value)
{
    const spu_observer* o = ctx->observer;
    if (o && o->on_channel) o->on_channel(o->user, ctx, is_write, ch, value);
}

#ifdef __cplusplus
}
#endif
#endif /* SPU_OBSERVE_H */
