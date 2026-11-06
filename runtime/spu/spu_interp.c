/* spu_interp.c — SPU interpreter core. See spu_interp.h.
 *
 * Decode tables are generated from the validated Python decoder
 * (tools/gen_spu_interp.py -> spu_interp_tables.inc); execute calls the SAME
 * spu_<mnemonic> helpers the lifter emits, so interpreted and lifted code are
 * bit-identical. Uncommon opcodes trap loudly (never silently wrong).
 */
#include <time.h>
#include "../platform/mingw_timespec_get.h"   /* timespec_get on msvcrt MinGW */
#include "spu_interp.h"
#include "spu_helpers.h"
#include "spu_observe.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>

/* Channel ABI (runtime/spu/spu_channels.c); the lifter emits these same protos
 * into each generated spu_recomp.h — declared here to stay header-independent. */
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

#include "spu_interp_tables.inc"   /* spu_op enum, spu_op_name[], spu_decode_tab[] */

/* Work descriptor for the next SYS_SPU_THREAD_STOP_RECEIVE_EVENT service, set
 * by the event-port send that wakes a sim SPU. Single-slot: dispatch is
 * synchronous -- the sending PPU thread runs the SPU inline. */
uint32_t g_spu_pending_evt[3];
int      g_spu_pending_evt_valid;

/* ---- decode: one table load, one dispatch ----
 * Every opcode field is a prefix of bits 31..21, so the op is spu_decode_tab[
 * insn >> 21] and the run loop switches on it once: each handler (OP below)
 * reads the register fields it uses straight from the instruction word and
 * binds the immediate or target its op's form defines. The form is the one the
 * generator derived for that op (SPU_FORM_<op>), so a handler cannot read an
 * operand of another form: I and TGT only exist where the form has them.
 *
 * spu_decode1 is the same binding with every field returned as data, for the
 * code that wants them that way (test_spu_decode, the RPCS3 differential
 * test). It visits the ops through SPU_OP_FORMS, so it returns what the handler
 * of that op reads. The run loop does not call it. */
/* spu_decode1 and spu_step below are reference entry points that tests include
 * this file to reach; the run loop uses neither. */
#if defined(__GNUC__) || defined(__clang__)
#  define SPU_INTERP_REFERENCE static inline __attribute__((unused))
#else
#  define SPU_INTERP_REFERENCE static inline
#endif

typedef struct {
    spu_op   op;
    uint8_t  rt, ra, rb, rc, ch;
    int32_t  imm;     /* sign/zero-extended immediate or shift, per form */
    uint32_t tgt;     /* absolute LS branch target (br-family) */
} spu_ins;

SPU_INTERP_REFERENCE void spu_decode1(uint32_t insn, uint32_t pc, spu_ins* d) {
    d->op = (spu_op)spu_decode_tab[insn >> 21];
    d->rt = insn & 0x7F; d->ra = (insn >> 7) & 0x7F;
    d->rb = (insn >> 14) & 0x7F; d->rc = (insn >> 21) & 0x7F;
    d->ch = (insn >> 7) & 0x7F; d->imm = 0; d->tgt = 0;

    switch (d->op) {
#define SPU_DECODE_CASE(name, form) \
    case SPU_##name: { SPU_BIND_##form(insn, pc) SPU_SAVE_##form(d) break; }
    SPU_OP_FORMS(SPU_DECODE_CASE)
#undef SPU_DECODE_CASE
    default: break;
    }
}

/* spu_lifted_lookup() is defined by spu_fn_registry.c (the single owner of the
 * lifted-function table + eviction). Declared in spu_interp.h. */

/* Defined below: the call-trace ring that the br*sl handlers feed and the
 * halt handlers dump. */
void spu_trace_call(uint32_t from, uint32_t to);
void spu_trace_dump(uint32_t at);

/* The instruction word at `pc`. spu_exec's caller and the SPU_INTERP_OPSTATS
 * counter both read it through here, so the histogram counts the op the step
 * runs. One 32-bit load and a swap: four byte loads and shifts were four loads
 * and three ORs per executed instruction. memcpy keeps an unaligned pc (the
 * 0xA70 return masks r0 with SPU_LS_MASK only) reading the same four bytes as
 * before. */
static inline uint32_t spu_fetch(const spu_context* ctx, uint32_t pc) {
    uint32_t m = pc & SPU_LS_MASK, w;
    memcpy(&w, ctx->ls + m, sizeof w);
    return SPU_BSWAP32(w);
}

/* ---- execute one instruction ---- */
#define RT_ ((insn) & 0x7Fu)
#define RA_ (((insn) >> 7) & 0x7Fu)
#define RB_ (((insn) >> 14) & 0x7Fu)
#define RC_ (((insn) >> 21) & 0x7Fu)
#define CH_ RA_                       /* channel number shares ra's field */
#define A   (ctx->gpr[RA_])
#define B   (ctx->gpr[RB_])
#define T   (ctx->gpr[RT_])           /* also the 3rd source in RRR */
#define DST (ctx->gpr[RT_])           /* default destination */
#define DSTC (ctx->gpr[RC_])          /* RRR destination */
#define PREF(r) ((r)._u32[0])

/* One case of spu_exec's switch, with the operand of the op's own form bound
 * (SPU_FORM_<op> is the generator's form for the op). */
#define SPU_BIND_FORM(form, insn, pc) SPU_BIND_FORM_(form, insn, pc)
#define SPU_BIND_FORM_(form, insn, pc) SPU_BIND_##form(insn, pc)
#define OP(name, ...) \
    case SPU_##name: { SPU_BIND_FORM(SPU_FORM_##name, insn, pc) __VA_ARGS__ break; }

#if defined(_MSC_VER)
#  define SPU_EXEC_INLINE static __forceinline
#else
#  define SPU_EXEC_INLINE static inline __attribute__((always_inline))
#endif

/* The fast step is the common case; without the hint the compiler weighs the
 * general prelude like it and spills `steps` to the stack around the dispatch. */
#if defined(__GNUC__) || defined(__clang__)
#  define SPU_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#  define SPU_LIKELY(x) (x)
#endif

/* Every taken indirect branch (bi, bisl, bisled, iret and the conditional
 * bi* forms) carries the interrupt enable/disable bits (E = 0x40000 sets
 * int_enable, D = 0x80000 clears it), exactly as the lifter decodes them. */
#define SPU_IED_BITS() do { \
    if (insn & 0x40000u) ctx->int_enable = 1; \
    else if (insn & 0x80000u) ctx->int_enable = 0; \
} while (0)

/* A taken transfer publishes its target in ctx->pc at once. Two diagnostics
 * sample a running context's pc from another thread to say where an SPU that
 * never answers is: the raw-SPU report while the PPU spins on Mbox_Stat
 * (spu_raw.c) and the SPU_PCSAMPLE thread (spu_channels.c). Every loop is a
 * cycle of taken transfers, so the value stays within one iteration of the
 * truth, as it did when every step stored it; straight-line code in between
 * stays unpublished. */
#define SPU_JUMP(t) do { next = (t); ctx->pc = next; } while (0)

/* Conditional halt (an assertion): stop the SPU when the condition holds. */
#define SPU_HALT_IF(cond) do { if (cond) { \
    spu_trace_dump(pc); \
    ctx->pc = pc; ctx->stop_code = 0x1000; ctx->status = SPU_STATUS_STOPPED_BY_HALT; \
    return 1; } } while (0)

/* cflts/cfltu at I8 <= 14: the conversion shifts by sh = be + 23 - I8, and sh >= 264 (reachable
 * for be >= 241 + I8, i.e. only for I8 <= 14) leaves the hardware's saturating path for a result
 * that is not modelled (489 measured lanes, no bit-exact rule; hwprobe `spuexh` lanes r007/r008 on
 * a CECHC04 with 4.81 REBUG; issue #126). I8 >= 15 cannot reach it (max sh = 255 + 23 - 15 = 263)
 * and is exact. Fail closed like the undecodable-word trap: stop at the word instead of running on
 * a value the console never produced. */
#define SPU_CFL_SCALE_TRAP(i8) do { \
    if ((uint32_t)(i8) <= 14u) { \
        fprintf(stderr, "[spu_interp] unmodelled cflts/cfltu scale (I8=%u, #126) (0x%08X) at LS 0x%05X\n", \
                (unsigned)(i8), insn, pc); \
        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1; \
    } } while (0)

/* Execute the instruction `insn` found at `pc`; returns 1 if the SPU stopped.
 * On return 0 *out_next is the pc to continue at.
 *
 * ctx->pc is NOT the current pc here: the run loop keeps it in a local, and a
 * step that does not leave this function does not store it. A handler stores
 * ctx->pc itself before anything that can observe it:
 *   - the channel ops store pc (spu_rdch/spu_wrch/spu_rchcnt read it, a
 *     blocking one longjmps out of the run through spu_halt, and an MFC command
 *     can run a nested interpreter that overwrites it);
 *   - the local-store accesses store pc (the SPU_LS_WATCH / LOWREAD / SMC
 *     diagnostics print it);
 *   - the taken branches store their target (SPU_JUMP);
 *   - stop, stopd, the halts and the unimplemented-op trap, which leave the
 *     stopping pc (stop: the next one) there for the caller.
 * Every other handler is straight-line register work, a helper of values, or
 * a call that takes its arguments by value (spu_trace_call, spu_trace_dump).
 * After a channel op *general is raised if the context now has an image
 * (image_id >= 0): an MFC command can switch it (spu_dma.h), and no other
 * handler can, so this is the only place the run loop has to look. */
SPU_EXEC_INLINE int spu_exec(spu_context* ctx, uint32_t pc, uint32_t insn,
                             uint32_t* out_next, int* general) {
    uint32_t next = (pc + 4) & 0x3FFFC;

    switch (spu_decode_tab[insn >> 21]) {
    /* immediates / loads */
    OP(il, DST = spu_il(I);)
    OP(ilh, DST = spu_ilh((uint16_t)I);)
    OP(ilhu, DST = spu_splat_u32((uint32_t)I << 16);)
    OP(ila, DST = spu_splat_u32((uint32_t)I & 0x3FFFF);)
    OP(iohl, DST = spu_ori(DST, (int32_t)((uint32_t)I & 0xFFFF));)
    OP(fsmbi, DST = spu_fsmbi(I);)
    OP(lqd, ctx->pc = pc; DST = spu_ls_read128(ctx, PREF(A) + (uint32_t)I);)
    OP(lqx, ctx->pc = pc; DST = spu_ls_read128(ctx, PREF(A) + PREF(B));)
    OP(lqa, ctx->pc = pc; DST = spu_ls_read128(ctx, TGT);)
    OP(lqr, ctx->pc = pc; DST = spu_ls_read128(ctx, TGT);)
    OP(stqd, ctx->pc = pc; spu_ls_write128(ctx, PREF(A) + (uint32_t)I, DST);)
    OP(stqx, ctx->pc = pc; spu_ls_write128(ctx, PREF(A) + PREF(B), DST);)
    OP(stqa, ctx->pc = pc; spu_ls_write128(ctx, TGT, DST);)
    OP(stqr, ctx->pc = pc; spu_ls_write128(ctx, TGT, DST);)
    /* integer arithmetic */
    OP(a, DST = spu_a(A,B);)
    OP(ai, DST = spu_ai(A,I);)
    OP(sf, DST = spu_sf(A,B);)
    OP(sfi, DST = spu_sfi(A,I);)
    OP(ah, DST = spu_ah(A,B);)
    OP(ahi, DST = spu_ahi(A,I);)
    OP(sfh, DST = spu_sfh(A,B);)
    OP(sfhi, DST = spu_sfhi(A,I);)
    OP(addx, DST = spu_addx(A,B,DST);)
    OP(sfx, DST = spu_sfx(A,B,DST);)
    OP(cg, DST = spu_cg(A,B);)
    OP(cgx, DST = spu_cgx(A,B,DST);)
    OP(mpyi, DST = spu_mpyi(A,I);)
    OP(mpyui, DST = spu_mpyui(A,I);)
    /* logical */
    OP(and, DST = spu_and(A,B);)
    OP(or, DST = spu_or(A,B);)
    OP(xor, DST = spu_xor(A,B);)
    OP(nand, DST = spu_nand(A,B);)
    OP(nor, DST = spu_nor(A,B);)
    OP(andc, DST = spu_andc(A,B);)
    OP(orc, DST = spu_orc(A,B);)
    OP(eqv, DST = spu_eqv(A,B);)
    OP(andi, DST = spu_andi(A,I);)
    OP(ori, DST = spu_ori(A,I);)
    OP(xori, DST = spu_xori(A,I);)
    OP(andbi, DST = spu_andbi(A,I);)
    OP(andhi, DST = spu_andhi(A,I);)
    OP(orhi, DST = spu_orhi(A,I);)
    OP(orbi, DST = spu_orbi(A,I);)
    OP(xorhi, DST = spu_xorhi(A,I);)
    OP(xorbi, DST = spu_xorbi(A,I);)
    OP(orx, DST = spu_orx(A);)
    /* compares */
    OP(ceq, DST = spu_ceq(A,B);)
    OP(ceqh, DST = spu_ceqh(A,B);)
    OP(ceqb, DST = spu_ceqb(A,B);)
    OP(ceqi, DST = spu_ceqi(A,I);)
    OP(cgt, DST = spu_cgt(A,B);)
    OP(cgth, DST = spu_cgth(A,B);)
    OP(cgtb, DST = spu_cgtb(A,B);)
    OP(cgti, DST = spu_cgti(A,I);)
    OP(clgt, DST = spu_clgt(A,B);)
    OP(clgth, DST = spu_clgth(A,B);)
    OP(clgtb, DST = spu_clgtb(A,B);)
    OP(clgti, DST = spu_clgti(A,I);)
    /* shifts / rotates */
    OP(shl, DST = spu_shl(A,B);)
    OP(shli, DST = spu_shli(A,I);)
    OP(shlh, DST = spu_shlh(A,B);)
    OP(shlhi, DST = spu_shlhi(A,I);)
    OP(rot, DST = spu_rot(A,B);)
    OP(roti, DST = spu_roti(A,I);)
    OP(rotm, DST = spu_rotm(A,B);)
    OP(rotmi, DST = spu_rotmi(A,I);)
    OP(rotma, DST = spu_rotma(A,B);)
    OP(rotmai, DST = spu_rotmai(A,I);)
    OP(roth, DST = spu_roth(A,B);)
    OP(rothi, DST = spu_rothi(A,I);)
    OP(shlqbi, DST = spu_shlqbi(A,B);)
    OP(shlqbii, DST = spu_shlqbii(A,I);)
    OP(shlqby, DST = spu_shlqby(A,B);)
    OP(shlqbyi, DST = spu_shlqbyi(A,I);)
    OP(rotqbi, DST = spu_rotqbi(A,B);)
    OP(rotqbii, DST = spu_rotqbii(A,I);)
    OP(rotqby, DST = spu_rotqby(A,B);)
    OP(rotqbyi, DST = spu_rotqbyi(A,I);)
    OP(rotqmby, DST = spu_rotqmby(A,B);)
    OP(rotqmbyi, DST = spu_rotqmbyi(A,I);)
    /* select / shuffle / masks / bitcount (RRR: dest=rc, srcs=ra,rb,rt) */
    OP(selb, DSTC = spu_selb(A,B,T);)
    OP(shufb, DSTC = spu_shufb(A,B,T);)
    OP(fsm, DST = spu_fsm(A);)
    OP(fsmb, DST = spu_fsmb(A);)
    OP(fsmh, DST = spu_fsmh(A);)
    OP(gb, DST = spu_gb(A);)
    OP(gbb, DST = spu_gbb(A);)
    OP(gbh, DST = spu_gbh(A);)
    OP(clz, DST = spu_clz(A);)
    OP(cntb, DST = spu_cntb(A);)
    /* gen-controls for insertion */
    OP(cbd, DST = spu_cbd(A,I);)
    OP(chd, DST = spu_chd(A,I);)
    OP(cwd, DST = spu_cwd(A,I);)
    OP(cdd, DST = spu_cdd(A,I);)
    OP(cbx, DST = spu_cbx(A,B);)
    OP(chx, DST = spu_chx(A,B);)
    OP(cwx, DST = spu_cwx(A,B);)
    OP(cdx, DST = spu_cdx(A,B);)
    /* multiply */
    OP(mpy, DST = spu_mpy(A,B);)
    OP(mpyu, DST = spu_mpyu(A,B);)
    OP(mpyh, DST = spu_mpyh(A,B);)
    OP(mpya, DSTC = spu_mpya(A,B,T);)
    /* sign-extend */
    OP(xsbh, DST = spu_xsbh(A);)
    OP(xshw, DST = spu_xshw(A);)
    OP(xswd, DST = spu_xswd(A);)
    /* float */
    OP(fa, DST = spu_fa(A,B);)
    OP(fs, DST = spu_fs(A,B);)
    OP(fm, DST = spu_fm(A,B);)
    OP(fcgt, DST = spu_fcgt(A,B);)
    OP(fceq, DST = spu_fceq(A,B);)
    OP(cflts, SPU_CFL_SCALE_TRAP(I); DST = spu_cflts(A,I);)
    OP(cfltu, SPU_CFL_SCALE_TRAP(I); DST = spu_cfltu(A,I);)
    OP(csflt, DST = spu_csflt(A,I);)
    OP(cuflt, DST = spu_cuflt(A,I);)
    OP(frest, DST = spu_frest(A);)
    OP(frsqest, DST = spu_frsqest(A);)
    OP(fi, DST = spu_fi(A,B);)
    OP(fcmgt, DST = spu_fcmgt(A,B);)
    OP(fcmeq, DST = spu_fcmeq(A,B);)
    OP(fesd, DST = spu_fesd(A);)
    OP(frds, DST = spu_frds(A);)
    OP(fma, DSTC = spu_fma(A,B,T);)    /* RRR: dest=rc */
    OP(fms, DSTC = spu_fms(A,B,T);)
    OP(fnms, DSTC = spu_fnms(A,B,T);)
    /* immediate byte/halfword compares */
    OP(ceqbi, DST = spu_ceqbi(A,I);)
    OP(cgtbi, DST = spu_cgtbi(A,I);)
    OP(clgtbi, DST = spu_clgtbi(A,I);)
    OP(ceqhi, DST = spu_ceqhi(A,I);)
    OP(cgthi, DST = spu_cgthi(A,I);)
    OP(clgthi, DST = spu_clgthi(A,I);)
    /* integer / byte extras */
    OP(bg, DST = spu_bg(A,B);)
    OP(bgx, DST = spu_bgx(A,B,DST);)   /* RR: rt is accumulator + dest */
    OP(absdb, DST = spu_absdb(A,B);)
    OP(avgb, DST = spu_avgb(A,B);)
    OP(sumb, DST = spu_sumb(A,B);)
    OP(mpys, DST = spu_mpys(A,B);)
    OP(mpyhh, DST = spu_mpyhh(A,B);)
    OP(mpyhhu, DST = spu_mpyhhu(A,B);)
    OP(mpyhha, DST = spu_mpyhha(A,B,DST);)   /* RR (0x346): rt is accumulator + dest, NOT RRR */
    OP(mpyhhau, DST = spu_mpyhhau(A,B,DST);)
    /* rotate / shift extras */
    OP(rothm, DST = spu_rothm(A,B);)
    OP(rothmi, DST = spu_rothmi(A,I);)
    OP(rotmah, DST = spu_rothma(A,B);)
    OP(rotmahi, DST = spu_rotmahi(A,I);)
    OP(rotqmbi, DST = spu_rotqmbi(A,B);)
    OP(rotqmbii, DST = spu_rotqmbii(A,(int)I);)
    OP(rotqbybi, DST = spu_rotqbybi(A,B);)
    OP(rotqmbybi, DST = spu_rotqmbybi(A,B);)
    OP(shlqbybi, DST = spu_shlqbybi(A,B);)
    /* channels */
    OP(wrch, ctx->pc = pc; spu_wrch(ctx, CH_, DST); *general |= ctx->image_id >= 0;)
    OP(rdch, ctx->pc = pc; DST = spu_rdch(ctx, CH_); *general |= ctx->image_id >= 0;)
    /* count in the preferred word, rest zero (as the lifter) */
    OP(rchcnt, ctx->pc = pc; DST = spu_pref_u32(spu_rchcnt(ctx, CH_)); *general |= ctx->image_id >= 0;)
    /* hints / no-ops */
    OP(nop,)
    OP(lnop,)
    OP(sync,)
    OP(dsync,)
    OP(hbr,)
    OP(hbra,)
    OP(hbrr,)
    OP(mtspr,)
    OP(fscrwr,)
    OP(mfspr, DST = spu_mfspr(A);)      /* SPRs read as zero */
    OP(fscrrd, DST = spu_fscrrd(A);)    /* FPSCR reads as zero */
    /* double precision */
    OP(dfa, DST = spu_dfa(A,B);)
    OP(dfs, DST = spu_dfs(A,B);)
    OP(dfm, DST = spu_dfm(A,B);)
    OP(dfma, DST = spu_dfma(A,B,DST);)   /* RR: rt is accumulator + dest */
    OP(dfms, DST = spu_dfms(A,B,DST);)
    OP(dfnms, DST = spu_dfnms(A,B,DST);)
    OP(dfnma, DST = spu_dfnma(A,B,DST);)
    /* control flow */
    OP(br, SPU_JUMP(TGT);)
    OP(bra, SPU_JUMP(TGT);)
    OP(brsl, DST = spu_link((pc + 4) & 0x3FFFC); SPU_JUMP(TGT); spu_trace_call(pc, TGT);)
    OP(brasl, DST = spu_link((pc + 4) & 0x3FFFC); SPU_JUMP(TGT); spu_trace_call(pc, TGT);)
    OP(brz, if (PREF(DST) == 0) SPU_JUMP(TGT);)
    OP(brnz, if (PREF(DST) != 0) SPU_JUMP(TGT);)
    OP(brhz, if ((PREF(DST) & 0xFFFF) == 0) SPU_JUMP(TGT);)
    OP(brhnz, if ((PREF(DST) & 0xFFFF) != 0) SPU_JUMP(TGT);)
    OP(bi, SPU_JUMP(PREF(A) & 0x3FFFC); SPU_IED_BITS();)
    OP(bisl,   /* read ra BEFORE the link write: rt may alias ra */
       uint32_t tg = PREF(A) & 0x3FFFC;
       DST = spu_link((pc + 4) & 0x3FFFC); SPU_JUMP(tg); SPU_IED_BITS();)
    OP(bisled,   /* branch when an enabled event is pending */
       uint32_t tg = PREF(A) & 0x3FFFC;
       DST = spu_link((pc + 4) & 0x3FFFC);
       if ((__atomic_load_n(&ctx->event_status, __ATOMIC_ACQUIRE) & ctx->event_mask) != 0) {
           SPU_JUMP(tg);
           SPU_IED_BITS(); })   /* E/D effective when taken, like the lift */
    OP(iret,
       SPU_JUMP(ctx->srr0 & 0x3FFFC);
       /* The handler preserves every register: on a return to the point the
        * interrupt was taken, restore the register file saved there. The
        * lifted dispatch path does this in spu_irq_regs_maybe_restore; an iret
        * executed here has to as well, or the snapshot stays armed and later
        * lands on an unrelated dispatch. */
       SPU_IED_BITS();
       if (ctx->irq_saved && next == (ctx->irq_resume_pc & SPU_LS_MASK)) {
           memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
           ctx->irq_saved = 0;
       })
    OP(biz, if (PREF(DST) == 0) { SPU_JUMP(PREF(A) & 0x3FFFC); SPU_IED_BITS(); })
    OP(binz, if (PREF(DST) != 0) { SPU_JUMP(PREF(A) & 0x3FFFC); SPU_IED_BITS(); })
    OP(bihz, if ((PREF(DST) & 0xFFFF) == 0) { SPU_JUMP(PREF(A) & 0x3FFFC); SPU_IED_BITS(); })
    OP(bihnz, if ((PREF(DST) & 0xFFFF) != 0) { SPU_JUMP(PREF(A) & 0x3FFFC); SPU_IED_BITS(); })
    OP(stop,
       ctx->pc = next; ctx->stop_code = (uint32_t)I;
       ctx->status = SPU_STATUS_STOPPED_BY_STOP; return 1;)
    /* stopd carries no code field: RPCS3 reports 0x3FFF */
    OP(stopd,
       ctx->pc = next; ctx->stop_code = 0x3FFFu;
       ctx->status = SPU_STATUS_STOPPED_BY_STOP; return 1;)
    /* conditional halts (assertions): stop the SPU when the condition holds,
     * else continue. The preferred word of ra is compared. */
    OP(heq, SPU_HALT_IF(PREF(A) == PREF(B));)
    OP(heqi, SPU_HALT_IF(PREF(A) == (uint32_t)I);)
    OP(hgt, SPU_HALT_IF((int32_t)PREF(A) > (int32_t)PREF(B));)
    OP(hgti, SPU_HALT_IF((int32_t)PREF(A) > (int32_t)(uint32_t)I);)
    OP(hlgt, SPU_HALT_IF(PREF(A) > PREF(B));)
    OP(hlgti, SPU_HALT_IF(PREF(A) > (uint32_t)I);)
    /* Unimplemented-op trap: the stopping pc is the word itself, not the next
     * one. dfceq, dfcmeq, dfcgt, dfcmgt and dftsv land here by decode
     * (gen_spu_interp.py maps their prefixes to SPU_word): a retail CBE SPU
     * raises an invalid-instruction error for each, so the guest must not run
     * on a result the console never produced -- hardware, direct: hwprobe
     * `spuexh` special A ran each word alone on a CECHC04 with 4.81 REBUG, the
     * thread group ended with join cause 4 and the stopped thread's exit
     * status read 0x8001000F (CELL_ESTAT); issue #124. */
    default:
        fprintf(stderr, "[spu_interp] unimplemented op '%s' (0x%08X) at LS 0x%05X\n",
                spu_op_name[spu_decode_tab[insn >> 21]], insn, pc);
        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1;
    }
    *out_next = next;
    return 0;
}

/* One instruction at ctx->pc through the context, for callers that step a
 * context rather than run it (the RPCS3 differential test). */
SPU_INTERP_REFERENCE int spu_step(spu_context* ctx) {
    uint32_t pc = ctx->pc, next;
    int general = 0;
    if (spu_exec(ctx, pc, spu_fetch(ctx, pc), &next, &general)) return 1;
    ctx->pc = next;
    return 0;
}

/* Where the last interpreted run got to, for the watchdog's diagnostics: written
 * by every interpreting thread, so relaxed atomics (no ordering is implied). */
uint32_t g_spu_interp_last_pc = 0;
uint64_t g_spu_interp_steps   = 0;

/* Call-trace ring buffer for diagnosing SPU asserts (env SPU_CALLTRACE), one per
 * interpreting thread. */
#define SPU_TRACE_N 32
static SPU_THREAD_LOCAL struct { uint32_t from, to; } s_trace[SPU_TRACE_N];
static SPU_THREAD_LOCAL unsigned s_trace_i = 0;
void spu_trace_call(uint32_t from, uint32_t to) {
    s_trace[s_trace_i % SPU_TRACE_N].from = from;
    s_trace[s_trace_i % SPU_TRACE_N].to   = to;
    s_trace_i++;
}
void spu_trace_dump(uint32_t at) {
    if (!getenv("SPU_CALLTRACE")) return;
    fprintf(stderr, "[SPU-TRACE] halt at LS 0x%05X; last %d calls (from->to):\n", at, SPU_TRACE_N);
    unsigned n = s_trace_i < SPU_TRACE_N ? s_trace_i : SPU_TRACE_N;
    for (unsigned k = 0; k < n; k++) {
        unsigned idx = (s_trace_i - n + k) % SPU_TRACE_N;
        fprintf(stderr, "   0x%05X -> 0x%05X\n", s_trace[idx].from, s_trace[idx].to);
    }
}

uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa) {
    return spu_interp_run_until(ctx, start_lsa, 0);
}

/* Host-stack nesting of interpreter runs. The interpreter's own loop never
 * recurses, but lifted code calls back in through spu_dispatch() and the SMC
 * microstep, and those callers hold live C frames. An observer yield may only
 * stop the outermost run (see spu_observe.h); a nested one is reported below. */
#if defined(_MSC_VER)
#  define SPU_INTERP_TLS __declspec(thread)
#else
#  define SPU_INTERP_TLS _Thread_local
#endif
static SPU_INTERP_TLS int s_interp_depth;

/* The diagnostic gates of spu_interp_run_inner, read from the environment on a
 * thread's first run (YDKJ_SPU_TRACE, SPU_STEPCAP, YDKJ_CRI_GATE1TRACE,
 * SPU_INTERP_STATS, SPU_INTERP_OPSTATS) and the counters that count down as
 * traces are dumped. */
typedef struct interp_diag {
    int init;
    int tr;
    uint64_t cap;
    int g1, g1in;
    long is;
    long ops;       /* SPU_INTERP_OPSTATS seconds; 0 = off */
    int diag;       /* is > 0 || ops > 0: the one flag the hot loop tests for both */
} interp_diag;
static SPU_INTERP_TLS interp_diag t_interp_diag;

static uint32_t spu_interp_run_inner(spu_context* ctx, uint32_t start_lsa,
                                     uint32_t stop_lsa, int outermost);

/* SPU_INTERP_STATS=<seconds>: how much SPU code runs interpreted, by image
 * and 1 KB page of local store, and where the interpreter was entered. A job
 * module streamed in at an address nothing was lifted for runs here at a
 * fraction of the compiled speed, and from the outside that only looks like a
 * workload that is slow for no reason (late audio, slow decode). The entry
 * list tells the two causes apart: entries spread over a code body mean its
 * lift isn't dispatched at all; a few hot entries inside an otherwise lifted
 * body are function starts the lifter didn't find. Per interpreting thread,
 * like the other gates here: the counts need no lock because nothing else
 * touches them. */
enum { IS_N = 64 };
static SPU_INTERP_TLS long s_is = -1;
static SPU_INTERP_TLS long long s_is_tot;
static SPU_INTERP_TLS int s_is_img[IS_N]; static SPU_INTERP_TLS uint32_t s_is_page[IS_N]; static SPU_INTERP_TLS long long s_is_cnt[IS_N];
static SPU_INTERP_TLS int s_ie_img[IS_N]; static SPU_INTERP_TLS uint32_t s_ie_pc[IS_N]; static SPU_INTERP_TLS long long s_ie_cnt[IS_N];
static SPU_INTERP_TLS unsigned long long s_is_t0;

static int interp_stats_on(void)
{
    if (s_is < 0) { const char* e = getenv("SPU_INTERP_STATS"); s_is = e ? atol(e) : 0; }
    return s_is > 0;
}

static void interp_stats_bump(int* img, uint32_t* key, long long* cnt, int image, uint32_t k)
{
    int i;
    for (i = 0; i < IS_N && cnt[i]; i++)
        if (img[i] == image && key[i] == k) break;
    if (i < IS_N) {
        if (!cnt[i]) { img[i] = image; key[i] = k; }
        cnt[i]++;
    }
}

static void interp_stats_top(const char* what, int* img, uint32_t* key, long long* cnt, int n)
{
    fprintf(stderr, "  top %s:\n", what);
    for (int r = 0; r < n; r++) {
        int best = -1;
        for (int j = 0; j < IS_N; j++)
            if (cnt[j] > 0 && (best < 0 || cnt[j] > cnt[best])) best = j;
        if (best < 0) break;
        fprintf(stderr, "    img=0x%08X ls=0x%05X  %lld\n", (uint32_t)img[best], key[best], cnt[best]);
        cnt[best] = -cnt[best];
    }
    for (int j = 0; j < IS_N; j++) cnt[j] = 0;
}

static void interp_stats_step(const spu_context* ctx)
{
    interp_stats_bump(s_is_img, s_is_page, s_is_cnt, ctx->image_id, ctx->pc & ~0x3FFu);
    if ((++s_is_tot & 0xFFFF) != 0) return;
    struct timespec ts; timespec_get(&ts, TIME_UTC);   /* stdlib: fixtures link this file alone */
    unsigned long long now = (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
    if (!s_is_t0) { s_is_t0 = now; return; }
    if (now - s_is_t0 < (unsigned long long)s_is * 1000ull) return;
    fprintf(stderr, "[spu-interp] %.1fM steps/s over %lds\n", s_is_tot / 1e6 / (double)s_is, s_is);
    interp_stats_top("pages (steps)", s_is_img, s_is_page, s_is_cnt, 6);
    interp_stats_top("entries (calls)", s_ie_img, s_ie_pc, s_ie_cnt, 8);
    s_is_tot = 0; s_is_t0 = now;
}

/* SPU_INTERP_OPSTATS=<seconds>: instructions executed per op, printed every
 * <seconds> as `[spu-ops] total=<N> <op>=<count> ...`, busiest op first, ops
 * with no executions left out. The totals are cumulative since the first
 * counted instruction. A profile of the interpreter shows one big switch; this says
 * which handlers the time belongs to. Counts are per interpreting thread and
 * folded into the process-wide totals every OPS_FOLD instructions and when a
 * run returns, as SPU_MFC_STATS does, so counting adds no cross-core traffic
 * per instruction; a thread that is mid-run holds up to OPS_FOLD - 1
 * instructions that a print does not show. The thread whose clock check wins a
 * compare-exchange on the last-print time is the one that prints. The op is
 * counted before the step runs: a stop or halt counts, and so does the
 * instruction SPU_STEPCAP's one-shot dump returns in front of. Short runs
 * (an unlifted call that returns at once) pay the fold on every return, which
 * is part of the cost of asking. */
enum { OPS_FOLD = 4096 };
static _Atomic(uint64_t) s_ops_total[SPU_OP_COUNT];
static _Atomic(uint64_t) s_ops_last_ms;      /* last print; 0 = clock not started */
static SPU_INTERP_TLS uint32_t t_ops_local[SPU_OP_COUNT];
static SPU_INTERP_TLS uint32_t t_ops_pending;

typedef struct { uint64_t n; unsigned op; } ops_row;

static int ops_row_cmp(const void* a, const void* b)
{
    const ops_row* x = (const ops_row*)a; const ops_row* y = (const ops_row*)b;
    if (x->n != y->n) return x->n < y->n ? 1 : -1;
    return (x->op > y->op) - (x->op < y->op);   /* equal counts: stable order */
}

/* One write per line: other threads log to the same stream. A mnemonic is at
 * most 18 characters, so 40 bytes per row bound the line. */
static void interp_opstats_report(FILE* out)
{
    size_t cap = 64 + (size_t)SPU_OP_COUNT * 40;
    ops_row* rows = (ops_row*)malloc((size_t)SPU_OP_COUNT * sizeof *rows);
    char* buf = (char*)malloc(cap);
    if (!rows || !buf) {
        fprintf(stderr, "[spu-ops] out of memory building the report\n");
        free(rows); free(buf);
        return;
    }
    size_t n = 0; uint64_t total = 0;
    for (unsigned op = 0; op < SPU_OP_COUNT; op++) {
        uint64_t c = atomic_load_explicit(&s_ops_total[op], memory_order_relaxed);
        total += c;
        if (c) { rows[n].n = c; rows[n].op = op; n++; }
    }
    qsort(rows, n, sizeof *rows, ops_row_cmp);
    size_t len = (size_t)snprintf(buf, cap, "[spu-ops] total=%llu", (unsigned long long)total);
    for (size_t i = 0; i < n; i++) {
        const char* name = spu_op_name[rows[i].op];
        len += (size_t)snprintf(buf + len, cap - len, " %s=%llu", name ? name : "?",
                                (unsigned long long)rows[i].n);
    }
    buf[len++] = '\n'; buf[len] = '\0';
    fputs(buf, out); fflush(out);
    free(rows); free(buf);
}

static void interp_opstats_fold(void)
{
    for (unsigned op = 0; op < SPU_OP_COUNT; op++)
        if (t_ops_local[op]) {
            atomic_fetch_add_explicit(&s_ops_total[op], t_ops_local[op], memory_order_relaxed);
            t_ops_local[op] = 0;
        }
    t_ops_pending = 0;
}

static void interp_opstats_tick(long period)
{
    struct timespec ts; timespec_get(&ts, TIME_UTC);   /* stdlib: fixtures link this file alone */
    uint64_t now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    uint64_t last = atomic_load_explicit(&s_ops_last_ms, memory_order_relaxed);
    if (!last) {   /* first fold anywhere: start the clock */
        atomic_compare_exchange_strong_explicit(&s_ops_last_ms, &last, now,
                                                memory_order_relaxed, memory_order_relaxed);
        return;
    }
    if (now < last + (uint64_t)period * 1000u) return;
    if (atomic_compare_exchange_strong_explicit(&s_ops_last_ms, &last, now,
                                                memory_order_relaxed, memory_order_relaxed))
        interp_opstats_report(stderr);
}

static void interp_opstats_step(const spu_context* ctx, long period)
{
    t_ops_local[spu_decode_tab[spu_fetch(ctx, ctx->pc) >> 21]]++;
    if (++t_ops_pending < OPS_FOLD) return;
    interp_opstats_fold();
    interp_opstats_tick(period);
}

static void interp_opstats_flush(long period)
{
    if (!t_ops_pending) return;
    interp_opstats_fold();
    interp_opstats_tick(period);
}

static void interp_diag_step(const spu_context* ctx, const interp_diag* dg)
{
    if (dg->is > 0) interp_stats_step(ctx);
    if (dg->ops > 0) interp_opstats_step(ctx, dg->ops);
}

uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa) {
    if (interp_stats_on()) {
        /* Credit an entry inside a resident code region to the image that
         * owns it, so the list names the lift that's missing the entry. */
        uint32_t pc = start_lsa & 0x3FFFC, key = pc; int owner = ctx->image_id;
        for (unsigned s = 0; s < 4; ++s)
            if (ctx->resident_code[s].image_id && pc >= ctx->resident_code[s].lsa &&
                pc - ctx->resident_code[s].lsa < ctx->resident_code[s].size) {
                owner = ctx->resident_code[s].image_id;
                /* Relocatable code: the CANONICAL pc (placement-independent), which
                 * is the entry its --reloc lift needs. */
                key = ctx->resident_code[s].canon
                    ? pc - ctx->resident_code[s].lsa + ctx->resident_code[s].canon : pc;
            }
        interp_stats_bump(s_ie_img, s_ie_pc, s_ie_cnt, owner, key);
    }
    int depth = ++s_interp_depth;
    uint32_t r = spu_interp_run_inner(ctx, start_lsa, stop_lsa, depth == 1);
    --s_interp_depth;
    if (t_interp_diag.ops > 0) interp_opstats_flush(t_interp_diag.ops);
    return r;
}

static uint32_t spu_interp_run_inner(spu_context* ctx, uint32_t start_lsa,
                                     uint32_t stop_lsa, int outermost) {
    ctx->pc = start_lsa & 0x3FFFC;
    ctx->status = SPU_STATUS_RUNNING;
    uint64_t steps = 0;
    uint32_t pc = ctx->pc, next = 0;
    /* YDKJ_SPU_TRACE=N: log the last N PCs into a ring buffer and dump them when the
     * interp halts -- shows the path to a branch-to-0 (the cri task/policy wall). */
    /* The env gates and their counters are per interpreting thread: they are
     * read lazily and the counters count down as a trace is dumped. `dg` keeps
     * the hot loop's checks off the thread-local lookup. */
    interp_diag* const dg = &t_interp_diag;
    if (!dg->init) {
        const char* e;
        dg->init = 1;
        e = getenv("YDKJ_SPU_TRACE");       dg->tr  = e ? atoi(e) : 0;
        e = getenv("SPU_STEPCAP");          dg->cap = e ? strtoull(e, 0, 0) : 0;
        e = getenv("YDKJ_CRI_GATE1TRACE");  dg->g1  = e ? atoi(e) : 0;
        e = getenv("SPU_INTERP_STATS");     dg->is  = e ? atol(e) : 0;
        e = getenv("SPU_INTERP_OPSTATS");   dg->ops = e ? atol(e) : 0;
        dg->diag = dg->is > 0 || dg->ops > 0;
    /* YDKJ_CRI_GATE1TRACE=N: cri decode task busy-spins in the validator func_00026E80
     * (LS 0x26E80..0x26F14), a straight-line leaf that returns r3 = 0 / 0x8041090F /
     * 0x80410909. Hand-decoding its selb/fsm/gb/ceqh lanes proved unreliable, so log the
     * ACTUAL runtime result + inputs the first N times it returns. Fires on range-exit
     * (pc left the fn), when r3 and the leaf's non-restored intermediates are still live. */
    }
    uint32_t ring[64]; int rc=0, rn=0;
    /* Everything between the prelude below and `step:` is a no-op for pure
     * interpretation (image_id < 0) with no observer and no diagnostic armed,
     * which is nearly every instruction of a raw SPU job. `general` says
     * whether any of it can fire; while it is clear a step skips it, and the
     * loop re-tests only what a step can change:
     *   - pc == 0xA70: the HLE hook below needs the full iteration;
     *   - image_id: only a channel op can change it (the MFC command that
     *     DMAs the taskset policy to LS 0xA00 does, spu_dma.h), and the rejoin
     *     check must see the new value before the next instruction. spu_exec
     *     raises `general` after a channel op that leaves an image, so a step
     *     that makes no channel access does not load image_id at all.
     * The observer is not re-read per step: a callback can only run, and so
     * only replace itself, while `general` is set; the driver installs one
     * between runs. The dg gates are fixed once the environment has been read
     * and only count down from a nonzero value, so they cannot arm mid-run.
     * One spu_exec call site serves both paths, which keeps it inlined.
     *
     * `pc` and `steps` are locals. ctx->pc is written where something can read
     * it; spu_exec lists its own writes (channel ops, local-store accesses,
     * taken branches, stop/halt/trap). Here it is written at:
     *   - the top of every general iteration, before anything below reads
     *     ctx->pc: the HLE hook, the rejoin lookup, the observer, the
     *     diagnostics and the STEPCAP/trace dumps (an observer may move it,
     *     and `pc` is reloaded after the callback);
     *   - the 0xA70 HLE return, which resumes at r0 through ctx->pc;
     *   - the stop 0x110 service, after which `pc` is reloaded from the
     *     ctx->pc that the stop left (the next instruction).
     * A fast step stores nothing else, so between taken branches ctx->pc is
     * stale while fast steps run; no code on this thread reads it then, and
     * the one reader on another thread is served by SPU_JUMP.
     * A longjmp out of the run (spu_halt from a channel op) leaves ctx->pc at
     * the pc of the instruction that made the call, as it always did.
     *
     * g_spu_interp_steps / g_spu_interp_last_pc are read only after a run
     * returns (lv2_register.c), so a fast step does not store last_pc: the stop
     * return writes it (the pc of the last step issued), and the other returns
     * are in the general path, which still stores it per step. `steps` is
     * published at every normal return and not by a longjmp, as before. */
    int general = 1;
    for (;;) {
        if (SPU_LIKELY(!(general | (pc == 0xA70u)))) { steps++; goto step; }
        ctx->pc = pc;
        general = ctx->image_id >= 0 || ctx->observer || dg->g1 > 0 || dg->tr > 0 || dg->diag;
        if (!general && pc != 0xA70u) { steps++; goto step; }
        /* Match the lifted path: only synthetic HLE tasks may bypass the
         * resident policy. Real tasksets also store 0xA70 in their API table.
         * Preserve the legacy CRI return convention while invoking the HLE. */
        if (ctx->pc == 0xA70u) {
            uint32_t sc = ((uint32_t)ctx->ls[0x27C4]<<24)|((uint32_t)ctx->ls[0x27C5]<<16)
                        | ((uint32_t)ctx->ls[0x27C6]<<8) | ctx->ls[0x27C7];
            if (ctx->image_id == 22 || (ctx->policy_mode && sc == 0xA70u)) {
                extern void spu_spurs_taskset_syscall(spu_context*);
                int _save = ctx->image_id; ctx->image_id = 22;
                spu_spurs_taskset_syscall(ctx);
                ctx->image_id = _save;
                ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;   /* resume at link r0 */
                pc = ctx->pc;
                continue;
            }
        }
        /* Rejoin the compiled fast path only for images that HAVE lifted
         * functions (image_id >= 0). image_id < 0 = pure interpretation: an
         * un-lifted image (e.g. a title's raw SPU jobs) must never rejoin
         * another image's functions that happen to share an LS address. */
        /* ponytail: pc-only match; a recursive call through the same site would stop early -- compare r1 too if one shows up. */
        if (ctx->image_id >= 0 && ((stop_lsa && ctx->pc == stop_lsa) || spu_lifted_lookup(ctx, ctx->pc))) { SPU_RELAXED_STORE(&g_spu_interp_steps, steps); SPU_RELAXED_STORE(&g_spu_interp_last_pc, ctx->pc); return ctx->pc; }  /* rejoin fast path */
        /* Observer yield: before executing the instruction at ctx->pc, with no
         * state consumed -- resuming at ctx->pc re-enters exactly here. Only
         * the outermost run may stop this way; see spu_observe.h for why a
         * nested one has no host stack to unwind. */
        { const spu_observer* obs = ctx->observer;
          if (obs && obs->on_insn && obs->on_insn(obs->user, ctx, ctx->pc)) {
              if (!outermost) {
                  fprintf(stderr, "[spu-interp] observer yield at LS 0x%05X inside a nested "
                                  "interpreter run (host depth %d); unsupported -- see "
                                  "spu_observe.h\n",
                          ctx->pc & SPU_LS_MASK, s_interp_depth);
                  fflush(stderr);
                  abort();
              }
              return SPU_INTERP_YIELDED;
          } }
        SPU_RELAXED_STORE(&g_spu_interp_last_pc, pc = ctx->pc);   /* an observer may have moved pc */
        if (dg->g1>0) {
            int inr = (ctx->pc >= 0x26E80u && ctx->pc < 0x26F14u);
            if (inr) { dg->g1in = 1; }
            else if (dg->g1in) {
                dg->g1in = 0;
                #define GLB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
                fprintf(stderr,"[gate1] func_00026E80 ret r3=%08X | LS2FB0=%08X LS2FB4=%08X LS2FB8=%08X | LS1E0=%08X LS1E4=%08X LS1E8=%08X | r1=%05X r29=%08X r32=%08X r16=%08X r22=%08X r8=%08X r6=%08X\n",
                    ctx->gpr[3]._u32[0], GLB(0x2FB0),GLB(0x2FB4),GLB(0x2FB8), GLB(0x1E0),GLB(0x1E4),GLB(0x1E8),
                    ctx->gpr[1]._u32[0], ctx->gpr[29]._u32[0], ctx->gpr[32]._u32[0],
                    ctx->gpr[16]._u32[0], ctx->gpr[22]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[6]._u32[0]);
                #undef GLB
                fflush(stderr); dg->g1--;
            }
        }
        if (dg->tr>0) { ring[rc&63]=ctx->pc; rc++; if(rn<64)rn++; }
        steps++;
        if (dg->diag) interp_diag_step(ctx, dg);
        /* SPU_STEPCAP=N: a task that never halts (infinite work/wait loop) never
         * dumps its ring. Force a one-shot dump after N steps to see where it loops. */
        if (dg->tr>0 && dg->cap && steps == dg->cap) {
            fprintf(stderr,"[spu-trace] STEPCAP pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->pc, (unsigned long long)steps, rn);
            for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
            fprintf(stderr,"\n");
            /* dump the LS 0x2700 SpursTasksetContext the poll reads -- compare to the
             * working RPCS3 SPU0 dump (RUNNING/READY/ENABLED=0x80000000, SIGNALLED=0). */
            #define LB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
            fprintf(stderr,"[spu-trace] LS ctx: RUN=%08X READY=%08X PEND=%08X ENA=%08X SIG=%08X WAIT=%08X | 2790=%08X 2794=%08X 27D8=%08X 2FB0=%08X\n",
                LB(0x2700),LB(0x2710),LB(0x2720),LB(0x2730),LB(0x2740),LB(0x2750),LB(0x2790),LB(0x2794),LB(0x27D8),LB(0x2FB0));
            #undef LB
            fflush(stderr); dg->tr--; SPU_RELAXED_STORE(&g_spu_interp_steps, steps); return 0x2000u;
        }
        step:
        if (spu_exec(ctx, pc, spu_fetch(ctx, pc), &next, &general)) {
            /* stop 0x110 = SYS_SPU_THREAD_STOP_RECEIVE_EVENT. The worker writes
             * the SPU queue number to its out-mailbox, stops, and lv2 replies
             * with {CELL_OK, data1, data2, data3} in the in-mailbox -- which is
             * how these sim SPUs receive each frame's work-descriptor EA.
             * Treating the stop as "job finished" let them resume with nothing
             * in that structure and DMA their results to address 0.
             * Service it and keep running; with no event pending, fall through
             * to the old behaviour so nothing waits forever. */
            if (ctx->stop_code == 0x110 && g_spu_pending_evt_valid) {
                spu_channel_read(&ctx->ch_out_mbox);      /* the SPU queue key */
                ctx->rcv_evt[0] = 0;                      /* CELL_OK */
                ctx->rcv_evt[1] = g_spu_pending_evt[0];
                ctx->rcv_evt[2] = g_spu_pending_evt[1];
                ctx->rcv_evt[3] = g_spu_pending_evt[2];
                ctx->rcv_evt_n = 4; ctx->rcv_evt_i = 0;
                g_spu_pending_evt_valid = 0;
                ctx->status = SPU_STATUS_RUNNING;
                pc = ctx->pc;   /* the stop stored the next pc there */
                continue;
            }
            SPU_RELAXED_STORE(&g_spu_interp_steps, steps);
            SPU_RELAXED_STORE(&g_spu_interp_last_pc, pc);
            if (dg->tr>0) { fprintf(stderr,"[spu-trace] halt stop=0x%X pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->stop_code, ctx->pc, (unsigned long long)steps, rn);
                for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
                fprintf(stderr,"\n"); fflush(stderr); dg->tr--; }
            return ctx->stop_code; }
        pc = next;
    }
}

void spu_dispatch(spu_context* ctx, uint32_t target) {
    for (;;) {
        target &= 0x3FFFC;
        spu_lifted_fn fn = spu_lifted_lookup(ctx, target);
        if (fn) { ctx->pc = target; fn(ctx); return; }
        uint32_t next = spu_interp_run(ctx, target);
        if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT))
            return;
        if ((next & 0x3FFFC) == target) return;   /* no-progress guard */
        target = next;
    }
}
