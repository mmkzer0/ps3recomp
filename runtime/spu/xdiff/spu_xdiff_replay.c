/*
 * ps3recomp - replaying a fixture into a shadow context (docs/SPU_XDIFF.md
 * sections 8.1, 9 and 10; interface in spu_xdiff_replay.h).
 *
 * The serve table is a cursor over the transcript with no model of the MFC:
 * every request the subject makes is matched against the next record, every
 * value it reads is the recorded one, and the first difference ends the run
 * through spu_port_serve_halt. Nothing below the effect port runs for the
 * shadow, so a subject that diverged cannot reach guest memory, a mailbox or
 * a lock (section 5.2).
 */
#include "spu_xdiff_replay.h"

#include "../spu_context.h"
#include "../spu_interp.h"
#include "../spu_observe.h"
#include "../spu_workload.h"
#include "../spurs_job.h"
#include "spu_xdiff_census.h"
#include "ps3emu/lifter_abi.h"
#include "ps3emu/ps3_log.h"
#include "ps3emu/stop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);

/* The image id the interpreter's 0xA70 taskset hook is tied to (spu_interp.c); with policy_mode it is
 * the only way a context reaches that hook. */
#define XDIFF_TASKSET_HLE_IMAGE 22

/* ---- knobs ---------------------------------------------------------------- */

/* The behaviour-changing knobs the runtime latches from the environment and a fixture does not
 * carry (section 8.1). A replay under one compares a different program. "Set" is any value, even an
 * empty one: SPU_TAILRET=0 is refused too, because the latches read their variables in different
 * ways (getenv() != NULL for most, the first character for SPU_TAILRET) and one rule that never
 * under-refuses is worth more than the convenience of a spelled-out zero. */
const char* spu_xdiff_refused_knob(void)
{
    static const char* const k_knobs[] = { "YDKJ_STOP_HALT", "SPU_TAILRET", "SPU_NO_LS0_END", "SPU_INTERP_UNLIFTED" };
    for (size_t i = 0; i < sizeof k_knobs / sizeof k_knobs[0]; i++)
        if (getenv(k_knobs[i]) != NULL) return k_knobs[i];
    return NULL;
}

/* ---- guest byte order ----------------------------------------------------- */

/* A register in a fixture is guest byte order: byte k is the byte a `stqd` stores at offset k. The
 * conversion is spu_ls_write128's, word by word, written out so it does not depend on the host's
 * u128 layout (spu_xdiff.h, REG0). */
static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static void gpr_from_guest(u128* r, const uint8_t* in)
{
    for (int i = 0; i < 4; i++) r->_u32[i] = be32(in + 4 * i);
}

static void gpr_to_guest(const u128* r, uint8_t* out)
{
    for (int i = 0; i < 4; i++) {
        const uint32_t w = r->_u32[i];
        out[4 * i]     = (uint8_t)(w >> 24);
        out[4 * i + 1] = (uint8_t)(w >> 16);
        out[4 * i + 2] = (uint8_t)(w >> 8);
        out[4 * i + 3] = (uint8_t)w;
    }
}

/* [off, off + n) inside [0, limit), in 64-bit arithmetic that cannot wrap. */
static int span_ok(uint64_t off, uint64_t n, uint64_t limit)
{
    return off <= limit && n <= limit - off;
}

/* Index of the first differing byte, or n. */
static uint32_t first_diff(const uint8_t* a, const uint8_t* b, uint32_t n)
{
    uint32_t i = 0;
    while (i < n && a[i] == b[i]) i++;
    return i;
}

/* The 16 bytes of the verdict record's want/got: `avail` bytes of `src`, zero beyond. */
static void copy16(uint8_t* dst, const uint8_t* src, uint32_t avail)
{
    memset(dst, 0, 16);
    if (src && avail) memcpy(dst, src, avail < 16u ? avail : 16u);
}

/* ---- the replay state ----------------------------------------------------- */

struct poison_run;

typedef struct replay {
    const spu_xdiff_fixture* f;
    spu_xdiff_verdict_record* v;
    spu_context* ctx;    /* the shadow's context, for the stop diversion */
    uint32_t cursor;     /* the next transcript record */
    int      failed;     /* the serve table filled the verdict and ended the run */
    int      host_fault; /* a named stop was raised inside the subject and diverted */
    uint64_t budget;     /* T0 instruction budget; 0: none */
    uint64_t spent;
    int      over_budget;
    struct poison_run* poison;   /* the poison run's state, NULL for a plain replay */
} replay;

struct spu_xdiff_shadow {
    spu_context        ctx;
    replay             rep;
    spu_observer       obs;
    spu_effect_port    port;
};

/* The first failure: fills the verdict and ends the run. The subject is what it issued at
 * `index` (NULL: nothing); `want`/`got` are the bytes from `where` on (NULL: none). */
static SPU_PORT_NORETURN void fail(replay* r, spu_context* ctx, uint32_t reason, uint32_t index,
                                   const spu_xdiff_effect* subject, uint32_t where, const uint8_t* want,
                                   uint32_t want_avail, const uint8_t* got, uint32_t got_avail)
{
    spu_xdiff_verdict_record* v = r->v;
    v->verdict = spu_xdiff_reason_verdict(reason);
    v->reason = reason;
    v->effect_index = index;
    if (subject) v->subject = *subject;
    v->where = where;
    v->subject_pc = ctx->pc;
    copy16(v->want, want, want_avail);
    copy16(v->got, got, got_avail);
    r->failed = 1;
    spu_port_serve_halt(ctx, spu_xdiff_reason_id(reason));
}

static spu_xdiff_effect subject_effect(const spu_context* ctx, uint32_t kind, uint32_t channel, uint32_t value)
{
    spu_xdiff_effect s;
    memset(&s, 0, sizeof s);
    s.kind = (uint8_t)kind;
    s.channel = (uint8_t)channel;
    s.pc = ctx->pc;
    s.value = value;
    s.blob = SPU_XDIFF_NO_BLOB;
    return s;
}

/* The payload of record `e`, or NULL when it holds none or its span lies outside BLOB. */
static const uint8_t* payload(const replay* r, const spu_xdiff_effect* e)
{
    if (e->blob == SPU_XDIFF_NO_BLOB || !span_ok(e->blob, e->size, r->f->blob_bytes)) return NULL;
    return r->f->blob + e->blob;
}

/* A record this build cannot replay although it decoded (a hand-built fixture: the decoder refuses
 * these, section 7.8). Fails closed as a format error, never as a difference of the subject. */
static SPU_PORT_NORETURN void bad_record(replay* r, spu_context* ctx, uint32_t index, const spu_xdiff_effect* sub)
{
    fail(r, ctx, SPU_XDIFF_R_FIXTURE_FORMAT, index, sub, 0, NULL, 0, NULL, 0);
}

/* Takes the next record for an effect of `sub`'s kind (and channel, except for MFC_CMD whose channel
 * is the command's). Past the end: effect-extra; another kind or channel: effect-kind. */
static const spu_xdiff_effect* expect(replay* r, spu_context* ctx, const spu_xdiff_effect* sub)
{
    const spu_xdiff_fixture* f = r->f;
    if (r->cursor >= f->effect_count)
        fail(r, ctx, SPU_XDIFF_R_EFFECT_EXTRA, f->effect_count, sub, 0, NULL, 0, NULL, 0);
    const spu_xdiff_effect* e = &f->effects[r->cursor];
    if (e->kind != sub->kind || e->channel != sub->channel)
        fail(r, ctx, SPU_XDIFF_R_EFFECT_KIND, r->cursor, sub, 0, NULL, 0, NULL, 0);
    r->cursor++;
    return e;
}

/* The XFER records after the current one: to local store the recorded bytes arrive, from local store
 * the subject's bytes are compared with the recorded ones, a zeroed EA needs nothing (section 8.1).
 * `parent` is what the subject issued; it is the subject of a payload failure, which names the
 * XFER's own index. */
static void apply_xfers(replay* r, spu_context* ctx, const spu_xdiff_effect* parent)
{
    const spu_xdiff_fixture* f = r->f;
    while (r->cursor < f->effect_count && f->effects[r->cursor].kind == SPU_FX_XFER) {
        const uint32_t idx = r->cursor;
        const spu_xdiff_effect* x = &f->effects[idx];
        r->cursor++;
        if (x->dir == SPU_XFER_ZERO_EA) continue;
        if ((x->dir != SPU_XFER_TO_LS && x->dir != SPU_XFER_FROM_LS) || !span_ok(x->lsa, x->size, SPU_LS_SIZE))
            bad_record(r, ctx, idx, parent);
        const uint8_t* p = payload(r, x);
        if (x->size && !p) bad_record(r, ctx, idx, parent);
        if (!x->size) continue;
        if (x->dir == SPU_XFER_TO_LS) {
            memcpy(ctx->ls + x->lsa, p, x->size);
        } else {
            const uint8_t* got = ctx->ls + x->lsa;
            const uint32_t d = first_diff(p, got, x->size);
            if (d < x->size)
                fail(r, ctx, SPU_XDIFF_R_EFFECT_PAYLOAD, idx, parent, d, p + d, x->size - d, got + d, x->size - d);
        }
    }
}

/* `bytes` at `lsa` in the subject's local store against the recorded payload of `e`; a difference is
 * effect-request (a list is part of the request, section 6), `where` its first differing byte. */
static void check_ls_bytes(replay* r, spu_context* ctx, uint32_t index, const spu_xdiff_effect* e,
                           uint32_t lsa, const spu_xdiff_effect* sub)
{
    const uint8_t* want = payload(r, e);
    if (!want || !span_ok(lsa, e->size, SPU_LS_SIZE)) bad_record(r, ctx, index, sub);
    const uint8_t* got = ctx->ls + lsa;
    const uint32_t d = first_diff(want, got, e->size);
    if (d < e->size) fail(r, ctx, SPU_XDIFF_R_EFFECT_REQUEST, index, sub, d, want + d, e->size - d, got + d, e->size - d);
}

static void sv_mfc_cmd(replay* r, spu_context* ctx, uint32_t cmd)
{
    spu_xdiff_effect sub = subject_effect(ctx, SPU_FX_MFC_CMD, MFC_Cmd, cmd);
    sub.tag = (uint8_t)(ctx->mfc_tag & 31u);
    sub.ea = (uint64_t)ctx->mfc_eah << 32 | ctx->mfc_eal;
    sub.lsa = ctx->mfc_lsa;
    sub.size = ctx->mfc_size;
    const uint32_t idx = r->cursor;
    const spu_xdiff_effect* e = expect(r, ctx, &sub);
    /* The latches are raw 32-bit values of the context; the recorder stores them as the runtime holds them, and
     * only the local-store bits of the LSA mean anything (a wider guest value selects the same address). */
    if (e->value != cmd || e->ea != sub.ea || ((e->lsa ^ sub.lsa) & SPU_LS_MASK) != 0 || e->size != sub.size ||
        e->tag != sub.tag)
        fail(r, ctx, SPU_XDIFF_R_EFFECT_REQUEST, idx, &sub, SPU_XDIFF_NO_WHERE, NULL, 0, NULL, 0);
    /* A list command: the list as staged is recorded, and the subject's local store must hold the same one. */
    if (e->blob != SPU_XDIFF_NO_BLOB) check_ls_bytes(r, ctx, idx, e, (uint32_t)e->ea & SPU_LS_MASK, &sub);
    apply_xfers(r, ctx, &sub);
}

static void sv_wrch(void* user, spu_context* ctx, uint32_t channel, uint32_t value)
{
    replay* r = (replay*)user;
    if (channel == MFC_Cmd) { sv_mfc_cmd(r, ctx, value); return; }
    spu_xdiff_effect sub = subject_effect(ctx, SPU_FX_WRCH, channel, value);
    const uint32_t idx = r->cursor;
    const spu_xdiff_effect* e = expect(r, ctx, &sub);
    if (e->value != value) fail(r, ctx, SPU_XDIFF_R_EFFECT_REQUEST, idx, &sub, SPU_XDIFF_NO_WHERE, NULL, 0, NULL, 0);
    if (channel != MFC_WrListStallAck) return;
    /* The runtime reads a parked list's remaining elements again at the resume, after the guest's stall
     * handler may have rewritten them; the recorded bytes are what the authority's resume read. */
    if (e->size || e->blob != SPU_XDIFF_NO_BLOB) check_ls_bytes(r, ctx, idx, e, e->lsa, &sub);
    apply_xfers(r, ctx, &sub);
}

static uint32_t sv_rdch(void* user, spu_context* ctx, uint32_t channel)
{
    replay* r = (replay*)user;
    spu_xdiff_effect sub = subject_effect(ctx, SPU_FX_RDCH, channel, 0);
    return expect(r, ctx, &sub)->value;
}

static uint32_t sv_rchcnt(void* user, spu_context* ctx, uint32_t channel)
{
    replay* r = (replay*)user;
    spu_xdiff_effect sub = subject_effect(ctx, SPU_FX_RCHCNT, channel, 0);
    return expect(r, ctx, &sub)->value;
}

static const spu_effect_serve k_serve = { sv_wrch, sv_rdch, sv_rchcnt };

/* ---- the instruction observer: T0 budget (section 10) and the poison run ---------- */

#define POISON_STACK_CAP 65536u   /* shadow call stack: local store holds fewer frames than this */

typedef struct poison_frame {
    uint32_t fn;    /* index into the plan */
    uint32_t ret;   /* the link the call set */
    uint32_t sp;    /* $r1 at the call */
} poison_frame;

typedef struct poison_run {
    spu_xdiff_poison_fn* fn;      /* the claims being applied: the plan, or a narrowed copy of it */
    uint32_t             count;
    int                  apply;   /* 0: only track the calls (the unpoisoned run) */
    poison_frame*        stack;
    uint32_t             depth;
    int                  overflow;
    uint32_t             after_return;  /* the instruction about to run follows a branch on $r0 */
    uint32_t             returns, poisoned;
    uint32_t*            applied;       /* per function: returns it was poisoned at */
    int                  violated;      /* a frame claim failed: the stack pointer was not the one the plan assumes */
    uint32_t             violation_fn;
    uint32_t             runs;
    uint8_t              reg_touched[128];
    uint8_t*             ls_touched;    /* one bit per byte of local store that was poisoned */
} poison_run;

static uint32_t ls_word(const spu_context* ctx, uint32_t a)
{
    return be32(ctx->ls + (a & (uint32_t)SPU_LS_MASK & ~3u));
}

/* The plan's function whose entry is `entry`, or -1. The plan is sorted by entry. */
static int64_t poison_find(const poison_run* p, uint32_t entry)
{
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (p->fn[mid].entry < entry) lo = mid + 1;
        else hi = mid;
    }
    return lo < p->count && p->fn[lo].entry == entry ? (int64_t)lo : -1;
}

static void poison_violate(poison_run* p, uint32_t fn)
{
    if (!p->violated) { p->violated = 1; p->violation_fn = fn; }
}

/* Overwrites what the plan calls dead for one return of `fr->fn`; `sp` is $r1 now. */
static void poison_apply(poison_run* p, spu_context* ctx, const poison_frame* fr, uint32_t sp)
{
    const spu_xdiff_poison_fn* fn = &p->fn[fr->fn];
    p->poisoned++;
    p->applied[fr->fn]++;
    for (uint32_t reg = 0; reg < 128; reg++) {
        if (reg == 1 || !((fn->dead[reg >> 6] >> (reg & 63)) & 1u)) continue;
        for (int i = 0; i < 4; i++) ctx->gpr[reg]._u32[i] = SPU_XDIFF_POISON_REG_WORD;
        p->reg_touched[reg] = 1;
    }
    if (!fn->stack_to) return;
    /* The frame is [entry sp - n, entry sp): a return that finds $r1 elsewhere, or a frame that does not fit
     * in local store, is not the function the plan describes. */
    if (sp != fr->sp || fn->stack_to > fr->sp || fr->sp > (uint32_t)SPU_LS_SIZE) {
        poison_violate(p, fr->fn);
        return;
    }
    for (uint32_t k = fn->stack_from; k <= fn->stack_to; k++) {
        const uint32_t at = fr->sp - k;
        ctx->ls[at] = (uint8_t)SPU_XDIFF_POISON_STACK_BYTE;
        p->ls_touched[at >> 3] |= (uint8_t)(1u << (at & 7u));
    }
}

/* The instruction at `pc` is the first one after a branch on $r0. It is a return of a tracked call when `pc`
 * is the link that call set: the newest frame matches on the link alone (the stack pointer is the plan's to
 * check), an older one only with the stack pointer it was called with, and the frames above it are dropped
 * as calls that never returned. */
static void poison_return(poison_run* p, spu_context* ctx, uint32_t pc)
{
    uint32_t k = p->depth;
    const uint32_t sp = ctx->gpr[1]._u32[0];
    if (!k) return;
    if (p->stack[k - 1].ret != pc) {
        for (k = k - 1; k > 0; k--)
            if (p->stack[k - 1].ret == pc && p->stack[k - 1].sp == sp) break;
        if (!k) return;
    }
    const poison_frame fr = p->stack[k - 1];
    p->depth = k - 1;
    p->returns++;
    if (p->apply) poison_apply(p, ctx, &fr, sp);
}

static void poison_step(poison_run* p, spu_context* ctx, uint32_t pc)
{
    pc &= (uint32_t)SPU_LS_MASK & ~3u;
    if (p->after_return) {
        p->after_return = 0;
        poison_return(p, ctx, pc);
    }
    const uint32_t w = ls_word(ctx, pc);
    const int32_t  rel = (int32_t)(int16_t)((w >> 7) & 0xFFFFu) * 4;
    uint32_t target = 0;
    int call = 0;
    if ((w >> 23) == 0x066u)      { target = (uint32_t)((int32_t)pc + rel); call = 1; }                 /* brsl */
    else if ((w >> 23) == 0x062u) { target = (uint32_t)rel; call = 1; }                                 /* brasl */
    else if ((w >> 21) == 0x1A9u) { target = ctx->gpr[(w >> 7) & 0x7Fu]._u32[0]; call = 1; }           /* bisl */
    if (call) {
        const int64_t fn = poison_find(p, target & (uint32_t)SPU_LS_MASK & ~3u);
        if (fn >= 0) {
            if (p->depth >= POISON_STACK_CAP) {
                p->overflow = 1;
            } else {
                poison_frame* fr = &p->stack[p->depth++];
                fr->fn = (uint32_t)fn;
                fr->ret = (pc + 4u) & (uint32_t)SPU_LS_MASK;
                fr->sp = ctx->gpr[1]._u32[0];
            }
        }
        return;
    }
    /* bi, biz, binz, bihz, bihnz with $r0 as the target (RA = 0) */
    const uint32_t op11 = w >> 21;
    if ((op11 == 0x1A8u || (op11 >= 0x128u && op11 <= 0x12Bu)) && ((w >> 7) & 0x7Fu) == 0) p->after_return = 1;
}

/* The state a run leaves is the fixture's, except where poison nobody read is still sitting: those
 * registers and bytes still hold the pattern, and take the fixture's value for the comparison. State the
 * poison reached anywhere else has no such value and differs. */
static void poison_unmask(const poison_run* p, spu_context* ctx, const spu_xdiff_fixture* f)
{
    for (uint32_t reg = 0; reg < 128; reg++) {
        if (!p->reg_touched[reg]) continue;
        const u128* g = &ctx->gpr[reg];
        if (g->_u32[0] == SPU_XDIFF_POISON_REG_WORD && g->_u32[1] == SPU_XDIFF_POISON_REG_WORD &&
            g->_u32[2] == SPU_XDIFF_POISON_REG_WORD && g->_u32[3] == SPU_XDIFF_POISON_REG_WORD)
            gpr_from_guest(&ctx->gpr[reg], f->reg1 + 16u * reg);
    }
    for (uint32_t at = 0; at < SPU_LS_SIZE; at++)
        if (((p->ls_touched[at >> 3] >> (at & 7u)) & 1u) && ctx->ls[at] == SPU_XDIFF_POISON_STACK_BYTE)
            ctx->ls[at] = f->ls1[at];
}

/* on_insn runs before every interpreted instruction. Yielding at the budget stops the outermost
 * interpreter run with nothing consumed; the driver reads `over_budget` after the run. */
static int observe_insn(void* user, spu_context* ctx, uint32_t pc)
{
    replay* r = (replay*)user;
    if (r->budget) {
        if (r->spent >= r->budget) { r->over_budget = 1; return 1; }
        r->spent++;
    }
    if (r->poison) poison_step(r->poison, ctx, pc);
    return 0;
}

static void entry_interp(spu_context* ctx)
{
    spu_interp_run(ctx, ctx->pc);
}

/* ---- a named stop inside the subject (section 10) ----------------------------- */

/* The subject is code under test: a lifted body or the interpreter reaching ps3_stop is a fault of the
 * subject, not a reason to end a process that has a game in it (the audit thread runs in a title's process).
 * Installed on the calling thread for the run only, so a stop raised anywhere else is untouched. It leaves
 * through the halt landing pad the run is under (spu_port_serve_halt), which also restores the interpreter's
 * nesting depth; if no pad were armed it aborts there, and so fails closed. */
static void stop_diverted(void* user, ps3_stop_kind kind, const char* what)
{
    replay* r = (replay*)user;
    r->host_fault = 1;
    fprintf(stderr, "%s[spu-xdiff] the subject raised a named stop (kind=%s: %s); it is void / host-fault for this run, "
                    "not a stop of the process\n",
            spu_diag_tag(), ps3_stop_kind_id(kind), what ? what : "");
    spu_port_serve_halt(r->ctx, "a named stop was raised inside the subject");
}

/* ---- the shadow ------------------------------------------------------------- */

spu_xdiff_shadow* spu_xdiff_shadow_new(void)
{
    return (spu_xdiff_shadow*)calloc(1, sizeof(spu_xdiff_shadow));
}

void spu_xdiff_shadow_free(spu_xdiff_shadow* s)
{
    free(s);
}

static uint32_t verdict_of(spu_xdiff_verdict_record* out, uint32_t reason)
{
    out->verdict = spu_xdiff_reason_verdict(reason);
    out->reason = reason;
    return out->verdict;
}

/* The end state against the fixture's (section 8.1): the end class (and the stop code of a stop),
 * every register, then all of local store; the first difference only. */
static uint32_t compare_end(const spu_xdiff_fixture* f, const spu_context* ctx, spu_xdiff_verdict_record* out)
{
    const spu_job_end e = spu_job_classify_end(ctx);
    if (e.end_class != f->end.end_class || (e.end_class == SPU_JOB_END_STOP && e.stop_code != f->end.stop_code)) {
        /* want/got hold {end_class, stop_code} as two little-endian words; `where` has no position. */
        out->where = SPU_XDIFF_NO_WHERE;
        const uint32_t want[2] = { f->end.end_class, f->end.stop_code }, got[2] = { e.end_class, e.stop_code };
        memset(out->want, 0, sizeof out->want);
        memset(out->got, 0, sizeof out->got);
        memcpy(out->want, want, sizeof want);
        memcpy(out->got, got, sizeof got);
        return verdict_of(out, SPU_XDIFF_R_END_CLASS);
    }
    for (uint32_t r = 0; r < 128; r++) {
        uint8_t g[16];
        gpr_to_guest(&ctx->gpr[r], g);
        if (memcmp(g, f->reg1 + 16u * r, 16) != 0) {
            out->where = r;
            memcpy(out->want, f->reg1 + 16u * r, 16);
            memcpy(out->got, g, 16);
            return verdict_of(out, SPU_XDIFF_R_END_REGS);
        }
    }
    const uint32_t d = first_diff(f->ls1, ctx->ls, SPU_LS_SIZE);
    if (d < SPU_LS_SIZE) {
        out->where = d;
        copy16(out->want, f->ls1 + d, SPU_LS_SIZE - d);
        copy16(out->got, ctx->ls + d, SPU_LS_SIZE - d);
        return verdict_of(out, SPU_XDIFF_R_END_LS);
    }
    return verdict_of(out, SPU_XDIFF_R_NONE);
}

/* The subject's body id. A lifted subject's lift identity is the one its registration declares, which is
 * the image's inventory (the capture's rule, xdiff_capture.cpp): no inventory leaves code_end and
 * lift_options 0, the documented "none" (spu_xdiff.h), and is also what T0 and T3 report. */
static void set_subject_body(spu_xdiff_body_id* body, uint32_t tier, const spu_xdiff_fixture* f)
{
    body->tier = (uint8_t)tier;
    if ((tier & SPU_TIER_MASK) != (uint32_t)SPU_TIER_LIFTED) return;
    body->lifter_abi = SPU_RUNTIME_LIFTER_ABI;   /* every registered lift passed the ABI gate */
    spu_workload_inventory inv;
    if (spu_workload_find_inventory(f->identity.image_fingerprint, &inv)) {
        body->code_end = inv.code_end;
        body->lift_options = inv.lift_options;
    }
}

/* One replay. `poison` is the poison run's state, NULL for a plain replay (spu_xdiff_replay). */
static uint32_t replay_core(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt, spu_xdiff_shadow* s,
                            spu_xdiff_verdict_record* out, poison_run* poison)
{
    memset(out, 0, sizeof *out);
    out->effect_index = SPU_XDIFF_NO_EFFECT;
    set_subject_body(&out->subject_body, opt->subject_tier, f);

    /* Refused knobs are the caller's to report (spu_job_replay_main exits 1): no verdict. */
    if (spu_xdiff_refused_knob() != NULL) {
        out->verdict = (uint32_t)SPU_XDIFF_VERDICT_INVALID;
        out->reason = (uint32_t)SPU_XDIFF_R_INVALID;
        return out->verdict;
    }
    /* The float mode is latched once per process; a fixture does not carry it into the run (8.1). */
    if ((f->identity.float_rz != 0) != (spu_float_rz_mode() != 0))
        return verdict_of(out, SPU_XDIFF_R_FIXTURE_IDENTITY);
    /* A transcript recorded under another effect vocabulary reads differently (spu_effect_port.h), and the shadow's
     * classifier knows one planted link: a fixture of another loader layout would classify a return as something else. */
    if (f->identity.effect_port_abi != SPU_EFFECT_PORT_ABI || f->unit.return_ls != SPU_JOB_RETURN_LINK_LS)
        return verdict_of(out, SPU_XDIFF_R_FIXTURE_IDENTITY);
    /* Another census is reported, not refused: the replay stays sound while the port ABI and the carried members
     * are unchanged (section 9), and a difference of members is the census test's to catch. */
    if (f->identity.context_census != spu_xdiff_context_census() && !(poison && poison->runs > 1))
        PS3_LOG(PS3_LOG_INFO, "spu-xdiff", "the fixture was recorded under spu_context census 0x%08X; this build's is 0x%08X\n", (unsigned)f->identity.context_census, (unsigned)spu_xdiff_context_census());
    const uint32_t match = spu_xdiff_match_for(f->identity.authority.tier, opt->subject_tier);
    out->match = match;
    /* Contract matching (a pair with native code) is not built here: no pair of this runner has it, and a
     * same-program verdict for it would be a false answer. */
    if (match != (uint32_t)SPU_XDIFF_MATCH_PROGRAM) return verdict_of(out, SPU_XDIFF_R_NOT_COMPARABLE);

    void (*entry)(spu_context*) = entry_interp;
    int image_id = -1;   /* T0: -1 keeps the interpreter from rejoining a lifted function (section 9) */
    int twin = 0;
    if ((opt->subject_tier & SPU_TIER_MASK) == (uint32_t)SPU_TIER_LIFTED) {
        entry = spu_workload_find_img(f->identity.image_fingerprint, &image_id);
        if (!entry) return verdict_of(out, SPU_XDIFF_R_NO_BODY);
        /* A budget is a bound on the loops of the subject, and only the budgeted twin carries one (section 10).
         * The twin has its own image id, so its indirect branches stay inside it. An image without a twin runs its
         * production body unbounded, as before: the offline runner's timeout is its bound. */
        if (opt->budget) {
            int twin_id = 0;
            spu_lifted_entry_fn t = spu_workload_find_twin(f->identity.image_fingerprint, &twin_id);
            if (t) { entry = t; image_id = twin_id; twin = 1; }
        }
    }

    /* The shadow is built, never copied: spu_context_init, then what a job's entry state sets (section 7.4). */
    spu_context* ctx = &s->ctx;
    spu_context_init(ctx, 0);
    for (uint32_t i = 0; i < 128; i++) gpr_from_guest(&ctx->gpr[i], f->reg0 + 16u * i);
    memcpy(ctx->ls, f->ls0, SPU_LS_SIZE);
    ctx->pc = f->unit.entry_pc;
    ctx->job_rt_ls = f->unit.job_rt_ls;
    ctx->job_rt_ls2 = f->unit.job_rt_ls2;
    ctx->image_id = image_id;

    replay* r = &s->rep;
    memset(r, 0, sizeof *r);
    r->f = f;
    r->v = out;
    r->ctx = ctx;
    s->port.serve = &k_serve;
    s->port.serve_user = r;
    s->port.tap = NULL;
    s->port.tap_user = NULL;
    spu_port_set(ctx, &s->port);
    if (twin) {
        ctx->budget = opt->budget;   /* the twin counts it down at every transfer that can close a loop */
        out->budget = opt->budget;
    } else if (image_id < 0 && (opt->budget || poison)) {
        r->budget = opt->budget;
        r->poison = poison;
        if (opt->budget) out->budget = opt->budget;
        s->obs.user = r;
        s->obs.on_insn = observe_insn;
        s->obs.on_mfc = NULL;
        s->obs.on_atomic = NULL;
        s->obs.on_channel = NULL;
        spu_observer_set(ctx, &s->obs);
    }

    ps3_stop_divert_begin(stop_diverted, r);
    const int halted = spu_run_with_halt(entry, ctx);
    ps3_stop_divert_end();
    /* The twin's own counter is the context's: what is left of the budget, and the unwind's status. */
    if (twin) {
        out->budget_spent = opt->budget - ctx->budget;
        if (halted && ctx->status == SPU_STATUS_BUDGET_EXHAUSTED) r->over_budget = 1;
    } else {
        out->budget_spent = r->spent;
    }
    out->subject_pc = ctx->pc;

    /* A served context that reached the runtime below the seam has broken the design's central claim
     * (section 9): a defect of the runtime, not a verdict about the subject. */
    if (ctx->coh_listed != 0 || spu_mfc_has_engine(ctx)) {
        PS3_LOG(PS3_LOG_INFO, "spu-xdiff", "shadow context %p reached the runtime below the effect port "
                        "(coh_listed=%d mfc_engine=%d) -- aborting\n", (void*)ctx, ctx->coh_listed, spu_mfc_has_engine(ctx));
        fflush(stderr);
        abort();
    }

    if (r->host_fault) return verdict_of(out, SPU_XDIFF_R_HOST_FAULT);   /* a stop inside the subject, diverted */
    if (r->failed) return out->verdict;   /* the serve table filled the record */
    if (r->over_budget) return verdict_of(out, SPU_XDIFF_R_BUDGET);
    if (ctx->lift_miss) return verdict_of(out, SPU_XDIFF_R_UNLIFTED_BRANCH);
    /* spu_depth_guard halts with no architectural status; host_depth is where it left it (spu_run_with_halt
     * clears it at entry), so a halt at the guard's limit is that exit and not an end state. */
    if (halted) {
        const uint32_t limit = SPU_RELAXED_LOAD(&g_spu_depth_guard_limit);
        if (limit && ctx->host_depth >= limit) return verdict_of(out, SPU_XDIFF_R_DEPTH_GUARD);
    }
    /* A served interpreter takes the 0xA70 taskset hook as a halt (spu_interp.c) and does not service `stop 0x110`:
     * the subject ended on something no port mediates. A stop 0x110 with records left is the authority's run going on
     * through a service this shadow does not model; with none left the end compare below decides. */
    if (ctx->status == SPU_STATUS_STOPPED_BY_HALT && ctx->stop_code == 0x1000u &&
        (ctx->pc & SPU_LS_MASK) == SPURS_TASKSET_PM_SYSCALL_LS && (ctx->image_id == XDIFF_TASKSET_HLE_IMAGE || ctx->policy_mode))
        return verdict_of(out, SPU_XDIFF_R_UNSUPPORTED_EFFECT);
    if (r->cursor < f->effect_count && ctx->status == SPU_STATUS_STOPPED_BY_STOP && ctx->stop_code == 0x110u)
        return verdict_of(out, SPU_XDIFF_R_UNSUPPORTED_EFFECT);
    if (r->cursor < f->effect_count) {
        out->effect_index = r->cursor;
        return verdict_of(out, SPU_XDIFF_R_EFFECT_MISSING);
    }
    if (poison && poison->apply) poison_unmask(poison, ctx, f);
    return compare_end(f, ctx, out);
}

uint32_t spu_xdiff_replay(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                          spu_xdiff_shadow* s, spu_xdiff_verdict_record* out)
{
    return replay_core(f, opt, s, out, NULL);
}

/* ---- the poison run ----------------------------------------------------------------- */

static void poison_free(poison_run* p)
{
    free(p->fn);
    free(p->applied);
    free(p->stack);
    free(p->ls_touched);
    memset(p, 0, sizeof *p);
}

static int poison_init(poison_run* p, const spu_xdiff_poison_plan* plan)
{
    const size_t n = plan->count ? plan->count : 1u;
    memset(p, 0, sizeof *p);
    p->count = plan->count;
    p->fn = (spu_xdiff_poison_fn*)malloc(n * sizeof *p->fn);
    p->applied = (uint32_t*)calloc(n, sizeof *p->applied);
    p->stack = (poison_frame*)malloc(POISON_STACK_CAP * sizeof *p->stack);
    p->ls_touched = (uint8_t*)calloc((size_t)SPU_LS_SIZE / 8u, 1);
    if (!p->fn || !p->applied || !p->stack || !p->ls_touched) {
        poison_free(p);
        return -1;
    }
    if (plan->count) memcpy(p->fn, plan->fn, plan->count * sizeof *p->fn);
    return 0;
}

static void poison_reset(poison_run* p, int apply)
{
    p->apply = apply;
    p->depth = 0;
    p->overflow = 0;
    p->after_return = 0;
    p->returns = p->poisoned = 0;
    p->violated = 0;
    p->violation_fn = 0;
    p->runs++;
    memset(p->applied, 0, (p->count ? p->count : 1u) * sizeof *p->applied);
    memset(p->reg_touched, 0, sizeof p->reg_touched);
    memset(p->ls_touched, 0, (size_t)SPU_LS_SIZE / 8u);
}

static void blank_record(spu_xdiff_verdict_record* out, const spu_xdiff_replay_options* opt,
                         const spu_xdiff_fixture* f)
{
    memset(out, 0, sizeof *out);
    out->effect_index = SPU_XDIFF_NO_EFFECT;
    set_subject_body(&out->subject_body, opt->subject_tier, f);
}

/* One replay of the poison run. The shadow call stack holds every call of a listed function until it returns:
 * more of them than local store has room for frames is a run this bound cannot follow, not an `equal`. */
static uint32_t poison_once(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt, spu_xdiff_shadow* s,
                            spu_xdiff_verdict_record* out, poison_run* p, int apply)
{
    poison_reset(p, apply);
    const uint32_t v = replay_core(f, opt, s, out, p);
    if (v == (uint32_t)SPU_XDIFF_EQUAL && p->overflow) {
        PS3_LOG(PS3_LOG_INFO, "spu-xdiff", "the poison run tracks at most %u open calls of listed functions\n", POISON_STACK_CAP);
        return verdict_of(out, SPU_XDIFF_R_CAPTURE_LIMIT);
    }
    return v;
}

typedef struct attribution {
    const spu_xdiff_fixture*        f;
    const spu_xdiff_replay_options* opt;
    spu_xdiff_shadow*               s;
    poison_run*                     p;
    spu_xdiff_verdict_record        best;   /* the narrowest failing run so far */
    uint32_t                        underlying;
} attribution;

/* Replays with the claims now in `a->p->fn`. A run fails when it differs from the fixture or a frame claim
 * did not hold; the narrowest failing run's record is kept. */
static int attribution_try(attribution* a)
{
    spu_xdiff_verdict_record rec;
    const uint32_t v = poison_once(a->f, a->opt, a->s, &rec, a->p, 1);
    if (v == (uint32_t)SPU_XDIFF_EQUAL && !a->p->violated) return 0;
    a->best = rec;
    a->underlying = v == (uint32_t)SPU_XDIFF_EQUAL ? (uint32_t)SPU_XDIFF_R_NONE : rec.reason;
    return 1;
}

static void poison_clear(spu_xdiff_poison_fn* fn)
{
    fn->stack_from = fn->stack_to = 0;
    fn->dead[0] = fn->dead[1] = 0;
}

/* Finds what a failing poison run is about: the first listed function (by entry) whose claims, applied alone,
 * fail; then the lowest dead register of it that fails alone; then, failing that, the frame, narrowed to the
 * lowest byte that matters. Each step is one more replay of the fixture. A failure that needs several functions,
 * registers or bytes together stays with the wider run's record. */
static void poison_attribute(attribution* a, const spu_xdiff_poison_plan* plan, const uint32_t* applied,
                             spu_xdiff_poison_report* report)
{
    poison_run* p = a->p;
    int culprit = -1;
    for (uint32_t i = 0; i < p->count; i++) poison_clear(&p->fn[i]);
    for (uint32_t i = 0; i < p->count && culprit < 0; i++) {
        if (!applied[i]) continue;
        p->fn[i] = plan->fn[i];
        if (attribution_try(a)) culprit = (int)i;
        else poison_clear(&p->fn[i]);
    }
    if (culprit < 0) return;
    const spu_xdiff_poison_fn orig = plan->fn[culprit];
    spu_xdiff_poison_fn* fn = &p->fn[culprit];
    report->has_function = 1;
    report->function = orig.entry;

    for (uint32_t reg = 0; reg < 128; reg++) {
        if (!((orig.dead[reg >> 6] >> (reg & 63)) & 1u)) continue;
        poison_clear(fn);
        fn->dead[reg >> 6] = (uint64_t)1 << (reg & 63);
        if (attribution_try(a)) {
            report->kind = SPU_XDIFF_POISON_REGISTER;
            report->reg = reg;
            return;
        }
    }
    if (!orig.stack_to) return;
    poison_clear(fn);
    fn->stack_from = orig.stack_from;
    fn->stack_to = orig.stack_to;
    if (!attribution_try(a)) return;
    report->kind = SPU_XDIFF_POISON_STACK;
    if (p->violated) return;   /* the claim failed on the stack pointer, not on a byte */
    /* Poison {k >= m} is a window that shrinks from the low-address end as m grows; the largest m that still
     * fails is the lowest byte that matters. */
    uint32_t lo = orig.stack_from, hi = orig.stack_to;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1u) / 2u;
        fn->stack_from = mid;
        if (attribution_try(a)) lo = mid;
        else hi = mid - 1u;
    }
    report->below_sp = lo;
    fn->stack_from = fn->stack_to = lo;
    report->exact = (uint32_t)attribution_try(a);
}

uint32_t spu_xdiff_replay_poison(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                                 const spu_xdiff_poison_plan* plan, spu_xdiff_shadow* s,
                                 spu_xdiff_verdict_record* out, spu_xdiff_poison_report* report)
{
    memset(report, 0, sizeof *report);
    if (opt->subject_tier != (uint32_t)SPU_TIER_INTERP) {
        blank_record(out, opt, f);
        return verdict_of(out, SPU_XDIFF_R_NOT_COMPARABLE);
    }
    if (plan->has_fingerprint && plan->image_fingerprint != f->identity.image_fingerprint) {
        blank_record(out, opt, f);
        return verdict_of(out, SPU_XDIFF_R_FIXTURE_IDENTITY);
    }
    poison_run p;
    if (poison_init(&p, plan) != 0) {
        PS3_LOG(PS3_LOG_INFO, "spu-xdiff", "out of memory for the poison run\n");
        blank_record(out, opt, f);
        return verdict_of(out, SPU_XDIFF_R_HOST_FAULT);
    }

    uint32_t v = poison_once(f, opt, s, out, &p, 0);
    report->returns = p.returns;
    if (v != (uint32_t)SPU_XDIFF_EQUAL) {   /* nothing to compare the poisoned run with */
        poison_free(&p);
        return v;
    }

    spu_xdiff_verdict_record second;
    v = poison_once(f, opt, s, &second, &p, 1);
    report->ran = 1;
    report->poisoned = p.poisoned;
    if (v == (uint32_t)SPU_XDIFF_EQUAL && !p.violated) {
        *out = second;
        poison_free(&p);
        return v;
    }

    attribution a;
    a.f = f;
    a.opt = opt;
    a.s = s;
    a.p = &p;
    a.best = second;
    a.underlying = v == (uint32_t)SPU_XDIFF_EQUAL ? (uint32_t)SPU_XDIFF_R_NONE : second.reason;
    uint32_t* applied = (uint32_t*)malloc((plan->count ? plan->count : 1u) * sizeof *applied);
    if (applied) {
        memcpy(applied, p.applied, (plan->count ? plan->count : 1u) * sizeof *applied);
        poison_attribute(&a, plan, applied, report);
        free(applied);
    }
    report->underlying = a.underlying;
    *out = a.best;
    out->verdict = spu_xdiff_reason_verdict(SPU_XDIFF_R_POISON);
    out->reason = SPU_XDIFF_R_POISON;
    poison_free(&p);
    return out->verdict;
}

/* ---- the liveness plan ------------------------------------------------------------------ */

/* A reader for the members of `ps3recomp.spu-liveness/1` the runner uses. It is a JSON reader, not a
 * validator of the whole document: members it does not know are skipped by shape. */
typedef struct jp {
    const char* base;
    const char* p;
    const char* end;
    char*       err;
    size_t      err_bytes;
} jp;

static int jp_fail(jp* j, const char* what)
{
    if (j->err && j->err_bytes)
        snprintf(j->err, j->err_bytes, "liveness plan: %s (byte %lu)", what, (unsigned long)(j->p - j->base));
    return -1;
}

static int jp_peek(jp* j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

static int jp_eat(jp* j, char c)
{
    if (jp_peek(j) == (unsigned char)c) { j->p++; return 0; }
    return jp_fail(j, c == ':' ? "expected ':'" : c == '{' ? "expected '{'" : c == '[' ? "expected '['" : "unexpected text");
}

static int jp_hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A string into `buf` (NUL-terminated, `cap` bytes; NULL: discarded). One that does not fit becomes "", which
 * no key or value this reader compares against equals. */
static int jp_str(jp* j, char* buf, size_t cap)
{
    size_t n = 0;
    int cut = 0;
    if (jp_eat(j, '"') != 0) return jp_fail(j, "expected a string");
    for (;;) {
        if (j->p >= j->end) return jp_fail(j, "unterminated string");
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') break;
        if (c < 0x20) return jp_fail(j, "control character in a string");
        if (c == '\\') {
            if (j->p >= j->end) return jp_fail(j, "unterminated string");
            c = (unsigned char)*j->p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    const int d = j->p < j->end ? jp_hex_digit((unsigned char)*j->p) : -1;
                    if (d < 0) return jp_fail(j, "bad \\u escape");
                    v = v << 4 | (unsigned)d;
                    j->p++;
                }
                c = v < 0x80u ? (unsigned char)v : (unsigned char)'?';
                break;
            }
            default: return jp_fail(j, "bad escape");
            }
        }
        if (buf) {
            if (n + 1 < cap) buf[n++] = (char)c;
            else cut = 1;
        }
    }
    if (buf && cap) {
        if (cut) n = 0;
        buf[n] = 0;
    }
    return 0;
}

/* A non-negative integer up to `max`: digits only, no sign, fraction, exponent or leading zero. */
static int jp_uint(jp* j, uint32_t* out, uint32_t max)
{
    uint64_t v = 0;
    if (jp_peek(j) < '0' || jp_peek(j) > '9') return jp_fail(j, "expected a non-negative integer");
    if (j->p[0] == '0' && j->p + 1 < j->end && j->p[1] >= '0' && j->p[1] <= '9') return jp_fail(j, "leading zero");
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        v = v * 10u + (uint64_t)(*j->p - '0');
        if (v > max) return jp_fail(j, "number out of range");
        j->p++;
    }
    if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) return jp_fail(j, "expected an integer");
    *out = (uint32_t)v;
    return 0;
}

/* A whole-string number in base 0 (the tool writes addresses as "0x..." strings), or a plain integer. */
static int jp_address(jp* j, uint64_t* out)
{
    if (jp_peek(j) == '"') {
        char text[64], *end = NULL;
        if (jp_str(j, text, sizeof text) != 0) return -1;
        if (!(text[0] >= '0' && text[0] <= '9')) return jp_fail(j, "expected a number in a string");
        errno = 0;
        const unsigned long long v = strtoull(text, &end, 0);
        if (errno != 0 || !end || *end != 0) return jp_fail(j, "expected a number in a string");
        *out = (uint64_t)v;
        return 0;
    }
    uint32_t v = 0;
    if (jp_uint(j, &v, 0xFFFFFFFFu) != 0) return -1;
    *out = v;
    return 0;
}

static int jp_literal(jp* j, const char* word)
{
    const size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return jp_fail(j, "unexpected text");
    j->p += n;
    return 0;
}

/* The separator after a member or element: 0 to go on, 1 at the closer `close`, -1 on an error. */
static int jp_next(jp* j, int close)
{
    const int c = jp_peek(j);
    if (c == ',') { j->p++; return 0; }
    if (c == close) { j->p++; return 1; }
    return jp_fail(j, close == '}' ? "expected ',' or '}'" : "expected ',' or ']'");
}

static int jp_skip(jp* j, int depth)
{
    int c = jp_peek(j);
    if (depth > 64) return jp_fail(j, "nested too deeply");
    if (c == '"') return jp_str(j, NULL, 0);
    if (c == '{' || c == '[') {
        const int close = c == '{' ? '}' : ']';
        j->p++;
        if (jp_peek(j) == close) { j->p++; return 0; }
        for (;;) {
            int more;
            if (close == '}' && (jp_str(j, NULL, 0) != 0 || jp_eat(j, ':') != 0)) return -1;
            if (jp_skip(j, depth + 1) != 0) return -1;
            more = jp_next(j, close);
            if (more < 0) return -1;
            if (more) return 0;
        }
    }
    if (c == 't') return jp_literal(j, "true");
    if (c == 'f') return jp_literal(j, "false");
    if (c == 'n') return jp_literal(j, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char* start = j->p;
        while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '-' || *j->p == '+' || *j->p == '.' ||
                                 *j->p == 'e' || *j->p == 'E'))
            j->p++;
        return j->p > start ? 0 : jp_fail(j, "unexpected text");
    }
    return jp_fail(j, "unexpected text");
}

static int plan_dead_regs(jp* j, spu_xdiff_poison_fn* fn)
{
    if (jp_eat(j, '[') != 0) return -1;
    if (jp_peek(j) == ']') { j->p++; return 0; }
    for (;;) {
        uint32_t reg = 0;
        int more;
        if (jp_uint(j, &reg, 127u) != 0) return -1;
        if (reg == 1u) return jp_fail(j, "$r1 is listed as dead: the stack pointer is the caller's");
        fn->dead[reg >> 6] |= (uint64_t)1 << (reg & 63u);
        more = jp_next(j, ']');
        if (more < 0) return -1;
        if (more) return 0;
    }
}

static int plan_dead_stack(jp* j, spu_xdiff_poison_fn* fn)
{
    if (jp_peek(j) == 'n') return jp_literal(j, "null");
    if (jp_eat(j, '{') != 0) return -1;
    if (jp_peek(j) == '}') { j->p++; return jp_fail(j, "dead_stack lacks below_sp"); }
    uint32_t below = 0;
    int seen = 0;
    for (;;) {
        char key[64];
        int more;
        if (jp_str(j, key, sizeof key) != 0 || jp_eat(j, ':') != 0) return -1;
        if (!strcmp(key, "below_sp")) {
            if (jp_uint(j, &below, (uint32_t)SPU_LS_SIZE) != 0) return -1;
            seen = 1;
        } else if (jp_skip(j, 0) != 0) {
            return -1;
        }
        more = jp_next(j, '}');
        if (more < 0) return -1;
        if (more) break;
    }
    if (!seen) return jp_fail(j, "dead_stack lacks below_sp");
    fn->stack_from = below ? 1u : 0u;
    fn->stack_to = below;
    return 0;
}

static int plan_function(jp* j, spu_xdiff_poison_fn* fn)
{
    int have_entry = 0;
    memset(fn, 0, sizeof *fn);
    if (jp_eat(j, '{') != 0) return -1;
    if (jp_peek(j) == '}') { j->p++; return jp_fail(j, "a function lacks its entry"); }
    for (;;) {
        char key[64];
        int more;
        if (jp_str(j, key, sizeof key) != 0 || jp_eat(j, ':') != 0) return -1;
        if (!strcmp(key, "entry")) {
            uint64_t v = 0;
            if (jp_address(j, &v) != 0) return -1;
            if (v >= (uint64_t)SPU_LS_SIZE || (v & 3u)) return jp_fail(j, "an entry is not a word address in local store");
            fn->entry = (uint32_t)v;
            have_entry = 1;
        } else if (!strcmp(key, "dead_regs")) {
            if (plan_dead_regs(j, fn) != 0) return -1;
        } else if (!strcmp(key, "dead_stack")) {
            if (plan_dead_stack(j, fn) != 0) return -1;
        } else if (jp_skip(j, 0) != 0) {
            return -1;
        }
        more = jp_next(j, '}');
        if (more < 0) return -1;
        if (more) break;
    }
    return have_entry ? 0 : jp_fail(j, "a function lacks its entry");
}

static int plan_functions(jp* j, spu_xdiff_poison_plan* plan)
{
    uint32_t cap = 0;
    if (jp_eat(j, '[') != 0) return -1;
    if (jp_peek(j) == ']') { j->p++; return 0; }
    for (;;) {
        int more;
        if (plan->count == cap) {
            uint32_t ncap = cap ? cap * 2u : 16u;
            spu_xdiff_poison_fn* grown;
            if (plan->count >= SPU_XDIFF_POISON_MAX_FUNCTIONS) return jp_fail(j, "too many functions");
            grown = (spu_xdiff_poison_fn*)realloc(plan->fn, ncap * sizeof *grown);
            if (!grown) return jp_fail(j, "out of memory");
            plan->fn = grown;
            cap = ncap;
        }
        if (plan_function(j, &plan->fn[plan->count]) != 0) return -1;
        plan->count++;
        more = jp_next(j, ']');
        if (more < 0) return -1;
        if (more) return 0;
    }
}

static int plan_image(jp* j, spu_xdiff_poison_plan* plan)
{
    if (jp_eat(j, '{') != 0) return -1;
    if (jp_peek(j) == '}') { j->p++; return 0; }
    for (;;) {
        char key[64];
        int more;
        if (jp_str(j, key, sizeof key) != 0 || jp_eat(j, ':') != 0) return -1;
        if (!strcmp(key, "fingerprint")) {
            uint64_t v = 0;
            if (jp_address(j, &v) != 0) return -1;
            plan->image_fingerprint = v;
            plan->has_fingerprint = 1;
        } else if (jp_skip(j, 0) != 0) {
            return -1;
        }
        more = jp_next(j, '}');
        if (more < 0) return -1;
        if (more) return 0;
    }
}

static int plan_compare(const void* a, const void* b)
{
    const uint32_t x = ((const spu_xdiff_poison_fn*)a)->entry, y = ((const spu_xdiff_poison_fn*)b)->entry;
    return x < y ? -1 : x > y;
}

static int plan_document(jp* j, spu_xdiff_poison_plan* plan)
{
    int schema = 0, functions = 0;
    if (jp_eat(j, '{') != 0) return -1;
    if (jp_peek(j) == '}') { j->p++; return jp_fail(j, "not a ps3recomp.spu-liveness/1 document"); }
    for (;;) {
        char key[64];
        int more;
        if (jp_str(j, key, sizeof key) != 0 || jp_eat(j, ':') != 0) return -1;
        if (!strcmp(key, "schema")) {
            char value[64];
            if (jp_str(j, value, sizeof value) != 0) return -1;
            if (strcmp(value, "ps3recomp.spu-liveness/1") != 0) return jp_fail(j, "not a ps3recomp.spu-liveness/1 document");
            schema = 1;
        } else if (!strcmp(key, "functions")) {
            if (functions) return jp_fail(j, "functions given twice");
            if (plan_functions(j, plan) != 0) return -1;
            functions = 1;
        } else if (!strcmp(key, "image")) {
            if (plan_image(j, plan) != 0) return -1;
        } else if (jp_skip(j, 0) != 0) {
            return -1;
        }
        more = jp_next(j, '}');
        if (more < 0) return -1;
        if (more) break;
    }
    if (jp_peek(j) != -1) return jp_fail(j, "text after the document");
    if (!schema || !functions) return jp_fail(j, "not a ps3recomp.spu-liveness/1 document");
    if (plan->count > 1) qsort(plan->fn, plan->count, sizeof *plan->fn, plan_compare);
    for (uint32_t i = 1; i < plan->count; i++)
        if (plan->fn[i].entry == plan->fn[i - 1].entry) return jp_fail(j, "a function is listed twice");
    return 0;
}

void spu_xdiff_poison_plan_free(spu_xdiff_poison_plan* plan)
{
    free(plan->fn);
    memset(plan, 0, sizeof *plan);
}

int spu_xdiff_poison_plan_parse(const char* text, size_t bytes, spu_xdiff_poison_plan* out, char* err, size_t err_bytes)
{
    jp j;
    j.base = j.p = text;
    j.end = text + bytes;
    j.err = err;
    j.err_bytes = err_bytes;
    memset(out, 0, sizeof *out);
    if (err && err_bytes) err[0] = 0;
    if (plan_document(&j, out) != 0) {
        spu_xdiff_poison_plan_free(out);
        return -1;
    }
    return 0;
}

int spu_xdiff_poison_plan_load(const char* path, spu_xdiff_poison_plan* out, char* err, size_t err_bytes)
{
    enum { MAX_PLAN_BYTES = 64 << 20 };
    FILE* f = fopen(path, "rb");
    char* text = NULL;
    long size = -1;
    int rc = -1;
    memset(out, 0, sizeof *out);
    if (!f) {
        if (err && err_bytes) snprintf(err, err_bytes, "liveness plan: cannot open %s", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || size > MAX_PLAN_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        if (err && err_bytes) snprintf(err, err_bytes, "liveness plan: %s is unreadable or larger than %d bytes", path, (int)MAX_PLAN_BYTES);
    } else if (!(text = (char*)malloc((size_t)size + 1u))) {
        if (err && err_bytes) snprintf(err, err_bytes, "liveness plan: out of memory");
    } else if (fread(text, 1, (size_t)size, f) != (size_t)size) {
        if (err && err_bytes) snprintf(err, err_bytes, "liveness plan: short read of %s", path);
    } else {
        rc = spu_xdiff_poison_plan_parse(text, (size_t)size, out, err, err_bytes);
    }
    free(text);
    fclose(f);
    return rc;
}
