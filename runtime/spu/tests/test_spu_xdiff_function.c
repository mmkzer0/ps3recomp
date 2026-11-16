/* Synthetic end-to-end T2 function units: capture a T0 caller twice, then
 * compare its lifted reference and a title-style native override. */
#include "spu_context.h"
#include "spu_dma.h"
#include "spu_interp.h"
#include "spu_workload.h"
#include "spurs_job.h"
#include "spu_coherency.h"
#include "xdiff/spu_xdiff_recorder.h"
#include "xdiff/spu_xdiff_replay.h"
#include "ps3emu/lifter_abi.h"
#include "spu_recomp.h"

#define FN_OVERRIDE_BASE 0x7C00u
#define FN_OVERRIDE_ENTRY (FN_OVERRIDE_BASE + 0x40u)
#define FN_OVERRIDE_FUNCTION (FN_OVERRIDE_BASE + 0x70u)
#define FN_OVERRIDE_ENTRY_SP 0x3F000u

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_memory[0x40000];
uint8_t* vm_base = g_guest_memory;
uint32_t ppu_vm_size = sizeof g_guest_memory;
const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* ctx, uint32_t a, uint32_t b, uint32_t c)
{
    (void)ctx; (void)a; (void)b; (void)c;
}
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

void xfn_spu_recomp_register(void);
void xfn_spu_func_00007C40(spu_context* ctx);
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
void spu_begin_image(int image_id);

static int g_native_mode;
enum {
    NATIVE_CORRECT,
    NATIVE_WRONG_LIVE,
    NATIVE_DEAD_REGISTER,
    NATIVE_DEAD_STACK,
    NATIVE_WRONG_LS,
    NATIVE_WRONG_PC
};

void test_fn_override(spu_context* ctx)
{
    for (uint32_t lane = 0; lane < 4; lane++) {
        uint32_t value = ctx->gpr[3]._u32[lane] + ctx->gpr[4]._u32[lane];
        if (g_native_mode == NATIVE_WRONG_LIVE) value++;
        ctx->gpr[5]._u32[lane] = value;
        if (g_native_mode == NATIVE_DEAD_REGISTER) ctx->gpr[6]._u32[lane] = 0xDEADBEEFu;
    }
    if (g_native_mode == NATIVE_DEAD_STACK)
        ctx->ls[(ctx->gpr[1]._u32[0] - 1u) & SPU_LS_MASK] = 0x6Du;
    if (g_native_mode == NATIVE_WRONG_LS) ctx->ls[0x18000u] ^= 1u;
    ctx->pc = ctx->gpr[0]._u32[0]; /* match the leaf's `bi $r0` return transfer */
    if (g_native_mode == NATIVE_WRONG_PC) ctx->pc = (ctx->pc + 4u) & SPU_LS_MASK;
}

#define REQUIRE(cond, ...)                                                                  \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                            \
            fprintf(stderr, __VA_ARGS__);                                                    \
            fputc('\n', stderr);                                                            \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static uint8_t* read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    const long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    uint8_t* bytes = (uint8_t*)malloc(length ? (size_t)length : 1u);
    if (!bytes) { fclose(file); return NULL; }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static void interpreter_entry(spu_context* ctx)
{
    spu_interp_run(ctx, ctx->pc);
}

static void record_job(const uint8_t* image, size_t image_bytes, spu_xdiff_fixture* fixture)
{
    static spu_context ctx;
    spu_xdiff_unit unit;
    spu_xdiff_body_id authority;
    spu_xdiff_recorder recorder;
    spu_context_init(&ctx, 0);
    ctx.image_id = -1; /* T0 must execute the staged bytes, never rejoin this image's lift. */
    ctx.pc = FN_OVERRIDE_ENTRY;
    ctx.gpr[1]._u32[0] = FN_OVERRIDE_ENTRY_SP;
    memcpy(ctx.ls + FN_OVERRIDE_BASE, image, image_bytes);
    memset(&unit, 0, sizeof unit);
    unit.kind = SPU_XDIFF_UNIT_JOB;
    unit.profile = SPU_XDIFF_PROFILE_JOBCHAIN;
    unit.entry_pc = FN_OVERRIDE_ENTRY;
    unit.return_ls = SPU_JOB_RETURN_LINK_LS;
    unit.image_ls = FN_OVERRIDE_BASE;
    unit.image_bytes = (uint32_t)image_bytes;
    unit.r1 = FN_OVERRIDE_ENTRY_SP;
    memset(&authority, 0, sizeof authority);
    authority.tier = (uint8_t)SPU_TIER_INTERP;
    REQUIRE(spu_xdiff_fixture_init(fixture) == 0, "fixture init");
    REQUIRE(spu_xdiff_recorder_begin(&recorder, fixture, &ctx, &unit, &authority,
                                     NULL, NULL, 0) == 0,
            "recorder begin: %s", recorder.error.detail);
    const int halted = spu_run_with_halt(interpreter_entry, &ctx);
    REQUIRE(!halted, "T0 job halted instead of stopping normally");
    const spu_job_end end = spu_job_classify_end(&ctx);
    const uint32_t failed = spu_xdiff_recorder_end(&recorder, &ctx, end.end_class, end.stop_code);
    REQUIRE(failed == SPU_XDIFF_R_NONE, "recorder end: %s: %s",
            spu_xdiff_reason_id(failed), recorder.error.detail);
    spu_coh_unregister(&ctx);
    spu_mfc_release(&ctx);
}

static uint32_t replay_job(const spu_xdiff_fixture* fixture, spu_xdiff_verdict_record* verdict)
{
    spu_xdiff_replay_options options = { (uint32_t)SPU_TIER_LIFTED, 0 };
    spu_xdiff_shadow* shadow = spu_xdiff_shadow_new();
    REQUIRE(shadow != NULL, "job shadow");
    const uint32_t result = spu_xdiff_replay(fixture, &options, shadow, verdict);
    spu_xdiff_shadow_free(shadow);
    return result;
}

static void run_replay_cli(const char* job_path, uint32_t expected)
{
    char* argv[] = { (char*)"spu_job_replay", (char*)"--tier", (char*)"registered", (char*)job_path };
    REQUIRE(spu_job_replay_main(4, argv) == (int)expected,
            "spu_job_replay --tier registered returned the wrong verdict");
}

static int run_function_cli(const char* unit_path, const char* symbol)
{
    char* argv[] = { (char*)"spu_job_replay", (char*)"--fn-unit", (char*)unit_path,
                     (char*)"--fn-symbol", (char*)symbol };
    return spu_job_replay_main(5, argv);
}

static void verify_function_unit(const char* unit_path, uint32_t expected_return_pc)
{
    spu_xdiff_fixture unit;
    spu_xdiff_error error;
    spu_xdiff_function_result result;
    memset(&unit, 0, sizeof unit);
    REQUIRE(spu_xdiff_fixture_read(unit_path, &unit, &error) == 0,
            "read function unit: %s", error.detail);
    REQUIRE(unit.unit.kind == SPU_XDIFF_UNIT_FUNCTION, "capture did not write a function unit");
    REQUIRE(unit.unit.return_ls == expected_return_pc,
            "capture lost return PC: got 0x%x want 0x%x",
            (unsigned)unit.unit.return_ls, (unsigned)expected_return_pc);
    REQUIRE((unit.function_mask.live_out[0] & ((1ULL << 1) | (1ULL << 5))) ==
                ((1ULL << 1) | (1ULL << 5)), "liveness mask lost $r1 or live-out $r5");

    g_native_mode = NATIVE_CORRECT;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 0, &result) == SPU_XDIFF_EQUAL,
            "lifted function replay: %s", spu_xdiff_reason_id(result.reason));
    REQUIRE(run_function_cli(unit_path, "lifted") == SPU_XDIFF_EQUAL,
            "spu_job_replay --fn-symbol lifted was not equal");
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_EQUAL,
            "native function replay: %s", spu_xdiff_reason_id(result.reason));
    REQUIRE(run_function_cli(unit_path, "native") == SPU_XDIFF_EQUAL,
            "spu_job_replay --fn-symbol native was not equal");

    g_native_mode = NATIVE_WRONG_LIVE;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_DIFFER,
            "wrong live-out override did not differ");
    REQUIRE(result.reason == SPU_XDIFF_R_END_REGS && result.where == 5,
            "wrong live-out override was not attributed to $r5: %s / 0x%x",
            spu_xdiff_reason_id(result.reason), (unsigned)result.where);
    REQUIRE(memcmp(result.want, result.got, sizeof result.want) != 0,
            "register verdict omitted the differing bytes");
    REQUIRE(run_function_cli(unit_path, "native") == SPU_XDIFF_DIFFER,
            "spu_job_replay did not report the wrong live register");

    g_native_mode = NATIVE_DEAD_REGISTER;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_EQUAL,
            "dead-register override failed the live-state comparison");
    REQUIRE(result.dead_registers > 0, "dead-register difference was not counted");
    REQUIRE(run_function_cli(unit_path, "native") == SPU_XDIFF_EQUAL,
            "spu_job_replay did not accept a dead-register-only difference");

    g_native_mode = NATIVE_DEAD_STACK;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_EQUAL,
            "dead stack extent was not masked");

    g_native_mode = NATIVE_WRONG_LS;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_DIFFER,
            "out-of-frame LS write did not differ");
    REQUIRE(result.reason == SPU_XDIFF_R_END_LS && result.where == 0x18000u,
            "LS difference was not attributed to 0x18000: %s / 0x%x",
            spu_xdiff_reason_id(result.reason), (unsigned)result.where);

    g_native_mode = NATIVE_WRONG_PC;
    REQUIRE(spu_xdiff_replay_function_unit(&unit, 1, &result) == SPU_XDIFF_DIFFER,
            "wrong function return PC was reported equal");
    REQUIRE(result.reason == SPU_XDIFF_R_END_PC && result.where == expected_return_pc,
            "wrong PC was not attributed to return 0x%x: %s / 0x%x",
            (unsigned)expected_return_pc, spu_xdiff_reason_id(result.reason),
            (unsigned)result.where);
    REQUIRE(memcmp(result.want, result.got, 4u) != 0, "return-PC verdict omitted want/got");
    REQUIRE(run_function_cli(unit_path, "native") == SPU_XDIFF_DIFFER,
            "spu_job_replay did not report the wrong return PC");
    spu_xdiff_fixture_free(&unit);
}

static void verify_capture_requires_fingerprint(const spu_xdiff_fixture* fixture,
                                                const char* plan_path, const char* out_dir)
{
    spu_xdiff_poison_plan plan;
    char error[256];
    uint32_t captured = 99;
    memset(&plan, 0, sizeof plan);
    REQUIRE(spu_xdiff_poison_plan_load(plan_path, &plan, error, sizeof error) == 0,
            "load liveness plan: %s", error);
    plan.has_fingerprint = 0;
    REQUIRE(spu_xdiff_capture_function_units(fixture, &plan, FN_OVERRIDE_FUNCTION,
                                             out_dir, &captured, error, sizeof error) != 0,
            "function capture accepted a plan without an image fingerprint");
    REQUIRE(strstr(error, "fingerprint") != NULL,
            "missing fingerprint refusal was not named: %s", error);
    spu_xdiff_poison_plan_free(&plan);

    REQUIRE(spu_xdiff_poison_plan_load(plan_path, &plan, error, sizeof error) == 0,
            "reload liveness plan: %s", error);
    plan.image_fingerprint ^= 1u;
    REQUIRE(spu_xdiff_capture_function_units(fixture, &plan, FN_OVERRIDE_FUNCTION,
                                             out_dir, &captured, error, sizeof error) != 0,
            "function capture accepted a mismatched image fingerprint");
    REQUIRE(strstr(error, "fingerprint") != NULL,
            "mismatched fingerprint refusal was not named: %s", error);
    spu_xdiff_poison_plan_free(&plan);
}

static void write_liveness_plan(const char* path, int has_fingerprint, uint64_t fingerprint)
{
    FILE* file = fopen(path, "wb");
    REQUIRE(file != NULL, "open synthetic liveness plan %s", path);
    fputs("{\"schema\":\"ps3recomp.spu-liveness/1\",", file);
    if (has_fingerprint)
        fprintf(file, "\"image\":{\"fingerprint\":\"0x%016llx\"},",
                (unsigned long long)fingerprint);
    fprintf(file, "\"functions\":[{\"entry\":\"0x%05x\"}]}\n",
            (unsigned)FN_OVERRIDE_FUNCTION);
    REQUIRE(fclose(file) == 0, "close synthetic liveness plan %s", path);
}

static int run_capture_cli(const char* plan_path, const char* fn_text,
                           const char* out_dir, const char* fixture_path)
{
    char* argv[] = { (char*)"spu_job_replay", (char*)"--tier", (char*)"interp",
                     (char*)"--capture-fn", (char*)plan_path, (char*)"--fn", (char*)fn_text,
                     (char*)"--out", (char*)out_dir, (char*)fixture_path };
    return spu_job_replay_main(10, argv);
}

static void verify_capture_cli_requires_fingerprint(const spu_xdiff_fixture* fixture,
                                                    const char* plan_base, const char* fn_text,
                                                    const char* out_dir, const char* fixture_path)
{
    char plan_path[1024];
    int n = snprintf(plan_path, sizeof plan_path, "%s.missing-fingerprint", plan_base);
    REQUIRE(n > 0 && (size_t)n < sizeof plan_path, "missing-fingerprint plan path too long");
    write_liveness_plan(plan_path, 0, 0);
    const int missing_rc = run_capture_cli(plan_path, fn_text, out_dir, fixture_path);
    remove(plan_path);
    REQUIRE(missing_rc != 0, "--capture-fn accepted a plan without image.fingerprint");

    n = snprintf(plan_path, sizeof plan_path, "%s.mismatched-fingerprint", plan_base);
    REQUIRE(n > 0 && (size_t)n < sizeof plan_path, "mismatched-fingerprint plan path too long");
    write_liveness_plan(plan_path, 1, fixture->identity.image_fingerprint ^ 1u);
    const int mismatched_rc = run_capture_cli(plan_path, fn_text, out_dir, fixture_path);
    remove(plan_path);
    REQUIRE(mismatched_rc != 0, "--capture-fn accepted a mismatched image.fingerprint");
}

int main(int argc, char** argv)
{
    REQUIRE(argc == 5, "image plan job-fixture unit-directory arguments required");
    size_t image_bytes = 0;
    uint8_t* image = read_file(argv[1], &image_bytes);
    REQUIRE(image != NULL, "read synthetic image %s", argv[1]);
    const uint64_t fingerprint = spu_workload_fingerprint(image, image_bytes);

    spu_begin_image(17);
    xfn_spu_recomp_register();
    spu_begin_image(0);
    spu_workload_register_img(fingerprint, xfn_spu_func_00007C40, 17, "synthetic-t2-function");
    spu_workload_set_body_identity(fingerprint, (uint8_t)SPU_TIER_PARTIAL, 0x1234u);

    spu_xdiff_fixture fixture;
    record_job(image, image_bytes, &fixture);
    free(image);
    REQUIRE(fixture.identity.image_fingerprint == fingerprint, "recorded fingerprint differs from registry");
    spu_xdiff_error error;
    REQUIRE(spu_xdiff_fixture_write(&fixture, argv[3], &error) == 0,
            "write T0 fixture: %s", error.detail);

    spu_xdiff_verdict_record verdict;
    g_native_mode = NATIVE_CORRECT;
    REQUIRE(replay_job(&fixture, &verdict) == SPU_XDIFF_EQUAL,
            "correct T2 whole-job replay: %s", spu_xdiff_reason_id(verdict.reason));
    run_replay_cli(argv[3], SPU_XDIFF_EQUAL);
    REQUIRE(verdict.subject_body.tier == SPU_TIER_PARTIAL && verdict.subject_body.native_rev == 0x1234u,
            "registered T2 identity did not carry tier/revision");

    g_native_mode = NATIVE_WRONG_LIVE;
    REQUIRE(replay_job(&fixture, &verdict) == SPU_XDIFF_DIFFER,
            "wrong T2 whole-job replay did not differ");
    run_replay_cli(argv[3], SPU_XDIFF_DIFFER);

    verify_capture_requires_fingerprint(&fixture, argv[2], argv[4]);

    char fn_text[16];
    snprintf(fn_text, sizeof fn_text, "0x%x", (unsigned)FN_OVERRIDE_FUNCTION);
    verify_capture_cli_requires_fingerprint(&fixture, argv[2], fn_text, argv[4], argv[3]);
    char* capture_argv[] = { (char*)"spu_job_replay", (char*)"--tier", (char*)"interp",
                             (char*)"--capture-fn", argv[2], (char*)"--fn", fn_text,
                             (char*)"--out", argv[4], argv[3] };
    REQUIRE(spu_job_replay_main(10, capture_argv) == 0,
            "spu_job_replay --capture-fn failed");

    char unit_path[1024];
    int unit_path_len = snprintf(unit_path, sizeof unit_path,
                                       "%s/%016llx-f%08x-c%02u.spujob", argv[4],
                                       (unsigned long long)fingerprint,
                                       (unsigned)FN_OVERRIDE_FUNCTION, 0u);
    REQUIRE(unit_path_len > 0 && (size_t)unit_path_len < sizeof unit_path, "unit path too long");
    verify_function_unit(unit_path, FN_OVERRIDE_ENTRY + 12u);
    unit_path_len = snprintf(unit_path, sizeof unit_path,
                             "%s/%016llx-f%08x-c%02u.spujob", argv[4],
                             (unsigned long long)fingerprint,
                             (unsigned)FN_OVERRIDE_FUNCTION, 1u);
    REQUIRE(unit_path_len > 0 && (size_t)unit_path_len < sizeof unit_path, "second unit path too long");
    verify_function_unit(unit_path, FN_OVERRIDE_ENTRY + 16u);

    spu_xdiff_fixture_free(&fixture);
    printf("PASS test_spu_xdiff_function\n");
    return 0;
}
