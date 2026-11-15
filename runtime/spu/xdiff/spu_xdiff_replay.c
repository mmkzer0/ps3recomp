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

typedef struct replay {
    const spu_xdiff_fixture* f;
    spu_xdiff_verdict_record* v;
    uint32_t cursor;     /* the next transcript record */
    int      failed;     /* the serve table filled the verdict and ended the run */
    uint64_t budget;     /* T0 instruction budget; 0: none */
    uint64_t spent;
    int      over_budget;
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

/* ---- T0 budget (section 10) ------------------------------------------------ */

/* on_insn runs before every interpreted instruction. Yielding at the budget stops the outermost
 * interpreter run with nothing consumed; the driver reads `over_budget` after the run. */
static int budget_on_insn(void* user, spu_context* ctx, uint32_t pc)
{
    replay* r = (replay*)user;
    (void)ctx; (void)pc;
    if (r->spent >= r->budget) { r->over_budget = 1; return 1; }
    r->spent++;
    return 0;
}

static void entry_interp(spu_context* ctx)
{
    spu_interp_run(ctx, ctx->pc);
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

uint32_t spu_xdiff_replay(const spu_xdiff_fixture* f, const spu_xdiff_replay_options* opt,
                          spu_xdiff_shadow* s, spu_xdiff_verdict_record* out)
{
    memset(out, 0, sizeof *out);
    out->effect_index = SPU_XDIFF_NO_EFFECT;
    out->subject_body.tier = (uint8_t)opt->subject_tier;
    if ((opt->subject_tier & SPU_TIER_MASK) == (uint32_t)SPU_TIER_LIFTED)
        out->subject_body.lifter_abi = SPU_RUNTIME_LIFTER_ABI;   /* every registered lift passed the ABI gate */

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
    if (f->identity.context_census != spu_xdiff_context_census())
        fprintf(stderr, "[spu-xdiff] the fixture was recorded under spu_context census 0x%08X; this build's is 0x%08X\n",
                (unsigned)f->identity.context_census, (unsigned)spu_xdiff_context_census());
    const uint32_t match = spu_xdiff_match_for(f->identity.authority.tier, opt->subject_tier);
    out->match = match;
    /* Contract matching (a pair with native code) is not built here: no pair of this runner has it, and a
     * same-program verdict for it would be a false answer. */
    if (match != (uint32_t)SPU_XDIFF_MATCH_PROGRAM) return verdict_of(out, SPU_XDIFF_R_NOT_COMPARABLE);

    void (*entry)(spu_context*) = entry_interp;
    int image_id = -1;   /* T0: -1 keeps the interpreter from rejoining a lifted function (section 9) */
    if ((opt->subject_tier & SPU_TIER_MASK) == (uint32_t)SPU_TIER_LIFTED) {
        entry = spu_workload_find_img(f->identity.image_fingerprint, &image_id);
        if (!entry) return verdict_of(out, SPU_XDIFF_R_NO_BODY);
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
    s->port.serve = &k_serve;
    s->port.serve_user = r;
    s->port.tap = NULL;
    s->port.tap_user = NULL;
    spu_port_set(ctx, &s->port);
    if (image_id < 0 && opt->budget) {
        r->budget = opt->budget;
        out->budget = opt->budget;
        s->obs.user = r;
        s->obs.on_insn = budget_on_insn;
        s->obs.on_mfc = NULL;
        s->obs.on_atomic = NULL;
        s->obs.on_channel = NULL;
        spu_observer_set(ctx, &s->obs);
    }

    const int halted = spu_run_with_halt(entry, ctx);
    out->budget_spent = r->spent;
    out->subject_pc = ctx->pc;

    /* A served context that reached the runtime below the seam has broken the design's central claim
     * (section 9): a defect of the runtime, not a verdict about the subject. */
    if (ctx->coh_listed != 0 || spu_mfc_has_engine(ctx)) {
        fprintf(stderr, "[spu-xdiff] shadow context %p reached the runtime below the effect port "
                        "(coh_listed=%d mfc_engine=%d) -- aborting\n",
                (void*)ctx, ctx->coh_listed, spu_mfc_has_engine(ctx));
        fflush(stderr);
        abort();
    }

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
    return compare_end(f, ctx, out);
}
