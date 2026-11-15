/* test_spu_xdiff_replay.c - replaying a fixture into a shadow context, and the offline runner
 * (spu_xdiff_replay.h, docs/SPU_XDIFF.md sections 8.1, 9, 10, 13 and the row-8 line of 16).
 *
 * The base job (gen_xdiff_replay_jobs.py, the port job of test_spu_effect_port.c) runs interpreted
 * against a synthetic guest memory under the recorder; the fixtures that makes are replayed into
 * shadow contexts with vm_base == NULL, so a subject that reached guest memory would crash. Each
 * lifted variant is the base with one mutation a same-program comparison has to name:
 *
 *   equal            T0 and the lift of the base job against its own transcript: equal
 *   size             one changed instruction: differ / effect-request at effect 0
 *   mask             another tag mask, the register then overwritten: differ / effect-request
 *   stall            the remaining elements of a stalled list rewritten before the acknowledgement,
 *                    put back afterwards: differ / effect-request at the acknowledgement
 *   latent           only the end local store differs: differ / end-ls; latent for a JobQueue2
 *                    fixture, visible for a jobchain one
 *   nested           a stop inside a nested lifted call: differ (docs/SPU_XDIFF.md 5.1)
 *   budget           a T0 subject over its instruction budget: void / budget
 *   halt then budget a T0 shadow ends by serve-halt, the next one on the thread exhausts its budget:
 *                    void / budget, not an abort (section 9)
 *   float, tweak     other float mode: malformed / fixture-identity; a tweak: malformed / not-comparable
 *   no body, miss, depth guard
 *                    not-found / no-body; void / unlifted-branch; void / depth-guard
 *   runner           exit codes 0 to 5 through spu_job_replay_main (the fork path), the JSON object
 *                    against the field list of section 13, and a refused knob
 */
#include "../spu_context.h"
#include "../spu_dma.h"
#include "../spu_interp.h"
#include "../spu_coherency.h"
#include "../spu_workload.h"
#include "../spurs_job.h"
#include "../xdiff/spu_xdiff_recorder.h"
#include "../xdiff/spu_xdiff_replay.h"
#include "ps3emu/lifter_abi.h"
#include "xdiff_replay_jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* The DMA guard resolves EAs through vm_base; a static arena is committed. */
static uint8_t g_mem[0x40000];
uint8_t* vm_base = g_mem;
uint32_t ppu_vm_size = sizeof g_mem;
const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;   /* this program links no PPU lift */

/* The SPURS runtime sources the channel runtime links against reach these; no job of this test touches them. */
uint32_t g_barrier_sync_watch = 0;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
void spu_begin_image(int image_id);

/* The lifted variants, each lifted with its own symbol prefix (CMake). */
#define LIFTS(X) X(base) X(size) X(mask) X(stall) X(latent) X(nested)
#define DECL_LIFT(n) void xr_##n##_spu_recomp_register(void); void xr_##n##_spu_func_00000000(spu_context*);
LIFTS(DECL_LIFT)
void xr_base_spu_func_00000400(spu_context*);
#define LIFT_ENUM(n) L_##n,
enum { LIFTS(LIFT_ENUM) L_COUNT };
#define IMG_OF(l) (20 + (l))   /* image ids: not 1, which spu_run_with_halt traces */
typedef struct { const char* name; void (*entry)(spu_context*); } lift_row;
#define LIFT_ROW(n) { #n, xr_##n##_spu_func_00000000 },
static const lift_row g_lifts[] = { LIFTS(LIFT_ROW) };

static int g_fail;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
            printf("FAIL: ");                                                \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)
#define REQUIRE(cond, ...)                                                   \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: ");                                                \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* ---- the world and the jobs --------------------------------------------------- */
static void fill_world(uint8_t* w)
{
    static const struct { uint32_t ea, n; } r[] = {
        { XR_EA_GET, XR_GET_SIZE }, { XR_EA_LIST0, XR_LIST0_SIZE },
        { XR_EA_LIST1, XR_LIST1_SIZE }, { XR_EA_ATOM, 128 },
        { XR_EA_SL0, 3 * XR_SL_SIZE }, { XR_EA_ZERO, XR_ZERO_SIZE } };
    memset(w, 0, sizeof g_mem);
    for (unsigned k = 0; k < sizeof r / sizeof r[0]; k++)
        for (uint32_t i = 0; i < r[k].n; i++) w[r[k].ea + i] = (uint8_t)((r[k].ea + i) * 131u + 7u);
}

static void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t w_il(int rt, uint32_t imm16) { return (0x081u << 23) | ((imm16 & 0xFFFFu) << 7) | (uint32_t)rt; }

typedef struct {
    const unsigned char* code;
    uint32_t code_bytes, entry, image_bytes, profile;
    int plant_miss;
    uint32_t code_ls;   /* where the code sits in local store (0: at the image base) */
} job_def;
static const job_def J_BASE   = { xr_code, XR_CODE_BYTES, XR_ENTRY, 0x410u, SPU_XDIFF_PROFILE_JOBCHAIN, 0 };
/* the same code over a different span of local store: a fingerprint no lift is registered under */
static const job_def J_UNREG  = { xr_code, XR_CODE_BYTES, XR_ENTRY, 0x420u, SPU_XDIFF_PROFILE_JOBCHAIN, 0 };
/* the job that branches to local store no lift holds; the interpreter runs the planted bytes */
static const job_def J_MISS   = { xr_code, XR_CODE_BYTES, XR_MISS_ENTRY, XR_MISS_ENTRY, SPU_XDIFF_PROFILE_JOBCHAIN, 1 };
static const job_def J_NESTED = { xr_nested_code, XR_NESTED_CODE_BYTES, 0, 0x30u, SPU_XDIFF_PROFILE_JOBCHAIN, 0 };

static spu_context g_ctx;

static void ctx_setup(spu_context* c, const job_def* j)
{
    spu_context_init(c, 0);
    c->image_id = -1;   /* the interpreter must not rejoin a lift */
    c->pc = j->entry;
    memcpy(c->ls + j->code_ls, j->code, j->code_bytes);
    put_be32(c->ls + XR_LS_LIST, XR_LIST0_SIZE);
    put_be32(c->ls + XR_LS_LIST + 4, XR_EA_LIST0);
    put_be32(c->ls + XR_LS_LIST + 8, XR_LIST1_SIZE);
    put_be32(c->ls + XR_LS_LIST + 12, XR_EA_LIST1);
    put_be32(c->ls + XR_LS_SLIST, 0x80000000u | XR_SL_SIZE);   /* the first element stalls */
    put_be32(c->ls + XR_LS_SLIST + 4, XR_EA_SL0);
    put_be32(c->ls + XR_LS_SLIST + 8, XR_SL_SIZE);
    put_be32(c->ls + XR_LS_SLIST + 12, XR_EA_SL1);
    put_be32(c->ls + XR_LS_SLIST + 16, XR_SL_SIZE);
    put_be32(c->ls + XR_LS_SLIST + 20, XR_EA_SL2);
    for (uint32_t i = 0; i < XR_PUT_SIZE; i++) c->ls[XR_LS_PUT + i] = (uint8_t)(i * 5u + 3u);
    if (j->plant_miss) {
        put_be32(c->ls + XR_MISS_TARGET, w_il(10, XR_MISS_MARK));
        put_be32(c->ls + XR_MISS_TARGET + 4, XR_MISS_STOP_CODE & 0x3FFFu);   /* stop */
    }
}

static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }

/* Runs `j` interpreted under the recorder against guest memory and returns its fixture. */
static void record(spu_xdiff_fixture* f, const job_def* j)
{
    spu_context* c = &g_ctx;
    spu_xdiff_unit u;
    spu_xdiff_body_id auth;
    spu_xdiff_recorder r;

    fill_world(g_mem);
    ctx_setup(c, j);
    memset(&u, 0, sizeof u);
    u.kind = SPU_XDIFF_UNIT_JOB;
    u.profile = j->profile;
    u.entry_pc = j->entry;
    u.return_ls = SPU_JOB_RETURN_LINK_LS;
    u.image_ls = 0;
    u.image_bytes = j->image_bytes;
    memset(&auth, 0, sizeof auth);
    auth.tier = (uint8_t)SPU_TIER_INTERP;
    REQUIRE(spu_xdiff_fixture_init(f) == 0, "fixture init");
    REQUIRE(spu_xdiff_recorder_begin(&r, f, c, &u, &auth, NULL, NULL, 0) == 0, "recorder begin: %s", r.error.detail);
    const int halted = spu_run_with_halt(interp_entry, c);
    REQUIRE(!halted, "the recorded run halted");
    const spu_job_end e = spu_job_classify_end(c);
    const uint32_t failed = spu_xdiff_recorder_end(&r, c, e.end_class, e.stop_code);
    REQUIRE(failed == SPU_XDIFF_R_NONE, "recorder end: %s: %s", spu_xdiff_reason_id(failed), r.error.detail);
    spu_coh_unregister(c);
    spu_mfc_release(c);
}

/* ---- replaying ----------------------------------------------------------------- */
static spu_xdiff_shadow* g_shadow;

static uint32_t replay(const spu_xdiff_fixture* f, uint32_t tier, uint64_t budget, spu_xdiff_verdict_record* v)
{
    spu_xdiff_replay_options opt;
    if (!g_shadow) g_shadow = spu_xdiff_shadow_new();
    REQUIRE(g_shadow != NULL, "shadow");
    opt.subject_tier = tier;
    opt.budget = budget;
    memset(g_mem, 0xEE, sizeof g_mem);   /* nothing may read this */
    uint8_t* const saved = vm_base;
    vm_base = NULL;                      /* a served context touches no guest memory */
    const uint32_t r = spu_xdiff_replay(f, &opt, g_shadow, v);
    vm_base = saved;
    return r;
}

static void use_lift(const spu_xdiff_fixture* f, int l)
{
    spu_workload_register_img(f->identity.image_fingerprint, g_lifts[l].entry, IMG_OF(l), g_lifts[l].name);
}

static void expect_v(const char* what, uint32_t ret, const spu_xdiff_verdict_record* v, uint32_t verdict, uint32_t reason)
{
    CHECK(ret == v->verdict, "%s: the return value %u is not the record's verdict %u", what, ret, v->verdict);
    CHECK(v->verdict == verdict && v->reason == reason, "%s: %s / %s, expected %s / %s (index %u where 0x%X pc 0x%X)", what,
          spu_xdiff_verdict_id(v->verdict), spu_xdiff_reason_id(v->reason), spu_xdiff_verdict_id(verdict),
          spu_xdiff_reason_id(reason), v->effect_index, v->where, v->subject_pc);
}

static uint32_t find_effect(const spu_xdiff_fixture* f, uint32_t kind, uint32_t channel)
{
    for (uint32_t i = 0; i < f->effect_count; i++)
        if (f->effects[i].kind == kind && f->effects[i].channel == channel) return i;
    return SPU_XDIFF_NO_EFFECT;
}

/* ---- the cases ------------------------------------------------------------------ */
static spu_xdiff_fixture F, FM, FN, FU;

static void t_equal(void)
{
    spu_xdiff_verdict_record v;
    CHECK(F.effect_count > 20, "the recorded base job has only %u effects", F.effect_count);
    CHECK(F.end.end_class == SPU_XDIFF_END_STOP && F.end.stop_code == XR_STOP_CODE, "the base job ended as class %u code 0x%X",
          F.end.end_class, F.end.stop_code);
    use_lift(&F, L_base);
    uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("lift vs T0", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(v.match == SPU_XDIFF_MATCH_PROGRAM && v.effect_index == SPU_XDIFF_NO_EFFECT, "equal: match %u index 0x%X", v.match, v.effect_index);
    CHECK(v.subject_body.tier == SPU_TIER_LIFTED && v.subject_body.lifter_abi == SPU_RUNTIME_LIFTER_ABI,
          "the subject body is tier %u abi %u", v.subject_body.tier, v.subject_body.lifter_abi);
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    expect_v("T0 vs T0", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

static void t_size(void)
{
    spu_xdiff_verdict_record v;
    use_lift(&F, L_size);
    const uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("changed instruction", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_REQUEST);
    CHECK(v.effect_index == 0 && v.subject.kind == SPU_FX_MFC_CMD && v.subject.size == XR_SIZE_BAD,
          "the first effect: index %u kind %u size %u", v.effect_index, v.subject.kind, v.subject.size);
    CHECK(F.effects[0].kind == SPU_FX_MFC_CMD && F.effects[0].size == XR_GET_SIZE, "the transcript's first effect is not the GET");
}

static void t_mask(void)
{
    spu_xdiff_verdict_record v;
    const uint32_t at = find_effect(&F, SPU_FX_WRCH, MFC_WrTagMask);
    use_lift(&F, L_mask);
    const uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("another tag mask", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_REQUEST);
    CHECK(at != SPU_XDIFF_NO_EFFECT && v.effect_index == at && v.subject.kind == SPU_FX_WRCH &&
          v.subject.channel == MFC_WrTagMask && v.subject.value == XR_MASK_BAD && F.effects[at].value == 1u << XR_TAG,
          "the mask write: index %u (expected %u) value 0x%X", v.effect_index, at, v.subject.value);
}

static void t_stall(void)
{
    spu_xdiff_verdict_record v;
    const uint32_t at = find_effect(&F, SPU_FX_WRCH, MFC_WrListStallAck);
    REQUIRE(at != SPU_XDIFF_NO_EFFECT, "the transcript has no acknowledgement");
    CHECK(F.effects[at].size == 2 * 8 && F.effects[at].lsa == XR_LS_SLIST + 8 && F.effects[at].blob != SPU_XDIFF_NO_BLOB,
          "the acknowledgement carries lsa 0x%X size %u blob 0x%X, expected the two remaining elements", F.effects[at].lsa,
          F.effects[at].size, F.effects[at].blob);
    use_lift(&F, L_stall);
    const uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("rewritten stalled list", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_REQUEST);
    CHECK(v.effect_index == at && v.where == XR_STALL_WHERE, "the acknowledgement: index %u (expected %u) where %u (expected %u)",
          v.effect_index, at, v.where, XR_STALL_WHERE);
    CHECK(v.want[0] != v.got[0] || v.want[1] != v.got[1] || v.want[2] != v.got[2] || v.want[3] != v.got[3],
          "want and got are the same bytes");
}

static void t_latent(void)
{
    spu_xdiff_verdict_record v;
    use_lift(&F, L_latent);
    F.unit.profile = SPU_XDIFF_PROFILE_JQ2;
    uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("end local store, JQ2", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_END_LS);
    /* the quadword is r15 = four words 0x00005A5A in guest byte order: 00 00 5A 5A; the first differing byte is the third */
    CHECK(v.where == XR_LATENT_LS + 2 && v.effect_index == SPU_XDIFF_NO_EFFECT, "end-ls where 0x%X index 0x%X", v.where, v.effect_index);
    CHECK(v.want[0] == 0 && v.got[0] == 0x5A && v.got[1] == 0x5A && v.got[2] == 0 && v.got[3] == 0, "end-ls bytes: want %02X got %02X%02X%02X%02X",
          v.want[0], v.got[0], v.got[1], v.got[2], v.got[3]);
    CHECK(spu_xdiff_differ_class_of(v.reason, F.unit.profile) == SPU_XDIFF_CLASS_LATENT, "a JobQueue2 end-ls difference is not latent");
    F.unit.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("end local store, jobchain", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_END_LS);
    CHECK(spu_xdiff_differ_class_of(v.reason, F.unit.profile) == SPU_XDIFF_CLASS_VISIBLE, "a jobchain end-ls difference is not visible");
}

static void t_nested(void)
{
    spu_xdiff_verdict_record v;
    CHECK(FN.end.end_class == SPU_XDIFF_END_STOP && FN.end.stop_code == XR_NESTED_SUB_STOP, "the interpreter's nested job ended as class %u code 0x%X",
          FN.end.end_class, FN.end.stop_code);
    spu_workload_register_img(FN.identity.image_fingerprint, xr_nested_spu_func_00000000, IMG_OF(L_nested), "nested");
    uint32_t r = replay(&FN, SPU_TIER_LIFTED, 0, &v);
    CHECK(r == SPU_XDIFF_DIFFER && v.verdict == SPU_XDIFF_DIFFER && v.reason == SPU_XDIFF_R_END_CLASS,
          "a stop inside a nested lifted call: %s / %s, expected differ / end-class (the lift runs on to the caller's stop)",
          spu_xdiff_verdict_id(v.verdict), spu_xdiff_reason_id(v.reason));
    CHECK(v.want[0] == SPU_XDIFF_END_STOP && v.want[4] == XR_NESTED_SUB_STOP && v.got[0] == SPU_XDIFF_END_STOP &&
          v.got[4] == XR_NESTED_AFTER_STOP, "end-class words: want %u/0x%X got %u/0x%X", v.want[0], v.want[4], v.got[0], v.got[4]);
    r = replay(&FN, SPU_TIER_INTERP, 0, &v);
    expect_v("nested job, T0 vs T0", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

static void t_budget(void)
{
    spu_xdiff_verdict_record v;
    uint32_t r = replay(&F, SPU_TIER_INTERP, 5, &v);
    expect_v("T0 over budget", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
    CHECK(v.budget == 5 && v.budget_spent == 5, "budget %llu spent %llu", (unsigned long long)v.budget, (unsigned long long)v.budget_spent);
    /* a budget the run stays under changes nothing */
    r = replay(&F, SPU_TIER_INTERP, 1u << 20, &v);
    expect_v("T0 under budget", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(v.budget_spent > 5 && v.budget_spent < (1u << 20), "an equal run spent %llu instructions", (unsigned long long)v.budget_spent);
}

/* A T0 shadow ends by serve-halt, which longjmps over the interpreter's run and skips its depth decrement;
 * the next T0 shadow on this thread must still be the outermost run, or its first budget yield aborts. */
static void t_halt_then_budget(void)
{
    spu_xdiff_verdict_record v;
    const uint32_t saved = F.effects[0].size;
    F.effects[0].size ^= 0x10u;   /* the subject's first request no longer matches */
    uint32_t r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[0].size = saved;
    expect_v("T0 shadow ended by serve-halt", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_REQUEST);
    CHECK(spu_interp_depth_get() == 0, "the halt left the interpreter depth at %d", spu_interp_depth_get());
    r = replay(&F, SPU_TIER_INTERP, 5, &v);
    expect_v("the next T0 shadow over its budget", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
}

static void t_identity(void)
{
    spu_xdiff_verdict_record v;
    const uint8_t saved = F.identity.float_rz;
    F.identity.float_rz = (uint8_t)!spu_float_rz_mode();
    uint32_t r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.identity.float_rz = saved;
    expect_v("other float mode", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_IDENTITY);

    r = replay(&F, SPU_TIER_INTERP | SPU_TIER_TWEAK, 0, &v);
    expect_v("tweak subject", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_NOT_COMPARABLE);
    const uint8_t saved_tier = F.identity.authority.tier;
    F.identity.authority.tier = (uint8_t)(SPU_TIER_INTERP | SPU_TIER_TWEAK);
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.identity.authority.tier = saved_tier;
    expect_v("tweak authority", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_NOT_COMPARABLE);

    /* another effect vocabulary, another planted link: not this runner's fixtures */
    uint32_t u32 = F.identity.effect_port_abi;
    F.identity.effect_port_abi = u32 ^ 1u;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.identity.effect_port_abi = u32;
    expect_v("other effect port ABI", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_IDENTITY);
    u32 = F.unit.return_ls;
    F.unit.return_ls = u32 + 0x100u;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.unit.return_ls = u32;
    expect_v("other planted link", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_IDENTITY);
    /* another context census is a note on stderr, not a refusal (section 9) */
    u32 = F.identity.context_census;
    F.identity.context_census = u32 ^ 1u;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.identity.context_census = u32;
    expect_v("other context census", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);

    /* no registered body for this image */
    r = replay(&FU, SPU_TIER_LIFTED, 0, &v);
    expect_v("no body", r, &v, SPU_XDIFF_NOT_FOUND, SPU_XDIFF_R_NO_BODY);
    r = replay(&FU, SPU_TIER_INTERP, 0, &v);
    expect_v("T0 needs no body", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

/* The first difference of each kind, made by editing the recorded fixture (a T0 subject replays it unchanged
 * otherwise, which t_equal showed to be equal). */
static void t_reasons(void)
{
    spu_xdiff_verdict_record v;
    const uint32_t n = F.effect_count;
    uint32_t r;

    /* effect-extra: the last record dropped, and the subject issues it anyway */
    F.effect_count = n - 1;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effect_count = n;
    expect_v("a record dropped", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_EXTRA);
    CHECK(v.effect_index == n - 1 && v.subject.kind == SPU_FX_WRCH && v.subject.channel == SPU_WrOutMbox,
          "effect-extra: index %u (expected %u) subject kind %u channel %u", v.effect_index, n - 1, v.subject.kind, v.subject.channel);

    /* effect-missing: a record the subject never reaches */
    spu_xdiff_effect extra;
    memset(&extra, 0, sizeof extra);
    extra.kind = SPU_FX_RDCH;
    extra.channel = SPU_RdInMbox;
    extra.blob = SPU_XDIFF_NO_BLOB;
    REQUIRE(spu_xdiff_fixture_add_effect(&F, &extra) == 0, "add_effect");
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effect_count = n;
    expect_v("a record added", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_MISSING);
    CHECK(v.effect_index == n && v.subject.kind == 0, "effect-missing: index %u (expected %u) subject kind %u", v.effect_index, n, v.subject.kind);

    /* effect-kind: another kind, and another channel of the same kind */
    const uint8_t kind0 = F.effects[0].kind;
    F.effects[0].kind = SPU_FX_WRCH;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[0].kind = kind0;
    expect_v("another kind", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_KIND);
    CHECK(v.effect_index == 0 && v.subject.kind == SPU_FX_MFC_CMD, "effect-kind: index %u subject kind %u", v.effect_index, v.subject.kind);
    const uint32_t mask_at = find_effect(&F, SPU_FX_WRCH, MFC_WrTagMask);
    REQUIRE(mask_at != SPU_XDIFF_NO_EFFECT, "no tag mask write in the transcript");
    F.effects[mask_at].channel = MFC_WrTagUpdate;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[mask_at].channel = MFC_WrTagMask;
    expect_v("another channel", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_KIND);
    CHECK(v.effect_index == mask_at, "effect-kind (channel): index %u (expected %u)", v.effect_index, mask_at);

    /* effect-payload: one byte of the PUT's recorded payload */
    uint32_t put_at = SPU_XDIFF_NO_EFFECT;
    for (uint32_t i = 0; i < n; i++)
        if (F.effects[i].kind == SPU_FX_XFER && F.effects[i].dir == SPU_XFER_FROM_LS && F.effects[i].value == 0x20u) { put_at = i; break; }
    REQUIRE(put_at != SPU_XDIFF_NO_EFFECT && F.effects[put_at].blob != SPU_XDIFF_NO_BLOB, "no PUT transfer in the transcript");
    F.blob[F.effects[put_at].blob + 2] ^= 0x01u;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.blob[F.effects[put_at].blob + 2] ^= 0x01u;
    expect_v("a PUT payload byte", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_EFFECT_PAYLOAD);
    CHECK(v.effect_index == put_at && v.where == 2 && (v.want[0] ^ v.got[0]) == 1u && v.subject.kind == SPU_FX_MFC_CMD,
          "effect-payload: index %u (expected %u) where %u bytes %02X/%02X", v.effect_index, put_at, v.where, v.want[0], v.got[0]);

    /* end-regs: one byte of one end register */
    F.reg1[16 * 5 + 3] ^= 0x01u;
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.reg1[16 * 5 + 3] ^= 0x01u;
    expect_v("an end register byte", r, &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_END_REGS);
    CHECK(v.where == 5 && (v.want[3] ^ v.got[3]) == 1u && v.effect_index == SPU_XDIFF_NO_EFFECT, "end-regs: where %u bytes %02X/%02X", v.where,
          v.want[3], v.got[3]);

    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    expect_v("after the edits", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

/* Records that decode but cannot be replayed: malformed, never a difference of the subject (the decoder refuses
 * these in a file; a fixture built in memory can still hold them). */
static void t_hostile(void)
{
    spu_xdiff_verdict_record v;
    uint32_t xfer_at = SPU_XDIFF_NO_EFFECT, ack_at = find_effect(&F, SPU_FX_WRCH, MFC_WrListStallAck);
    for (uint32_t i = 0; i < F.effect_count; i++)
        if (F.effects[i].kind == SPU_FX_XFER && F.effects[i].dir == SPU_XFER_TO_LS) { xfer_at = i; break; }
    REQUIRE(xfer_at != SPU_XDIFF_NO_EFFECT && ack_at != SPU_XDIFF_NO_EFFECT, "the transcript lacks a GET transfer or an acknowledgement");

    spu_xdiff_effect saved = F.effects[xfer_at];
    F.effects[xfer_at].lsa = SPU_LS_SIZE - 16u;   /* 32 bytes from here leave local store */
    uint32_t r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[xfer_at] = saved;
    expect_v("a transfer past local store", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_FORMAT);
    CHECK(v.effect_index == xfer_at, "bad record: index %u (expected %u)", v.effect_index, xfer_at);

    F.effects[xfer_at].blob = F.blob_bytes;       /* its payload lies outside BLOB */
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[xfer_at] = saved;
    expect_v("a payload outside BLOB", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_FORMAT);

    saved = F.effects[ack_at];
    F.effects[ack_at].blob = F.blob_bytes;        /* the acknowledgement's list lies outside BLOB */
    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    F.effects[ack_at] = saved;
    expect_v("an acknowledgement list outside BLOB", r, &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_FORMAT);
    CHECK(v.effect_index == ack_at, "bad record: index %u (expected %u)", v.effect_index, ack_at);

    r = replay(&F, SPU_TIER_INTERP, 0, &v);
    expect_v("after the edits", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

/* Two ends of a served interpreter that no port mediates. */
static spu_xdiff_fixture FS, FH;
static unsigned char g_stop110[16], g_hle[16];
static job_def J_STOP110, J_HLE;

static void t_unsupported(void)
{
    spu_xdiff_verdict_record v;
    /* `stop 0x110` with nothing pending ends the recorded run; the shadow never services it either */
    uint32_t r = replay(&FS, SPU_TIER_INTERP, 0, &v);
    expect_v("stop 0x110, nothing left", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    spu_xdiff_effect extra;
    memset(&extra, 0, sizeof extra);
    extra.kind = SPU_FX_RDCH;
    extra.channel = SPU_RdInMbox;
    extra.blob = SPU_XDIFF_NO_BLOB;
    REQUIRE(spu_xdiff_fixture_add_effect(&FS, &extra) == 0, "add_effect");
    r = replay(&FS, SPU_TIER_INTERP, 0, &v);
    FS.effect_count = 0;
    expect_v("stop 0x110 with a record left", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_UNSUPPORTED_EFFECT);

    /* the 0xA70 taskset hook, reached by an interpreter running as the taskset image (id 22): ends as a halt */
    spu_workload_register_img(FH.identity.image_fingerprint, interp_entry, 22, "hle");
    r = replay(&FH, SPU_TIER_LIFTED, 0, &v);
    expect_v("the 0xA70 hook", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_UNSUPPORTED_EFFECT);
    r = replay(&FH, SPU_TIER_INTERP, 0, &v);   /* as plain T0 the address is just code */
    expect_v("0xA70 as plain T0", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

/* The stack-reset halt case of spu_run_with_halt (spu_restart_dispatch, longjmp 2) restores the interpreter depth too. */
void spu_restart_dispatch(spu_context* ctx);
static void restart_entry(spu_context* ctx)
{
    spu_interp_depth_set(7);   /* as if an interpreter run were live on the stack the jump discards */
    spu_restart_dispatch(ctx);
}

static void t_restart_depth(void)
{
    spu_xdiff_verdict_record v;
    spu_workload_register_img(F.identity.image_fingerprint, restart_entry, IMG_OF(L_COUNT) + 2, "restart");
    const uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    /* the restart re-enters the resolver, which holds nothing at the entry pc of this image */
    expect_v("stack reset", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_UNLIFTED_BRANCH);
    CHECK(spu_interp_depth_get() == 0, "the stack-reset halt left the interpreter depth at %d", spu_interp_depth_get());
}

static void t_miss(void)
{
    spu_xdiff_verdict_record v;
    spu_workload_register_img(FM.identity.image_fingerprint, xr_base_spu_func_00000400, IMG_OF(L_base), "miss");
    uint32_t r = replay(&FM, SPU_TIER_LIFTED, 0, &v);
    expect_v("branch to unlifted code", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_UNLIFTED_BRANCH);
    r = replay(&FM, SPU_TIER_INTERP, 0, &v);
    expect_v("the interpreter runs the same bytes", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
}

/* A registered body that runs the runtime's own depth guard (spu_depth_guard halts without an architectural status). */
static void guard_entry(spu_context* ctx)
{
    spu_depth_guard(ctx);   /* the first call reads the limit; at depth 0 it returns */
    ctx->host_depth = SPU_RELAXED_LOAD(&g_spu_depth_guard_limit);
    spu_depth_guard(ctx);
}

static void t_depth_guard(void)
{
    spu_xdiff_verdict_record v;
    spu_workload_register_img(F.identity.image_fingerprint, guard_entry, IMG_OF(L_COUNT), "guard");
    const uint32_t r = replay(&F, SPU_TIER_LIFTED, 0, &v);
    expect_v("depth guard", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_DEPTH_GUARD);
}

static void t_knobs(void)
{
    static const char* const k_names[] = { "YDKJ_STOP_HALT", "SPU_TAILRET", "SPU_NO_LS0_END", "SPU_INTERP_UNLIFTED" };
    CHECK(spu_xdiff_refused_knob() == NULL, "a refused knob is set in the test's environment: %s", spu_xdiff_refused_knob());
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        spu_xdiff_verdict_record v;
#if defined(_WIN32)
        _putenv_s(k_names[i], "1");
#else
        setenv(k_names[i], "1", 1);
#endif
        const char* got = spu_xdiff_refused_knob();
        CHECK(got != NULL && !strcmp(got, k_names[i]), "%s set: spu_xdiff_refused_knob returned %s", k_names[i], got ? got : "NULL");
        const uint32_t r = replay(&F, SPU_TIER_INTERP, 0, &v);
        CHECK(r == SPU_XDIFF_VERDICT_INVALID && v.verdict == SPU_XDIFF_VERDICT_INVALID, "%s set: the replay answered %u", k_names[i], r);
#if defined(_WIN32)
        _putenv_s(k_names[i], "");
#else
        unsetenv(k_names[i]);
#endif
    }
}

/* ---- the runner ------------------------------------------------------------------ */
#if !defined(_WIN32)
static void crash_entry(spu_context* ctx)
{
    (void)ctx;
    abort();   /* the replay child dies by a signal */
}

/* A minimal JSON syntax check: one value, then only whitespace. */
static const char* js_ws(const char* p)
{
    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    return p;
}
static const char* js_value(const char* p, int depth);
static const char* js_string(const char* p)
{
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) {
        if ((unsigned char)*p < 0x20) return NULL;
        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                for (int i = 1; i <= 4; i++)
                    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f') || (p[i] >= 'A' && p[i] <= 'F'))) return NULL;
                p += 4;
            } else if (!*p || !strchr("\"\\/bfnrt", *p)) {
                return NULL;
            }
        }
    }
    return *p == '"' ? p + 1 : NULL;
}
static const char* js_value(const char* p, int depth)
{
    if (depth > 8) return NULL;
    p = js_ws(p);
    if (*p == '"') return js_string(p);
    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        p = js_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = js_string(js_ws(p));
                if (!p) return NULL;
                p = js_ws(p);
                if (*p++ != ':') return NULL;
            }
            p = js_value(p, depth + 1);
            if (!p) return NULL;
            p = js_ws(p);
            if (*p == ',') { p++; continue; }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (!strncmp(p, "null", 4)) return p + 4;
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    const char* q = p;
    if (*q == '-') q++;
    if (!(*q >= '0' && *q <= '9')) return NULL;
    while ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-') q++;
    return q;
}
static int json_valid(const char* s)
{
    const char* e = js_value(s, 0);
    return e && *js_ws(e) == 0 && e > s && s[strlen(s) - 1] == '\n';   /* one object, one line, no trailing junk */
}

/* The field list of section 13 (plus `class`), each present, in this order. */
static void check_json_fields(const char* what, const char* j)
{
    static const char* const k_keys[] = {
        "{\"schema\":\"ps3recomp.spujob-verdict/1\"", "\"tool\":", "\"version\":", "\"verdict\":", "\"exit_code\":", "\"reason\":",
        "\"match\":", "\"class\":", "\"fixture\":{\"path\":", "\"image_fingerprint\":", "\"authority_tier\":", "\"effects\":",
        "\"toolkit_commit\":", "\"subject\":{\"tier\":", "\"lifter_abi\":", "\"code_end\":", "\"lift_options\":", "\"native_rev\":",
        "\"first\":{\"effect_index\":", "\"authority\":", "\"subject\":", "\"where\":", "\"want\":", "\"got\":",
        "\"subject_pc\":", "\"budget\":{\"limit\":", "\"spent\":", "\"seconds\":" };
    CHECK(json_valid(j), "%s: the output is not one JSON object on one line: %s", what, j);
    const char* p = j;
    for (size_t i = 0; i < sizeof k_keys / sizeof k_keys[0]; i++) {
        const char* at = strstr(p, k_keys[i]);
        if (!at) { CHECK(0, "%s: the JSON lacks %s (or has it out of order): %s", what, k_keys[i], j); return; }
        p = at + strlen(k_keys[i]);
    }
}

static char g_dir[128];
static char g_files[24][160];
static int g_nfiles;

static const char* fixture_file(const spu_xdiff_fixture* f)
{
    spu_xdiff_error err;
    if (!g_dir[0]) {
        const char* t = getenv("TMPDIR");
        snprintf(g_dir, sizeof g_dir, "%s/xr_XXXXXX", t && t[0] ? t : "/tmp");
        REQUIRE(mkdtemp(g_dir) != NULL, "mkdtemp");
    }
    REQUIRE(g_nfiles < 24, "too many fixture files");
    snprintf(g_files[g_nfiles], sizeof g_files[0], "%s/f%d.spujob", g_dir, g_nfiles);
    memset(&err, 0, sizeof err);
    REQUIRE(spu_xdiff_fixture_write(f, g_files[g_nfiles], &err) == 0, "writing a fixture: %s", err.detail);
    return g_files[g_nfiles++];
}

static const char* garbage_file(void)
{
    (void)fixture_file(&F);   /* makes the directory */
    snprintf(g_files[g_nfiles], sizeof g_files[0], "%s/garbage.spujob", g_dir);
    FILE* o = fopen(g_files[g_nfiles], "wb");
    REQUIRE(o != NULL, "garbage file");
    fputs("this is not a fixture, however long the file is\n", o);
    fclose(o);
    return g_files[g_nfiles++];
}

/* Runs spu_job_replay_main in a child process with its output captured; returns its exit status. */
static int run_runner(const char* const* args, int nargs, char* out, size_t outn, char* err, size_t errn)
{
    char* argv[16];
    int po[2], pe[2], st = 0;
    size_t lo = 0, le = 0;
    REQUIRE(nargs < 14, "too many runner arguments");
    argv[0] = (char*)"spu_job_replay";
    for (int i = 0; i < nargs; i++) argv[i + 1] = (char*)args[i];
    argv[nargs + 1] = NULL;
    REQUIRE(pipe(po) == 0 && pipe(pe) == 0, "pipe");
    fflush(NULL);
    const pid_t pid = fork();
    REQUIRE(pid >= 0, "fork");
    if (pid == 0) {
        dup2(po[1], 1); dup2(pe[1], 2);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        const int rc = spu_job_replay_main(nargs + 1, argv);
        fflush(NULL);
        _exit(rc);
    }
    close(po[1]); close(pe[1]);
    for (ssize_t n; lo < outn - 1 && (n = read(po[0], out + lo, outn - 1 - lo)) > 0;) lo += (size_t)n;
    for (ssize_t n; le < errn - 1 && (n = read(pe[0], err + le, errn - 1 - le)) > 0;) le += (size_t)n;
    out[lo] = 0; err[le] = 0;
    close(po[0]); close(pe[0]);
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1000 - (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

typedef struct { char out[8192], err[4096]; int rc; } run_result;

static run_result g_run;
static void runner(const char* what, int want_rc, const char* a0, const char* a1, const char* a2, const char* a3)
{
    const char* args[4];
    int n = 0;
    if (a0) args[n++] = a0;
    if (a1) args[n++] = a1;
    if (a2) args[n++] = a2;
    if (a3) args[n++] = a3;
    g_run.rc = run_runner(args, n, g_run.out, sizeof g_run.out, g_run.err, sizeof g_run.err);
    CHECK(g_run.rc == want_rc, "%s: exit status %d, expected %d (stderr: %s)", what, g_run.rc, want_rc, g_run.err);
}

static void out_has(const char* what, const char* needle)
{
    CHECK(strstr(g_run.out, needle) != NULL, "%s: the output lacks %s: %s", what, needle, g_run.out);
}

static void t_runner(void)
{
    /* 0: equal, as JSON and as one human line */
    const char* pbase = fixture_file(&F);
    use_lift(&F, L_base);
    runner("equal", 0, "--json", pbase, NULL, NULL);
    check_json_fields("equal", g_run.out);
    out_has("equal", "\"verdict\":\"equal\""); out_has("equal", "\"exit_code\":0"); out_has("equal", "\"reason\":\"none\"");
    out_has("equal", "\"class\":\"none\""); out_has("equal", "\"first\":{\"effect_index\":null");
    out_has("equal", "\"subject\":{\"tier\":1,\"lifter_abi\":1");
    runner("equal, one line", 0, pbase, NULL, NULL, NULL);
    CHECK(!strncmp(g_run.out, "equal none ", 11) && strchr(g_run.out, '\n') == g_run.out + strlen(g_run.out) - 1, "the human line: %s", g_run.out);

    /* 1: usage errors and a refused knob; no verdict on standard output */
    runner("no arguments", 1, NULL, NULL, NULL, NULL);
    CHECK(strstr(g_run.err, "usage") != NULL && g_run.out[0] == 0, "no arguments: stderr %s stdout %s", g_run.err, g_run.out);
    runner("unknown option", 1, "--frobnicate", pbase, NULL, NULL);
    runner("bad number", 1, "--budget", "12x", pbase, NULL);
    runner("bad tier", 1, "--tier", "lifted", pbase, NULL);
    runner("two fixtures", 1, pbase, pbase, NULL, NULL);
    runner("zero timeout", 1, "--timeout", "0", pbase, NULL);
    setenv("SPU_TAILRET", "1", 1);
    runner("a refused knob", 1, "--json", pbase, NULL, NULL);
    CHECK(strstr(g_run.err, "SPU_TAILRET") != NULL && g_run.out[0] == 0, "a refused knob: stderr %s stdout %s", g_run.err, g_run.out);
    unsetenv("SPU_TAILRET");

    /* 2: malformed: another float mode, and a file that is not a fixture */
    const uint8_t saved = F.identity.float_rz;
    F.identity.float_rz = (uint8_t)!spu_float_rz_mode();
    const char* pfloat = fixture_file(&F);
    F.identity.float_rz = saved;
    runner("other float mode", 2, "--json", pfloat, NULL, NULL);
    check_json_fields("other float mode", g_run.out);
    out_has("other float mode", "\"verdict\":\"malformed\""); out_has("other float mode", "\"reason\":\"fixture-identity\"");
    runner("not a fixture", 2, "--json", garbage_file(), NULL, NULL);
    check_json_fields("not a fixture", g_run.out);
    out_has("not a fixture", "\"reason\":\"fixture-format\""); out_has("not a fixture", "\"image_fingerprint\":null");

    /* 3: not found: no file, and an image without a registered body */
    runner("no file", 3, "--json", "/nonexistent/xr.spujob", NULL, NULL);
    check_json_fields("no file", g_run.out);
    out_has("no file", "\"verdict\":\"not-found\""); out_has("no file", "\"reason\":\"no-fixture\"");
    const char* punreg = fixture_file(&FU);
    runner("no body", 3, "--json", punreg, NULL, NULL);
    out_has("no body", "\"reason\":\"no-body\"");
    runner("no body, tier interp", 0, "--tier", "interp", punreg, NULL);

    /* 4: differ, named by the effect; visible and latent */
    use_lift(&F, L_size);
    runner("changed instruction", 4, "--json", pbase, NULL, NULL);
    check_json_fields("changed instruction", g_run.out);
    out_has("changed instruction", "\"verdict\":\"differ\""); out_has("changed instruction", "\"reason\":\"effect-request\"");
    out_has("changed instruction", "\"class\":\"visible\""); out_has("changed instruction", "\"effect_index\":0");
    out_has("changed instruction", "\"authority\":{\"kind\":\"mfc-cmd\""); out_has("changed instruction", "\"subject\":{\"kind\":\"mfc-cmd\"");
    out_has("changed instruction", "\"where\":null,\"want\":null,\"got\":null");   /* a scalar field of the request: no byte position */
    use_lift(&F, L_latent);
    runner("end local store, jobchain", 4, "--json", pbase, NULL, NULL);
    out_has("end local store, jobchain", "\"reason\":\"end-ls\""); out_has("end local store, jobchain", "\"class\":\"visible\"");
    out_has("end local store, jobchain", "\"where\":6146");
    F.unit.profile = SPU_XDIFF_PROFILE_JQ2;
    const char* pjq2 = fixture_file(&F);
    F.unit.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    runner("end local store, JQ2", 4, "--json", pjq2, NULL, NULL);
    check_json_fields("end local store, JQ2", g_run.out);
    out_has("end local store, JQ2", "\"reason\":\"end-ls\""); out_has("end local store, JQ2", "\"class\":\"latent\"");
    runner("end local store, JQ2, line", 4, pjq2, NULL, NULL, NULL);
    CHECK(strstr(g_run.out, "differ end-ls") == g_run.out && strstr(g_run.out, " (latent)\n") != NULL, "the human line: %s", g_run.out);

    /* 5: void: a T0 budget, and a body that kills its process */
    {
        const char* args[6] = { "--json", "--tier", "interp", "--budget", "5", pbase };
        g_run.rc = run_runner(args, 6, g_run.out, sizeof g_run.out, g_run.err, sizeof g_run.err);
        CHECK(g_run.rc == 5, "budget: exit status %d, expected 5 (stderr: %s)", g_run.rc, g_run.err);
        check_json_fields("budget", g_run.out);
        out_has("budget", "\"verdict\":\"void\""); out_has("budget", "\"reason\":\"budget\"");
        out_has("budget", "\"budget\":{\"limit\":5,\"spent\":5}");
    }
    spu_workload_register_img(F.identity.image_fingerprint, crash_entry, IMG_OF(L_COUNT) + 1, "crash");
    runner("a body that aborts", 5, "--json", pbase, NULL, NULL);
    check_json_fields("a body that aborts", g_run.out);
    out_has("a body that aborts", "\"reason\":\"host-fault\"");
    CHECK(strstr(g_run.err, "signal") != NULL, "a body that aborts: stderr does not name the signal: %s", g_run.err);
    out_has("a body that aborts", "\"match\":1");   /* the pair's rule survives a host fault */

    /* end-class: no byte position, but the two {class, code} words */
    spu_workload_register_img(FN.identity.image_fingerprint, xr_nested_spu_func_00000000, IMG_OF(L_nested), "nested");
    runner("end-class", 4, "--json", fixture_file(&FN), NULL, NULL);
    check_json_fields("end-class", g_run.out);
    out_has("end-class", "\"reason\":\"end-class\""); out_has("end-class", "\"where\":null,\"want\":\"");

    /* a fixture path that needs escaping: a quote and two non-ASCII bytes */
    use_lift(&F, L_base);
    {
        spu_xdiff_error werr;
        memset(&werr, 0, sizeof werr);
        snprintf(g_files[g_nfiles], sizeof g_files[0], "%s/q\"\xC3\xA9.spujob", g_dir);
        REQUIRE(spu_xdiff_fixture_write(&F, g_files[g_nfiles], &werr) == 0, "writing a fixture: %s", werr.detail);
        const char* pq = g_files[g_nfiles++];
        runner("an escaped path", 0, "--json", pq, NULL, NULL);
        check_json_fields("an escaped path", g_run.out);
        out_has("an escaped path", "q\\\"\\u00c3\\u00a9.spujob\"");
    }

    for (int i = 0; i < g_nfiles; i++) remove(g_files[i]);
    if (g_dir[0]) rmdir(g_dir);
}
#endif /* !_WIN32 */

int main(int argc, char** argv)
{
    REQUIRE(spu_xdiff_refused_knob() == NULL, "a refused knob is set: %s", spu_xdiff_refused_knob());
#define REGISTER_LIFT(n) spu_begin_image(IMG_OF(L_##n)); xr_##n##_spu_recomp_register();
    LIFTS(REGISTER_LIFT)
    spu_begin_image(0);

    record(&F, &J_BASE);
    record(&FM, &J_MISS);
    record(&FN, &J_NESTED);
    record(&FU, &J_UNREG);
    REQUIRE(F.identity.image_fingerprint != FU.identity.image_fingerprint, "the unregistered image shares the base fingerprint");

    /* two tiny jobs at LS 0x100: `stop 0x110`, and a branch to 0xA70 */
    put_be32(g_stop110, 0x110u);
    J_STOP110 = (job_def){ g_stop110, 4, 0x100, 0x110, SPU_XDIFF_PROFILE_JOBCHAIN, 0, 0x100 };
    put_be32(g_hle, w_il(3, 0xA70));
    put_be32(g_hle + 4, (0x1A8u << 21) | (3u << 7));   /* bi r3 */
    J_HLE = (job_def){ g_hle, 8, 0x100, 0x110, SPU_XDIFF_PROFILE_JOBCHAIN, 0, 0x100 };
    record(&FS, &J_STOP110);
    record(&FH, &J_HLE);

    /* with a path argument the base fixture is also left there, for the spu_job_replay executable's own test */
    if (argc > 1) {
        spu_xdiff_error werr;
        memset(&werr, 0, sizeof werr);
        REQUIRE(spu_xdiff_fixture_write(&F, argv[1], &werr) == 0, "writing %s: %s", argv[1], werr.detail);
    }

    t_equal();
    t_size();
    t_mask();
    t_stall();
    t_latent();
    t_nested();
    t_budget();
    t_halt_then_budget();
    t_identity();
    t_reasons();
    t_hostile();
    t_unsupported();
    t_restart_depth();
    t_miss();
    t_depth_guard();
    t_knobs();
#if !defined(_WIN32)
    t_runner();
#else
    printf("SKIP: the runner cases need fork\n");
#endif

    spu_xdiff_shadow_free(g_shadow);
    spu_xdiff_fixture_free(&F); spu_xdiff_fixture_free(&FM); spu_xdiff_fixture_free(&FN); spu_xdiff_fixture_free(&FU);
    spu_xdiff_fixture_free(&FS); spu_xdiff_fixture_free(&FH);
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("OK: xdiff replay (shadow, serve table, verdicts, runner)\n");
    return 0;
}
