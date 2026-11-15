/* test_spu_xdiff_poison.c - the poison run (spu_xdiff_replay.h, docs/SPU_XDIFF.md 8.2, docs/SPU_LIFTER.md "Liveness").
 *
 * gen_xdiff_poison_job.py makes one caller and one callee at 0x40 in four variants and runs tools/spu_liveness.py
 * on each: the claims in DIR/xp_<variant>.json are the tool's. Each variant is recorded under T0 into a fixture
 * (written to DIR/xp_<variant>.spujob for the runner tests of CMake) and replayed with a plan:
 *
 *   ok           the tool's plan: equal, with poison applied at the one return
 *   wrong reg    the same job, a plan that calls $r3 dead (the caller signals it): differ / poison, the callee and $r3
 *   frame        the caller reads the callee's frame: the tool's plan, whose frame claim is false: differ / poison,
 *                the callee, the frame byte the signalled word starts at
 *   copy         the caller copies a register the plan calls dead into one it keeps: only the end state shows it
 *   nested       the callee calls a second function: two returns, the inner one with the outer call still open
 *   unbalanced   a callee that does not give its frame back: the tool claims no frame (equal), a plan that does is
 *                false on the stack pointer
 *
 * and the edges: a plan for another image, a lifted subject, a baseline that already differs, a function the
 * job never calls, and the reader's rejections.
 */
#include "../spu_context.h"
#include "../spu_coherency.h"
#include "../spu_dma.h"
#include "../spu_interp.h"
#include "../spu_workload.h"
#include "../spurs_job.h"
#include "../xdiff/spu_xdiff_recorder.h"
#include "../xdiff/spu_xdiff_replay.h"
#include "ps3emu/lifter_abi.h"
#include "xdiff_poison_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DMA guard resolves EAs through vm_base; a static arena is committed. */
static uint8_t g_mem[0x40000];
uint8_t* vm_base = g_mem;
uint32_t ppu_vm_size = sizeof g_mem;
const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;   /* this program links no PPU lift */

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

#define SP0 0x3F000u   /* $r1 at entry: the loader's stack pointer */

/* ---- recording ------------------------------------------------------------------ */
typedef struct { const char* name; const unsigned char* code; uint32_t bytes; } variant;
static const variant k_variants[] = {
    { "ok", xp_ok_code, sizeof xp_ok_code }, { "frame", xp_frame_code, sizeof xp_frame_code },
    { "copy", xp_copy_code, sizeof xp_copy_code }, { "nested", xp_nested_code, sizeof xp_nested_code },
    { "unbalanced", xp_unbalanced_code, sizeof xp_unbalanced_code },
};
enum { V_OK, V_FRAME, V_COPY, V_NESTED, V_UNBALANCED, V_COUNT };
static spu_xdiff_fixture g_fx[V_COUNT];
static spu_xdiff_poison_plan g_tool_plan[V_COUNT];   /* tools/spu_liveness.py's claims for each */

static spu_context g_ctx;
static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }

static void record(spu_xdiff_fixture* f, const variant* v)
{
    spu_context* c = &g_ctx;
    spu_xdiff_unit u;
    spu_xdiff_body_id auth;
    spu_xdiff_recorder r;

    spu_context_init(c, 0);
    c->image_id = -1;   /* the interpreter must not rejoin a lift */
    c->pc = 0;
    memcpy(c->ls, v->code, v->bytes);
    c->gpr[1]._u32[0] = SP0;
    memset(&u, 0, sizeof u);
    u.kind = SPU_XDIFF_UNIT_JOB;
    u.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    u.entry_pc = 0;
    u.return_ls = SPU_JOB_RETURN_LINK_LS;
    u.image_ls = 0;
    u.image_bytes = v->bytes;
    u.r1 = SP0;
    memset(&auth, 0, sizeof auth);
    auth.tier = (uint8_t)SPU_TIER_INTERP;
    REQUIRE(spu_xdiff_fixture_init(f) == 0, "fixture init");
    REQUIRE(spu_xdiff_recorder_begin(&r, f, c, &u, &auth, NULL, NULL, 0) == 0, "recorder begin: %s", r.error.detail);
    const int halted = spu_run_with_halt(interp_entry, c);
    REQUIRE(!halted, "%s: the recorded run halted", v->name);
    const spu_job_end e = spu_job_classify_end(c);
    const uint32_t failed = spu_xdiff_recorder_end(&r, c, e.end_class, e.stop_code);
    REQUIRE(failed == SPU_XDIFF_R_NONE, "recorder end: %s: %s", spu_xdiff_reason_id(failed), r.error.detail);
    spu_coh_unregister(c);
    spu_mfc_release(c);
}

/* ---- replaying ------------------------------------------------------------------- */
static spu_xdiff_shadow* g_shadow;

static uint32_t run(const spu_xdiff_fixture* f, uint32_t tier, const spu_xdiff_poison_plan* plan,
                    spu_xdiff_verdict_record* v, spu_xdiff_poison_report* rep)
{
    spu_xdiff_replay_options opt;
    if (!g_shadow) g_shadow = spu_xdiff_shadow_new();
    REQUIRE(g_shadow != NULL, "shadow");
    memset(&opt, 0, sizeof opt);
    opt.subject_tier = tier;
    memset(g_mem, 0xEE, sizeof g_mem);   /* nothing may read this */
    uint8_t* const saved = vm_base;
    vm_base = NULL;                      /* a served context touches no guest memory */
    const uint32_t r = plan ? spu_xdiff_replay_poison(f, &opt, plan, g_shadow, v, rep) : spu_xdiff_replay(f, &opt, g_shadow, v);
    vm_base = saved;
    CHECK(r == v->verdict, "the return value %u is not the record's verdict %u", r, v->verdict);
    return r;
}

static void expect(const char* what, const spu_xdiff_verdict_record* v, uint32_t verdict, uint32_t reason)
{
    CHECK(v->verdict == verdict && v->reason == reason, "%s: %s / %s, expected %s / %s (index %u where 0x%X)", what,
          spu_xdiff_verdict_id(v->verdict), spu_xdiff_reason_id(v->reason), spu_xdiff_verdict_id(verdict),
          spu_xdiff_reason_id(reason), v->effect_index, v->where);
}

/* A plan for `f` from the members of its "functions" array. */
static void make_plan(spu_xdiff_poison_plan* plan, const spu_xdiff_fixture* f, const char* functions)
{
    char text[4096], err[256];
    snprintf(text, sizeof text, "{\"schema\":\"ps3recomp.spu-liveness/1\",\"image\":{\"fingerprint\":\"0x%016llx\"},\"functions\":[%s]}",
             (unsigned long long)f->identity.image_fingerprint, functions);
    REQUIRE(spu_xdiff_poison_plan_parse(text, strlen(text), plan, err, sizeof err) == 0, "plan %s: %s", functions, err);
}

static const spu_xdiff_poison_fn* plan_fn(const spu_xdiff_poison_plan* plan, uint32_t entry)
{
    for (uint32_t i = 0; i < plan->count; i++)
        if (plan->fn[i].entry == entry) return &plan->fn[i];
    return NULL;
}

static int dead(const spu_xdiff_poison_fn* fn, uint32_t reg) { return (int)((fn->dead[reg >> 6] >> (reg & 63)) & 1u); }

/* ---- the cases -------------------------------------------------------------------- */
static void t_plan_from_tool(void)
{
    const spu_xdiff_poison_plan* p = &g_tool_plan[V_OK];
    const spu_xdiff_poison_fn* callee = plan_fn(p, XP_CALLEE);
    const spu_xdiff_poison_fn* entry = plan_fn(p, 0);
    CHECK(p->has_fingerprint && p->image_fingerprint == g_fx[V_OK].identity.image_fingerprint,
          "the tool's image fingerprint 0x%016llX is not the fixture's 0x%016llX", (unsigned long long)p->image_fingerprint,
          (unsigned long long)g_fx[V_OK].identity.image_fingerprint);
    REQUIRE(callee && entry, "the tool's plan lacks the callee or the entry (%u functions)", p->count);
    CHECK(!dead(callee, 3) && !dead(callee, 1) && dead(callee, 4) && dead(callee, 5) && dead(callee, 0) && dead(callee, 127),
          "the callee's dead registers are wrong: r0 %d r1 %d r3 %d r4 %d r5 %d r127 %d", dead(callee, 0), dead(callee, 1),
          dead(callee, 3), dead(callee, 4), dead(callee, 5), dead(callee, 127));
    CHECK(callee->stack_from == 1 && callee->stack_to == XP_FRAME, "the callee's frame is [%u, %u]", callee->stack_from, callee->stack_to);
    CHECK(entry->dead[0] == 0 && entry->dead[1] == 0 && entry->stack_to == 0, "the caller of unknown callers claims something");
    CHECK(plan_fn(&g_tool_plan[V_UNBALANCED], XP_CALLEE)->stack_to == 0, "the tool claims a frame for a callee that keeps it");
}

static void t_equal(void)
{
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    for (int i = 0; i < V_COUNT; i++) {
        run(&g_fx[i], SPU_TIER_INTERP, NULL, &v, &rep);
        expect("plain T0 replay", &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    }
    /* The tool's plan holds for every job whose caller does what the analysis saw, and poison is applied, not
     * skipped. The frame job's frame claim is the false one: t_frame. */
    static const int claims_hold[] = { V_OK, V_COPY, V_UNBALANCED };
    for (unsigned k = 0; k < sizeof claims_hold / sizeof claims_hold[0]; k++) {
        const int i = claims_hold[k];
        run(&g_fx[i], SPU_TIER_INTERP, &g_tool_plan[i], &v, &rep);
        expect(k_variants[i].name, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
        CHECK(rep.ran == 1 && rep.returns == 1 && rep.poisoned == 1, "%s: ran %u returns %u poisoned %u", k_variants[i].name,
              rep.ran, rep.returns, rep.poisoned);
    }
}

static void t_wrong_register(void)
{
    spu_xdiff_poison_plan plan;
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    char fn[128];
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[9,3,4]}", XP_CALLEE);
    make_plan(&plan, &g_fx[V_OK], fn);
    run(&g_fx[V_OK], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("$r3 called dead", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.ran && rep.has_function && rep.function == XP_CALLEE && rep.kind == SPU_XDIFF_POISON_REGISTER && rep.reg == 3,
          "culprit: function 0x%X kind %u reg %u", rep.function, rep.kind, rep.reg);
    CHECK(rep.underlying == SPU_XDIFF_R_EFFECT_REQUEST, "underlying %s", spu_xdiff_reason_id(rep.underlying));
    CHECK(v.effect_index != SPU_XDIFF_NO_EFFECT && v.subject.kind == SPU_FX_WRCH && v.subject.channel == XP_OUT_MBOX &&
              v.subject.value == SPU_XDIFF_POISON_REG_WORD,
          "the record: effect %u kind %u channel %u value 0x%X", v.effect_index, v.subject.kind, v.subject.channel, v.subject.value);
    CHECK(spu_xdiff_differ_class_of(v.reason, 1) == SPU_XDIFF_CLASS_VISIBLE, "a poison is a visible differ");
    spu_xdiff_poison_plan_free(&plan);

    /* Only the register the caller reads is named, whatever else the plan claims. */
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[2,5,6,7,100,127]}", XP_CALLEE);
    make_plan(&plan, &g_fx[V_OK], fn);
    run(&g_fx[V_OK], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("registers nobody reads", &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    spu_xdiff_poison_plan_free(&plan);
}

static void t_frame(void)
{
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    run(&g_fx[V_FRAME], SPU_TIER_INTERP, &g_tool_plan[V_FRAME], &v, &rep);
    expect("a frame the caller reads", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.has_function && rep.function == XP_CALLEE && rep.kind == SPU_XDIFF_POISON_STACK && rep.below_sp == XP_SPILL_BELOW_SP &&
              rep.exact == 1,
          "culprit: function 0x%X kind %u below_sp 0x%X exact %u (expected 0x%X)", rep.function, rep.kind, rep.below_sp, rep.exact,
          XP_SPILL_BELOW_SP);
    CHECK(rep.underlying == SPU_XDIFF_R_EFFECT_REQUEST, "underlying %s", spu_xdiff_reason_id(rep.underlying));
}

static void t_end_state_only(void)
{
    spu_xdiff_poison_plan plan;
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    char fn[128];
    /* the tool knows the caller copies $r4: its plan is fine, and a plan that calls it dead is not */
    CHECK(!dead(plan_fn(&g_tool_plan[V_COPY], XP_CALLEE), 4), "the tool called the copied register dead");
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[4,5]}", XP_CALLEE);
    make_plan(&plan, &g_fx[V_COPY], fn);
    run(&g_fx[V_COPY], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("a register copied into live state", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.has_function && rep.kind == SPU_XDIFF_POISON_REGISTER && rep.reg == 4 && rep.underlying == SPU_XDIFF_R_END_REGS && v.where == 10,
          "culprit: kind %u reg %u underlying %s where %u", rep.kind, rep.reg, spu_xdiff_reason_id(rep.underlying), v.where);
    spu_xdiff_poison_plan_free(&plan);
}

static void t_nested(void)
{
    spu_xdiff_poison_plan plan;
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    char fn[256];
    const spu_xdiff_poison_plan* tool = &g_tool_plan[V_NESTED];
    const spu_xdiff_poison_fn* outer = plan_fn(tool, XP_CALLEE);
    const spu_xdiff_poison_fn* inner = plan_fn(tool, XP_INNER);
    REQUIRE(outer && inner, "the tool's plan lacks the outer or the inner function");
    /* $r0 is dead when the inner function returns (the outer one reloads it) and the outer one's frame holds the link */
    CHECK(dead(inner, 0) && !dead(inner, 3) && inner->stack_to == 0 && outer->stack_to == XP_NESTED_FRAME && !dead(outer, 3),
          "inner: r0 %d r3 %d frame %u; outer: frame %u r3 %d", dead(inner, 0), dead(inner, 3), inner->stack_to, outer->stack_to, dead(outer, 3));
    run(&g_fx[V_NESTED], SPU_TIER_INTERP, tool, &v, &rep);
    expect("nested calls", &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(rep.returns == 2 && rep.poisoned == 2, "returns %u poisoned %u", rep.returns, rep.poisoned);

    /* the value the inner function returns is the outer one's to pass on: calling it dead at the inner return is found
     * there, and calling it dead at the outer return is found at the outer one */
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[3]}", XP_INNER);
    make_plan(&plan, &g_fx[V_NESTED], fn);
    run(&g_fx[V_NESTED], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("the inner result called dead", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.has_function && rep.function == XP_INNER && rep.kind == SPU_XDIFF_POISON_REGISTER && rep.reg == 3, "culprit 0x%X reg %u",
          rep.function, rep.reg);
    spu_xdiff_poison_plan_free(&plan);
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[3]},{\"entry\":\"0x%X\",\"dead_regs\":[4]}", XP_CALLEE, XP_INNER);
    make_plan(&plan, &g_fx[V_NESTED], fn);
    run(&g_fx[V_NESTED], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("the outer result called dead", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.has_function && rep.function == XP_CALLEE && rep.kind == SPU_XDIFF_POISON_REGISTER && rep.reg == 3, "culprit 0x%X reg %u",
          rep.function, rep.reg);
    spu_xdiff_poison_plan_free(&plan);
}

static void t_unbalanced(void)
{
    spu_xdiff_poison_plan plan;
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    char fn[128];
    snprintf(fn, sizeof fn, "{\"entry\":\"0x%X\",\"dead_regs\":[],\"dead_stack\":{\"below_sp\":%u}}", XP_CALLEE, XP_FRAME);
    make_plan(&plan, &g_fx[V_UNBALANCED], fn);
    run(&g_fx[V_UNBALANCED], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("a frame claim the stack pointer contradicts", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_POISON);
    CHECK(rep.has_function && rep.function == XP_CALLEE && rep.kind == SPU_XDIFF_POISON_STACK && rep.below_sp == 0 &&
              rep.underlying == SPU_XDIFF_R_NONE,
          "culprit: kind %u below_sp %u underlying %s", rep.kind, rep.below_sp, spu_xdiff_reason_id(rep.underlying));
    spu_xdiff_poison_plan_free(&plan);
}

static void t_edges(void)
{
    spu_xdiff_poison_plan plan;
    spu_xdiff_verdict_record v;
    spu_xdiff_poison_report rep;
    char fn[160];

    /* a plan computed for another image */
    make_plan(&plan, &g_fx[V_OK], "");
    plan.image_fingerprint ^= 1;
    run(&g_fx[V_OK], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("another image's plan", &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_FIXTURE_IDENTITY);
    CHECK(!rep.ran, "a refused plan ran");
    /* the observer is T0's */
    plan.image_fingerprint ^= 1;
    run(&g_fx[V_OK], SPU_TIER_LIFTED, &plan, &v, &rep);
    expect("a lifted subject", &v, SPU_XDIFF_MALFORMED, SPU_XDIFF_R_NOT_COMPARABLE);
    spu_xdiff_poison_plan_free(&plan);

    /* a function the job never calls, and the caller that nothing calls */
    snprintf(fn, sizeof fn, "{\"entry\":\"0x80\",\"dead_regs\":[3,4,5],\"dead_stack\":{\"below_sp\":64}},{\"entry\":0,\"dead_regs\":[3]}");
    make_plan(&plan, &g_fx[V_OK], fn);
    run(&g_fx[V_OK], SPU_TIER_INTERP, &plan, &v, &rep);
    expect("functions that are not called", &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(rep.ran && rep.returns == 0 && rep.poisoned == 0, "ran %u returns %u poisoned %u", rep.ran, rep.returns, rep.poisoned);
    spu_xdiff_poison_plan_free(&plan);

    /* a baseline that already differs: the verdict is the replay's own and no poison run is made */
    spu_xdiff_fixture bad = g_fx[V_OK];   /* a shallow copy: the owned arrays stay g_fx[V_OK]'s */
    make_plan(&plan, &g_fx[V_OK], "{\"entry\":\"0x40\",\"dead_regs\":[3]}");
    bad.reg1[16 * 20 + 3] ^= 1;
    run(&bad, SPU_TIER_INTERP, &plan, &v, &rep);
    expect("a fixture the plain replay differs from", &v, SPU_XDIFF_DIFFER, SPU_XDIFF_R_END_REGS);
    CHECK(!rep.ran && v.where == 20, "ran %u where %u", rep.ran, v.where);
    spu_xdiff_poison_plan_free(&plan);
}

static void bad_plan(const char* what, const char* text)
{
    spu_xdiff_poison_plan plan;
    char err[256];
    CHECK(spu_xdiff_poison_plan_parse(text, strlen(text), &plan, err, sizeof err) != 0 && plan.fn == NULL && plan.count == 0,
          "accepted: %s", what);
    CHECK(err[0] != 0, "%s: no message", what);
}

#define DOC(body) "{\"schema\":\"ps3recomp.spu-liveness/1\",\"functions\":[" body "]}"

static void t_reader(void)
{
    spu_xdiff_poison_plan plan;
    char err[256];
    static const char good[] =
        "{ \"schema\" : \"ps3recomp.spu-liveness/1\", \"tool\":\"spu_liveness\", \"version\": 1,\n"
        "  \"image\": {\"sha256\":\"ab\\u0063\", \"fingerprint\": \"0x00000000000000ff\", \"bytes\": 12, \"code_end\": null},\n"
        "  \"functions\": [\n"
        "    {\"entry\": \"0x00040\", \"exits\": [\"0x44\"], \"live_out\": [1,3], \"dead_regs\": [0,2,127], \"dead_stack\": {\"below_sp\": 64},\n"
        "     \"callers\": [], \"nested\": {\"a\": [1, 2.5e3, -4, true, false, null, \"x\\\"y\"]}, \"unknown_callers\": false},\n"
        "    {\"entry\": 16, \"dead_regs\": [], \"dead_stack\": null}\n"
        "  ], \"trailer\": [] }\n";
    REQUIRE(spu_xdiff_poison_plan_parse(good, sizeof good - 1, &plan, err, sizeof err) == 0, "a good plan: %s", err);
    CHECK(plan.count == 2 && plan.fn[0].entry == 16 && plan.fn[1].entry == 0x40 && plan.has_fingerprint && plan.image_fingerprint == 0xFF,
          "parsed: %u functions, first 0x%X, fingerprint 0x%llX", plan.count, plan.fn[0].entry, (unsigned long long)plan.image_fingerprint);
    CHECK(plan.fn[1].stack_from == 1 && plan.fn[1].stack_to == 64 && dead(&plan.fn[1], 0) && dead(&plan.fn[1], 2) &&
              dead(&plan.fn[1], 127) && !dead(&plan.fn[1], 1) && !dead(&plan.fn[1], 3),
          "the claims of the function at 0x40");
    spu_xdiff_poison_plan_free(&plan);

    bad_plan("empty text", "");
    bad_plan("not an object", "[]");
    bad_plan("another schema", "{\"schema\":\"ps3recomp.spu-liveness/2\",\"functions\":[]}");
    bad_plan("no schema", "{\"functions\":[]}");
    bad_plan("no functions", "{\"schema\":\"ps3recomp.spu-liveness/1\"}");
    bad_plan("functions twice", "{\"schema\":\"ps3recomp.spu-liveness/1\",\"functions\":[],\"functions\":[]}");
    bad_plan("trailing text", DOC("") " x");
    bad_plan("truncated", "{\"schema\":\"ps3recomp.spu-liveness/1\",\"functions\":[{\"entry\":\"0x40\"");
    bad_plan("$r1 dead", DOC("{\"entry\":\"0x40\",\"dead_regs\":[3,1]}"));
    bad_plan("register 128", DOC("{\"entry\":\"0x40\",\"dead_regs\":[128]}"));
    bad_plan("register -1", DOC("{\"entry\":\"0x40\",\"dead_regs\":[-1]}"));
    bad_plan("register 3.0", DOC("{\"entry\":\"0x40\",\"dead_regs\":[3.0]}"));
    bad_plan("register as a string", DOC("{\"entry\":\"0x40\",\"dead_regs\":[\"3\"]}"));
    bad_plan("leading zero", DOC("{\"entry\":\"0x40\",\"dead_regs\":[03]}"));
    bad_plan("no entry", DOC("{\"dead_regs\":[3]}"));
    bad_plan("empty function", DOC("{}"));
    bad_plan("unaligned entry", DOC("{\"entry\":\"0x42\"}"));
    bad_plan("entry past local store", DOC("{\"entry\":\"0x40000\"}"));
    bad_plan("entry that is not a number", DOC("{\"entry\":\"main\"}"));
    bad_plan("entry with trailing text", DOC("{\"entry\":\"0x40 \"}"));
    bad_plan("a function twice", DOC("{\"entry\":\"0x40\"},{\"entry\":64}"));
    bad_plan("frame past local store", DOC("{\"entry\":\"0x40\",\"dead_stack\":{\"below_sp\":262145}}"));
    bad_plan("frame without below_sp", DOC("{\"entry\":\"0x40\",\"dead_stack\":{}}"));
    bad_plan("frame of a wrong type", DOC("{\"entry\":\"0x40\",\"dead_stack\":3}"));
    bad_plan("trailing comma", DOC("{\"entry\":\"0x40\",},"));
    bad_plan("bad escape", DOC("{\"entry\":\"0x40\",\"x\":\"\\q\"}"));
    bad_plan("control character", DOC("{\"entry\":\"0x40\",\"x\":\"\t\"}"));
    {
        char deep[512] = "{\"schema\":\"ps3recomp.spu-liveness/1\",\"functions\":[],\"x\":";
        const size_t at = strlen(deep);
        memset(deep + at, '[', 80);
        memset(deep + at + 80, ']', 80);
        strcpy(deep + at + 160, "}");
        bad_plan("deep nesting", deep);
    }
    bad_plan("a fingerprint that is not a number", "{\"schema\":\"ps3recomp.spu-liveness/1\",\"image\":{\"fingerprint\":\"zz\"},\"functions\":[]}");

    spu_xdiff_poison_plan from_file;
    CHECK(spu_xdiff_poison_plan_load("/nonexistent/xp.json", &from_file, err, sizeof err) != 0 && err[0], "a missing file");
}

int main(int argc, char** argv)
{
    REQUIRE(argc == 2, "usage: %s DIR  (DIR holds xp_<variant>.json from gen_xdiff_poison_job.py; the fixtures go there)", argv[0]);
    char path[1024], err[256];
    for (int i = 0; i < V_COUNT; i++) {
        record(&g_fx[i], &k_variants[i]);
        snprintf(path, sizeof path, "%s/xp_%s.json", argv[1], k_variants[i].name);
        REQUIRE(spu_xdiff_poison_plan_load(path, &g_tool_plan[i], err, sizeof err) == 0, "%s: %s", path, err);
        snprintf(path, sizeof path, "%s/xp_%s.spujob", argv[1], k_variants[i].name);
        spu_xdiff_error werr;
        REQUIRE(spu_xdiff_fixture_write(&g_fx[i], path, &werr) == 0, "writing %s: %s", path, werr.detail);
    }
    t_plan_from_tool();
    t_equal();
    t_wrong_register();
    t_frame();
    t_end_state_only();
    t_nested();
    t_unbalanced();
    t_edges();
    t_reader();
    for (int i = 0; i < V_COUNT; i++) {
        spu_xdiff_poison_plan_free(&g_tool_plan[i]);
        spu_xdiff_fixture_free(&g_fx[i]);
    }
    spu_xdiff_shadow_free(g_shadow);
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("spu_xdiff_poison: ok\n");
    return 0;
}
