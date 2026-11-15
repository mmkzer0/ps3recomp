/* test_spu_xdiff_budget.c - the budgeted twin of a lifted image (docs/SPU_XDIFF.md section 10, row 9 of 16).
 *
 * gen_xdiff_budget_job.py writes three loops without a single effect, one for each kind of site the twin
 * counts: a backward goto (a), cross-function transfers (b) and an indirect branch (c). The image is lifted
 * once and compiled twice (CMake): the production object, and the twin, which is the same generated source
 * under -DSPU_LIFT_BUDGET with its symbols renamed (xdiff_budget_twin.c). Each job is recorded interpreted
 * (an empty transcript, its end state) and replayed into a shadow at the registered tier:
 *
 *   registry     the production entry stays what spu_workload_find_img answers; the twin has its own entry
 *                and image id; a twin for no body, or under image id 0, is refused; registering the body
 *                again drops its twin
 *   equal        a budget the loop stays under: equal, and the counter reads what the loop spent
 *   budget       a small budget: void / budget, the counter at the limit, and the process goes on -- the
 *                next replay on the same shadow and thread is equal
 *   endless      r3 = 0 counts through 2^32 laps; the budget ends it, which no timeout is waited for
 *   none         budget 0 runs the production body, and so does an image without a twin
 *   runner       spu_job_replay_main --tier registered with a budget: exit 5 / budget, then equal
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
#include "xdiff_budget_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
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

/* The image, lifted with the prefix xb_, and its twin (the same names, suffixed). */
void xb_spu_recomp_register(void);
void xb_spu_recomp_register_budget(void);
void xb_spu_func_00000000(spu_context*);
void xb_spu_func_00000100(spu_context*);
void xb_spu_func_00000200(spu_context*);
void xb_spu_func_00000000_budget(spu_context*);
void xb_spu_func_00000100_budget(spu_context*);
void xb_spu_func_00000200_budget(spu_context*);

#define PROD_IMAGE 40   /* not 1, which spu_run_with_halt traces */
#define TWIN_IMAGE 41

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

typedef struct {
    const char* name;
    uint32_t entry, stop_code, image_bytes;
    spu_lifted_entry_fn prod, twin;
    uint64_t spent;   /* transfers the loop makes with r3 = LAPS: what the twin must count */
} job_def;

#define LAPS 1000u
/* The sites the twin counts, per job: a taken `brnz` after each lap but the last (a: LAPS - 1); a `br` per lap
 * and a taken `brnz` after each but the last (b: 2 * LAPS - 1); the fall-through from c0 into c1 and a taken
 * `binz` after each lap but the last (c: LAPS). */
static const job_def J[] = {
    { "a", XB_A_ENTRY, XB_A_STOP, 0x410u, xb_spu_func_00000000, xb_spu_func_00000000_budget, LAPS - 1 },
    { "b", XB_B_ENTRY, XB_B_STOP, 0x420u, xb_spu_func_00000100, xb_spu_func_00000100_budget, 2 * LAPS - 1 },
    { "c", XB_C_ENTRY, XB_C_STOP, 0x430u, xb_spu_func_00000200, xb_spu_func_00000200_budget, LAPS },
};
#define NJOBS (sizeof J / sizeof J[0])

static spu_context g_ctx;

static void interp_entry(spu_context* c) { spu_interp_run(c, c->pc); }

static void set_laps(spu_context* c, uint32_t laps)
{
    for (int i = 0; i < 4; i++) c->gpr[3]._u32[i] = laps;
}

/* Runs `j` interpreted under the recorder, r3 = LAPS, and returns its fixture: the empty transcript and the end. */
static void record(spu_xdiff_fixture* f, const job_def* j, uint32_t image_bytes)
{
    spu_context* c = &g_ctx;
    spu_xdiff_unit u;
    spu_xdiff_body_id auth;
    spu_xdiff_recorder r;

    spu_context_init(c, 0);
    c->image_id = -1;   /* the interpreter must not rejoin a lift */
    c->pc = j->entry;
    memcpy(c->ls, xb_code, XB_CODE_BYTES);
    set_laps(c, LAPS);
    memset(&u, 0, sizeof u);
    u.kind = SPU_XDIFF_UNIT_JOB;
    u.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    u.entry_pc = j->entry;
    u.return_ls = SPU_JOB_RETURN_LINK_LS;
    u.image_ls = 0;
    u.image_bytes = image_bytes;
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

static spu_xdiff_shadow* g_shadow;

static uint32_t replay(const spu_xdiff_fixture* f, uint32_t tier, uint64_t budget, spu_xdiff_verdict_record* v)
{
    spu_xdiff_replay_options opt;
    if (!g_shadow) g_shadow = spu_xdiff_shadow_new();
    REQUIRE(g_shadow != NULL, "shadow");
    opt.subject_tier = tier;
    opt.budget = budget;
    uint8_t* const saved = vm_base;
    vm_base = NULL;   /* a served context touches no guest memory */
    const uint32_t r = spu_xdiff_replay(f, &opt, g_shadow, v);
    vm_base = saved;
    return r;
}

static void expect_v(const char* what, uint32_t ret, const spu_xdiff_verdict_record* v, uint32_t verdict, uint32_t reason)
{
    CHECK(ret == v->verdict, "%s: the return value %u is not the record's verdict %u", what, ret, v->verdict);
    CHECK(v->verdict == verdict && v->reason == reason, "%s: %s / %s, expected %s / %s (pc 0x%X)", what,
          spu_xdiff_verdict_id(v->verdict), spu_xdiff_reason_id(v->reason), spu_xdiff_verdict_id(verdict),
          spu_xdiff_reason_id(reason), v->subject_pc);
}

static spu_xdiff_fixture F[NJOBS];

/* r3 of the fixture's entry registers, every lane (guest byte order, 16 bytes a register). */
static void fixture_laps(spu_xdiff_fixture* f, uint32_t laps)
{
    for (int lane = 0; lane < 4; lane++) {
        uint8_t* p = f->reg0 + 16 * 3 + 4 * lane;
        p[0] = (uint8_t)(laps >> 24); p[1] = (uint8_t)(laps >> 16); p[2] = (uint8_t)(laps >> 8); p[3] = (uint8_t)laps;
    }
}

static void t_registry(void)
{
    for (size_t k = 0; k < NJOBS; k++) {
        const uint64_t fp = F[k].identity.image_fingerprint;
        int id = -1;
        CHECK(spu_workload_find_img(fp, &id) == J[k].prod && id == PROD_IMAGE,
              "%s: find_img no longer answers the production body (id %d)", J[k].name, id);
        id = -1;
        CHECK(spu_workload_find_twin(fp, &id) == J[k].twin && id == TWIN_IMAGE,
              "%s: find_twin does not answer the twin (id %d)", J[k].name, id);
        CHECK(J[k].prod != J[k].twin, "%s: the twin is the production body", J[k].name);
    }
    /* an image nobody registered has no twin, and the out value says so */
    int id = -1;
    CHECK(spu_workload_find_twin(0x1234567812345678ull, &id) == NULL && id == 0, "an unknown fingerprint has a twin (id %d)", id);
    /* refused: a twin for no body, and one under the wildcard image id 0 (spu_lookup matches every image with it) */
    spu_workload_register_twin(0x1234567812345678ull, xb_spu_func_00000000_budget, TWIN_IMAGE);
    CHECK(spu_workload_find_twin(0x1234567812345678ull, NULL) == NULL, "a twin without a body was registered");
    spu_xdiff_fixture fx;
    record(&fx, &J[0], 0x440u);
    spu_workload_register_img(fx.identity.image_fingerprint, xb_spu_func_00000000, PROD_IMAGE, "no-twin");
    spu_workload_register_twin(fx.identity.image_fingerprint, xb_spu_func_00000000_budget, 0);
    CHECK(spu_workload_find_twin(fx.identity.image_fingerprint, NULL) == NULL, "a twin under image id 0 was registered");
    spu_workload_register_twin(fx.identity.image_fingerprint, NULL, TWIN_IMAGE);
    CHECK(spu_workload_find_twin(fx.identity.image_fingerprint, NULL) == NULL, "a NULL twin was registered");

    /* an image without a twin: the budget does not apply, the production body runs and answers as it always did */
    spu_xdiff_verdict_record v;
    uint32_t r = replay(&fx, SPU_TIER_LIFTED, 10, &v);
    expect_v("no twin, budget 10", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(v.budget == 0 && v.budget_spent == 0, "no twin: budget %llu spent %llu", (unsigned long long)v.budget,
          (unsigned long long)v.budget_spent);
    spu_xdiff_fixture_free(&fx);
}

static void t_job(size_t k)
{
    spu_xdiff_verdict_record v;
    const char* n = J[k].name;
    spu_xdiff_fixture* f = &F[k];
    CHECK(f->effect_count == 0, "%s: the loop recorded %u effects", n, f->effect_count);
    CHECK(f->end.end_class == SPU_XDIFF_END_STOP && f->end.stop_code == J[k].stop_code, "%s: the interpreter's end is class %u code 0x%X", n,
          f->end.end_class, f->end.stop_code);

    /* a budget the loop stays under: equal, and the twin's counter is the loop's transfers */
    uint32_t r = replay(f, SPU_TIER_LIFTED, 1u << 20, &v);
    expect_v("under budget", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(v.budget == (1u << 20) && v.budget_spent == J[k].spent, "%s: under budget: limit %llu spent %llu, expected %llu", n,
          (unsigned long long)v.budget, (unsigned long long)v.budget_spent, (unsigned long long)J[k].spent);

    /* a budget it is over: void / budget with the counter at the limit; and the process goes on */
    r = replay(f, SPU_TIER_LIFTED, 10, &v);
    expect_v("over budget", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
    CHECK(v.budget == 10 && v.budget_spent == 10, "%s: over budget: limit %llu spent %llu", n, (unsigned long long)v.budget,
          (unsigned long long)v.budget_spent);
    CHECK(v.match == SPU_XDIFF_MATCH_PROGRAM, "%s: over budget: match %u", n, v.match);
    CHECK(spu_interp_depth_get() == 0, "%s: the unwind left the interpreter depth at %d", n, spu_interp_depth_get());
    r = replay(f, SPU_TIER_LIFTED, 1u << 20, &v);
    expect_v("after an exhausted budget, on the same shadow", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);

    /* the exact edge: a budget equal to the transfers made is enough, one fewer is not */
    r = replay(f, SPU_TIER_LIFTED, J[k].spent, &v);
    expect_v("budget equal to the loop's transfers", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    r = replay(f, SPU_TIER_LIFTED, J[k].spent - 1, &v);
    expect_v("budget one short", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);

    /* no budget: the production body, which the twin's counter never touches */
    r = replay(f, SPU_TIER_LIFTED, 0, &v);
    expect_v("no budget", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    CHECK(v.budget == 0 && v.budget_spent == 0, "%s: no budget: limit %llu spent %llu", n, (unsigned long long)v.budget,
          (unsigned long long)v.budget_spent);

    /* the loop that would not end in a test's lifetime: 2^32 laps */
    fixture_laps(f, 0);
    r = replay(f, SPU_TIER_LIFTED, 5000, &v);
    expect_v("endless", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
    CHECK(v.budget_spent == 5000, "%s: endless: spent %llu", n, (unsigned long long)v.budget_spent);
    fixture_laps(f, LAPS);
    r = replay(f, SPU_TIER_LIFTED, 1u << 20, &v);
    expect_v("after the endless loop", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);

    /* the interpreter's own budget, unchanged, on the same fixture */
    r = replay(f, SPU_TIER_INTERP, 10, &v);
    expect_v("T0 over budget", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
    CHECK(v.budget == 10 && v.budget_spent == 10, "%s: T0: limit %llu spent %llu", n, (unsigned long long)v.budget,
          (unsigned long long)v.budget_spent);
}

/* Re-registering the body drops its twin: a twin belongs to the body it was registered for. */
static void t_reregister(void)
{
    spu_xdiff_verdict_record v;
    const uint64_t fp = F[0].identity.image_fingerprint;
    spu_workload_register_img(fp, J[0].prod, PROD_IMAGE, "a");
    CHECK(spu_workload_find_twin(fp, NULL) == NULL, "a re-registered body kept its twin");
    uint32_t r = replay(&F[0], SPU_TIER_LIFTED, 10, &v);
    expect_v("re-registered body, budget 10", r, &v, SPU_XDIFF_EQUAL, SPU_XDIFF_R_NONE);
    spu_workload_register_twin(fp, J[0].twin, TWIN_IMAGE);
    r = replay(&F[0], SPU_TIER_LIFTED, 10, &v);
    expect_v("twin registered again", r, &v, SPU_XDIFF_VOID, SPU_XDIFF_R_BUDGET);
}

#if !defined(_WIN32)
/* Runs spu_job_replay_main in a child process with its output captured; returns its exit status. */
static int run_runner(const char* const* args, int nargs, char* out, size_t outn)
{
    char* argv[16];
    int po[2], st = 0;
    size_t lo = 0;
    REQUIRE(nargs < 14, "too many runner arguments");
    argv[0] = (char*)"spu_job_replay";
    for (int i = 0; i < nargs; i++) argv[i + 1] = (char*)args[i];
    argv[nargs + 1] = NULL;
    REQUIRE(pipe(po) == 0, "pipe");
    fflush(NULL);
    const pid_t pid = fork();
    REQUIRE(pid >= 0, "fork");
    if (pid == 0) {
        dup2(po[1], 1);
        close(po[0]); close(po[1]);
        const int rc = spu_job_replay_main(nargs + 1, argv);
        fflush(NULL);
        _exit(rc);
    }
    close(po[1]);
    for (ssize_t n; lo < outn - 1 && (n = read(po[0], out + lo, outn - 1 - lo)) > 0;) lo += (size_t)n;
    out[lo] = 0;
    close(po[0]);
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1000 - (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static void t_runner(void)
{
    char dir[128], path[160], out[8192];
    const char* t = getenv("TMPDIR");
    spu_xdiff_error err;
    snprintf(dir, sizeof dir, "%s/xb_XXXXXX", t && t[0] ? t : "/tmp");
    REQUIRE(mkdtemp(dir) != NULL, "mkdtemp");
    snprintf(path, sizeof path, "%s/loop.spujob", dir);
    memset(&err, 0, sizeof err);
    REQUIRE(spu_xdiff_fixture_write(&F[0], path, &err) == 0, "writing a fixture: %s", err.detail);

    const char* over[] = { "--json", "--tier", "registered", "--budget", "10", path };
    int rc = run_runner(over, 6, out, sizeof out);
    CHECK(rc == 5, "runner over budget: exit status %d, expected 5: %s", rc, out);
    CHECK(strstr(out, "\"verdict\":\"void\"") && strstr(out, "\"reason\":\"budget\"") &&
          strstr(out, "\"budget\":{\"limit\":10,\"spent\":10}"), "runner over budget: %s", out);

    const char* under[] = { "--json", "--tier", "registered", "--budget", "1048576", path };
    rc = run_runner(under, 6, out, sizeof out);
    CHECK(rc == 0 && strstr(out, "\"verdict\":\"equal\"") && strstr(out, "\"budget\":{\"limit\":1048576,\"spent\":999}"),
          "runner under budget: exit status %d: %s", rc, out);

    const char* none[] = { "--json", path };   /* the default tier is the registered body; no budget: the production body */
    rc = run_runner(none, 2, out, sizeof out);
    CHECK(rc == 0 && strstr(out, "\"verdict\":\"equal\"") && strstr(out, "\"budget\":{\"limit\":0,\"spent\":0}"),
          "runner without a budget: exit status %d: %s", rc, out);
    remove(path);
    rmdir(dir);
}
#endif

int main(void)
{
    REQUIRE(spu_xdiff_refused_knob() == NULL, "a refused knob is set: %s", spu_xdiff_refused_knob());
    spu_begin_image(PROD_IMAGE); xb_spu_recomp_register();
    spu_begin_image(TWIN_IMAGE); xb_spu_recomp_register_budget();
    spu_begin_image(0);

    for (size_t k = 0; k < NJOBS; k++) {
        record(&F[k], &J[k], J[k].image_bytes);
        spu_workload_register_img(F[k].identity.image_fingerprint, J[k].prod, PROD_IMAGE, J[k].name);
        spu_workload_register_twin(F[k].identity.image_fingerprint, J[k].twin, TWIN_IMAGE);
    }
    t_registry();
    for (size_t k = 0; k < NJOBS; k++) t_job(k);
    t_reregister();
#if !defined(_WIN32)
    t_runner();
#else
    printf("SKIP: the runner cases need fork\n");
#endif

    spu_xdiff_shadow_free(g_shadow);
    for (size_t k = 0; k < NJOBS; k++) spu_xdiff_fixture_free(&F[k]);
    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("OK: xdiff budget (twin registry, counted sites, exhaustion, runner)\n");
    return 0;
}
