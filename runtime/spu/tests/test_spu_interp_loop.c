/* The interpreter loop has a fast path for pure interpretation with nothing
 * observing it, and a general path for everything else. Both must leave the
 * same registers, local store, pc, stop code, status, return value and the
 * g_spu_interp_steps / g_spu_interp_last_pc diagnostics. Each synthetic
 * program below runs in every mode against literal expectations worked out by
 * hand from the ISA, so a mode that agrees with the others but is wrong fails
 * too. The modes:
 *   fast   image_id < 0, no observer, no diagnostic
 *   obs    an observer whose on_insn never yields (forces the general loop)
 *   diag   the diagnostic flag set with nothing to count (forces it too)
 *   img    image_id >= 0 with no lifted function registered (general loop)
 * Hand-encoded instructions only; no title or firmware bytes.
 *
 * The fast loop keeps pc and the step count in locals, so ctx->pc is stale
 * between fast steps and must be current wherever something reads it. The
 * programs below pin those places by what the reader sees: the channel ops
 * (spu_rdch/spu_wrch/spu_rchcnt), the local-store accesses (the SPU_LS_WATCH
 * diagnostic prints the pc), the observer, the exits through stop, halt and an
 * unimplemented op, a blocking channel that leaves the run by longjmp, and a
 * channel that runs a nested interpreter on the same context. */
#include "../spu_interp.c"
#include <pthread.h>
#include <string.h>
#include <setjmp.h>

/* Channels play the out-of-line callers. They record the pc the context holds
 * when called (the instruction making the call), and act on a few numbers:
 *   CH_SWITCH_IMAGE  changes the context's image, as the DMA command that
 *                    loads the taskset policy does (spu_dma.h);
 *   CH_HALT          leaves the run by longjmp, as spu_halt does;
 *   CH_NEST          runs a nested interpreter at 0x400 on the same context,
 *                    as a channel command that dispatches into lifted code. */
enum { CH_SWITCH_IMAGE = 70, CH_PEEK = 71, CH_HALT = 72, CH_NEST = 73 };
static int g_switch_calls;
static uint32_t g_ch_pc[8]; static int g_ch_n;
static jmp_buf g_halt_env;
static int g_nest_runs; static uint32_t g_nest_ret, g_nest_steps, g_nest_last_pc, g_nest_steps_total;
static void chan(spu_context* c, uint32_t ch) {
    if (ch == CH_SWITCH_IMAGE) { g_switch_calls++; c->image_id = 5; return; }
    if (ch == CH_PEEK) { if (g_ch_n < 8) g_ch_pc[g_ch_n++] = c->pc; return; }
    if (ch == CH_HALT) longjmp(g_halt_env, 1);
    if (ch == CH_NEST) {
        g_nest_ret = spu_interp_run_until(c, 0x400, 0);
        g_nest_steps = (uint32_t)g_spu_interp_steps;
        g_nest_last_pc = g_spu_interp_last_pc;
        g_nest_steps_total += g_nest_steps;
        g_nest_runs++;
        return;
    }
    abort();
}
u128 spu_rdch(spu_context* c, uint32_t ch) { chan(c, ch); return spu_splat_u32(0xC0DE); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { chan(c, ch); return 3; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)v; chan(c, ch); }

/* spu_ls_read128/write128 call this with ctx->pc while a watch is armed. */
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
static struct { int write; uint32_t pc; } g_ls_hit[16]; static int g_ls_n;
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)p; (void)lr;
    if (g_ls_n < 16) { g_ls_hit[g_ls_n].write = w; g_ls_hit[g_ls_n].pc = pc; g_ls_n++; }
}
static int g_arm_watch;

/* What run_mode reports for a run that left by longjmp instead of returning. */
#define HALT_LONGJMP 0xBAD0BAD0u

/* The registry knows one lifted entry. Only a context with image_id >= 0
 * may be asked: pure interpretation must never reach the registry. */
static uint32_t g_lifted_pc;
static int g_lookup_calls, g_lookup_with_pure_image;
static void lifted_stub(spu_context* c) { (void)c; abort(); }
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t pc) {
    g_lookup_calls++;
    if (c->image_id < 0) g_lookup_with_pure_image++;
    return (g_lifted_pc && pc == g_lifted_pc) ? lifted_stub : NULL;
}

/* The HLE hook at 0xA70: records the image it runs under and answers in r7. */
static int g_hle_calls, g_hle_image;
void spu_spurs_taskset_syscall(spu_context* c) {
    g_hle_calls++; g_hle_image = c->image_id;
    c->gpr[7] = spu_splat_u32(0x77);
}

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d [%s/%s]: %s\n", __FILE__, __LINE__, g_case, g_mode, #cond); \
    exit(1); } } while (0)
static const char *g_case, *g_mode;

/* ISA layouts (IBM SPU ISA v1.2, ch. 2). */
static uint32_t ri16(uint32_t op9, uint32_t i16, uint32_t rt) {
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt;
}
static uint32_t ri10(uint32_t op8, uint32_t i10, uint32_t ra, uint32_t rt) {
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt;
}
static uint32_t rr(uint32_t op11, uint32_t ra, uint32_t rt) { return (op11 << 21) | (ra << 7) | rt; }
static uint32_t rrb(uint32_t op11, uint32_t rb, uint32_t ra, uint32_t rt) {
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt;
}
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put(spu_context* c, uint32_t at, uint32_t insn) { be32(c->ls + at, insn); }

enum { IL = 0x081, BRNZ = 0x042, BRSL = 0x066, AI = 0x1C, BI = 0x1A8, WRCH = 0x10D,
       RDCH = 0x00D, RCHCNT = 0x00F,
       LQD = 0x34, STQD = 0x24, LQX = 0x1C4, STQX = 0x144,
       LQA = 0x61, LQR = 0x67, STQA = 0x41, STQR = 0x47,
       HEQI = 0x7F, HGTI = 0x4F, HLGTI = 0x5F };
static uint32_t il(uint32_t rt, uint32_t v) { return ri16(IL, v, rt); }
static uint32_t ai(uint32_t rt, uint32_t ra, int v) { return ri10(AI, (uint32_t)v, ra, rt); }
static uint32_t stop(uint32_t code) { return code & 0x3FFF; }

/* Counted loop, a call to LS 0xA70, then a stop:
 *   100 il r2,5
 *   104 ai r3,r3,3          loop: r3 += 3, r2 -= 1, again while r2 != 0
 *   108 ai r2,r2,-1
 *   10C brnz r2,0x104
 *   110 brsl r0,0xA70       link r0 = 0x114
 *   114 il r8,0x66
 *   118 stop 0x123
 *   A70 il r9,0x55          what runs there when the HLE hook does not
 *   A74 bi r0 */
static void load_call_program(spu_context* c) {
    put(c, 0x100, il(2, 5));
    put(c, 0x104, ai(3, 3, 3));
    put(c, 0x108, ai(2, 2, -1));
    put(c, 0x10C, ri16(BRNZ, 0xFFFE, 2));
    put(c, 0x110, ri16(BRSL, (0xA70 - 0x110) >> 2, 0));
    put(c, 0x114, il(8, 0x66));
    put(c, 0x118, stop(0x123));
    put(c, 0xA70, il(9, 0x55));
    put(c, 0xA74, rr(BI, 0, 0));
}

/* stop 0x110 is the receive-event request the loop services itself:
 *   100 il r2,1 / 104 stop 0x110 / 108 il r3,9 / 10C stop 0x45 */
static void load_event_program(spu_context* c) {
    put(c, 0x100, il(2, 1));
    put(c, 0x104, stop(0x110));
    put(c, 0x108, il(3, 9));
    put(c, 0x10C, stop(0x45));
}

/* A step that changes image_id, then an instruction the registry owns:
 *   100 il r2,3 / 104 wrch CH,r2 / 108 il r4,1 / 10C stop 0 */
static void load_switch_program(spu_context* c) {
    put(c, 0x100, il(2, 3));
    put(c, 0x104, rr(WRCH, CH_SWITCH_IMAGE, 2));
    put(c, 0x108, il(4, 1));
    put(c, 0x10C, stop(0));
}

typedef struct { int calls, pc_stale; } obs_count;
static int obs_on_insn(void* user, spu_context* c, uint32_t pc) {
    obs_count* oc = user;
    oc->calls++;
    if (c->pc != pc) oc->pc_stale++;   /* the callback is told the pc ctx->pc holds */
    return 0;
}

static spu_context g_ctx;     /* 256 KiB of local store: not on the stack */

enum { M_FAST, M_OBS, M_DIAG, M_IMG, M_COUNT };
static const char* const mode_name[M_COUNT] = { "fast", "obs", "diag", "img" };

typedef struct { uint32_t ret, steps, last_pc; } result;

/* Prepare a context in `mode`, run `prog` from 0x100 (stop_lsa as given) and
 * return what the run left. The caller examines the context. */
static result run_mode(int mode, void (*prog)(spu_context*), uint32_t stop_lsa,
                       int policy_hle, obs_count* oc) {
    static const spu_observer obs_tmpl = { 0, obs_on_insn, 0, 0, 0 };
    static spu_observer obs;
    spu_context* c = &g_ctx;
    memset(c, 0, sizeof *c);
    spu_context_init(c, 0);
    c->image_id = (mode == M_IMG) ? 7 : -1;
    if (policy_hle) {
        c->policy_mode = 1;
        be32(c->ls + 0x27C4, 0xA70);   /* the policy's syscall slot names the HLE */
    }
    prog(c);
    memset(&t_interp_diag, 0, sizeof t_interp_diag);
    t_interp_diag.init = 1;            /* the environment must not pick the mode */
    if (mode == M_DIAG) t_interp_diag.diag = 1;
    if (mode == M_OBS) { obs = obs_tmpl; obs.user = oc; spu_observer_set(c, &obs); }
    g_hle_calls = g_hle_image = g_lookup_calls = g_lookup_with_pure_image = g_switch_calls = 0;
    g_ch_n = g_ls_n = g_nest_runs = 0; g_nest_steps_total = 0;
    g_spu_ls_watch_n = g_arm_watch; g_spu_ls_rdiag = g_spu_ls_wdiag = g_arm_watch;
    g_spu_interp_steps = 0xDEADBEEFDEADBEEFull;   /* must be rewritten by the run */
    g_spu_interp_last_pc = 0xFEEDFACEu;
    result r;
    if (setjmp(g_halt_env) == 0) {
        r.ret = spu_interp_run_until(c, 0x100, stop_lsa);
    } else {
        r.ret = HALT_LONGJMP;
        s_interp_depth = 0;   /* the frames the longjmp skipped never decremented it */
    }
    r.steps = (uint32_t)g_spu_interp_steps;
    r.last_pc = g_spu_interp_last_pc;
    CHECK(r.ret == HALT_LONGJMP || g_spu_interp_steps >> 32 == 0);
    CHECK(mode != M_OBS || (uint32_t)oc->calls == r.steps + g_nest_steps_total || r.ret == HALT_LONGJMP);
    CHECK(oc->pc_stale == 0);
    CHECK(g_lookup_with_pure_image == 0);   /* pure interpretation never asks the registry */
    g_spu_ls_watch_n = 0; g_spu_ls_rdiag = g_spu_ls_wdiag = 0;
    return r;
}

/* Everything a run leaves, compared across modes. */
typedef struct {
    result r; u128 gpr[128]; uint32_t pc, stop_code, status; int image_id, hle_calls, hle_image;
    uint8_t* ls;
} snap;
static void take(snap* s, result r) {
    s->r = r;
    memcpy(s->gpr, g_ctx.gpr, sizeof s->gpr);
    s->pc = g_ctx.pc; s->stop_code = g_ctx.stop_code; s->status = g_ctx.status;
    s->image_id = g_ctx.image_id; s->hle_calls = g_hle_calls; s->hle_image = g_hle_image;
    s->ls = malloc(SPU_LS_SIZE);
    CHECK(s->ls);
    memcpy(s->ls, g_ctx.ls, SPU_LS_SIZE);
}
static void same(const snap* a, const snap* b) {
    CHECK(a->r.ret == b->r.ret);
    CHECK(a->r.ret == HALT_LONGJMP || (a->r.steps == b->r.steps && a->r.last_pc == b->r.last_pc));
    CHECK(!memcmp(a->gpr, b->gpr, sizeof a->gpr));
    CHECK(!memcmp(a->ls, b->ls, SPU_LS_SIZE));
    CHECK(a->pc == b->pc && a->stop_code == b->stop_code && a->status == b->status);
    CHECK(a->hle_calls == b->hle_calls && a->hle_image == b->hle_image);
    CHECK(a->image_id == b->image_id || a->image_id == 7 || b->image_id == 7);
}

#define R(i) (g_ctx.gpr[i]._u32[0])

/* Run `prog` in every mode; `expect` examines the context of each. */
static void all_modes(const char* name, void (*prog)(spu_context*), uint32_t stop_lsa,
                      int policy_hle, void (*expect)(int mode, result r)) {
    snap first; int have = 0;
    for (int m = 0; m < M_COUNT; ++m) {
        obs_count oc = { 0 };
        g_case = name; g_mode = mode_name[m];
        result r = run_mode(m, prog, stop_lsa, policy_hle, &oc);
        expect(m, r);
        snap s; take(&s, r);
        if (have) { same(&first, &s); free(s.ls); } else { first = s; have = 1; }
    }
    free(first.ls);
}

/* il 1 + loop 5 x 3 + brsl 1 + (A70: il, bi = 2) + il r8 + stop = 21 steps;
 * with the HLE answering at 0xA70 that visit is no step: 19. The last step
 * issued is the stop at 0x118, which leaves pc = 0x11C. */
static void expect_call_hle(int mode, result r) {
    CHECK(r.ret == 0x123 && g_ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    CHECK(g_ctx.pc == 0x11C && g_ctx.stop_code == 0x123);
    CHECK(r.steps == 19 && r.last_pc == 0x118);
    CHECK(R(2) == 0 && R(3) == 15 && R(8) == 0x66);
    CHECK(g_hle_calls == 1 && g_hle_image == 22 && R(7) == 0x77 && R(9) == 0);
    CHECK(g_ctx.image_id == (mode == M_IMG ? 7 : -1));   /* the HLE restores it */
    CHECK(R(0) == 0x114);
}
static void expect_call_native(int mode, result r) {
    CHECK(r.ret == 0x123 && g_ctx.pc == 0x11C);
    CHECK(r.steps == 21 && r.last_pc == 0x118);
    CHECK(R(3) == 15 && R(8) == 0x66);
    CHECK(g_hle_calls == 0 && R(7) == 0 && R(9) == 0x55);
    CHECK(g_ctx.image_id == (mode == M_IMG ? 7 : -1));
}

/* Serviced: the 0x110 stop is not a step that ends the run: 4 steps, final
 * stop 0x45 at 0x10C. */
static void expect_event_serviced(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x45 && g_ctx.pc == 0x110 && g_ctx.stop_code == 0x45);
    CHECK(r.steps == 4 && r.last_pc == 0x10C);
    CHECK(R(2) == 1 && R(3) == 9);
    CHECK(g_ctx.rcv_evt_n == 4 && g_ctx.rcv_evt[0] == 0 && g_ctx.rcv_evt[1] == 0x11111111u
          && g_ctx.rcv_evt[2] == 0x22222222u && g_ctx.rcv_evt[3] == 0x33333333u);
    CHECK(!g_spu_pending_evt_valid);
    CHECK(g_ctx.status == SPU_STATUS_STOPPED_BY_STOP);
}
static void expect_event_unserviced(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x110 && g_ctx.pc == 0x108 && g_ctx.stop_code == 0x110);
    CHECK(r.steps == 2 && r.last_pc == 0x104);
    CHECK(R(2) == 1 && R(3) == 0 && g_ctx.rcv_evt_n == 0);
}

/* il, wrch (switches the image to 5) = 2 steps, then the registry owns 0x108:
 * the run returns that pc. A rejoin records the pc it returns at as last_pc. */
static void expect_switch_rejoin(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x108 && g_ctx.pc == 0x108 && g_ctx.status == SPU_STATUS_RUNNING);
    CHECK(r.steps == 2 && r.last_pc == 0x108);
    CHECK(R(2) == 3 && R(4) == 0);
    CHECK(g_switch_calls == 1 && g_ctx.image_id == 5);
}
/* The same switch with the registry empty, and stop_lsa as the rejoin point. */
static void expect_switch_stop_lsa(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x108 && r.steps == 2 && r.last_pc == 0x108 && g_ctx.image_id == 5);
    CHECK(R(4) == 0);
}
/* image_id never changes: the run is to the stop (the switch case in img mode
 * starts at 7, and wrch moves it to 5 all the same). */
static void expect_switch_runs_on(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0 && g_ctx.pc == 0x110 && r.steps == 4 && r.last_pc == 0x10C);
    CHECK(R(4) == 1 && g_ctx.image_id == 5);
}

/* il r2,3 / rdch r3,PEEK / wrch PEEK,r2 / rchcnt r4,PEEK / stop 7: every
 * channel call sees the pc of its own instruction in ctx->pc. */
static void load_chan_program(spu_context* c) {
    put(c, 0x100, il(2, 3));
    put(c, 0x104, rr(RDCH, CH_PEEK, 3));
    put(c, 0x108, rr(WRCH, CH_PEEK, 2));
    put(c, 0x10C, rr(RCHCNT, CH_PEEK, 4));
    put(c, 0x110, stop(7));
}
static void expect_chan_pcs(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 7 && g_ctx.pc == 0x114 && r.steps == 5 && r.last_pc == 0x110);
    CHECK(g_ch_n == 3 && g_ch_pc[0] == 0x104 && g_ch_pc[1] == 0x108 && g_ch_pc[2] == 0x10C);
    CHECK(R(2) == 3 && R(3) == 0xC0DE && R(4) == 3);
}

/* The switch program through the other two channel ops: rdch and rchcnt can
 * change the image as well, and the run must notice before the next step.
 *   100 il r2,3 / 104 rdch r5,SWITCH / 108 il r4,1 / 10C stop 0 */
static void load_switch_rdch_program(spu_context* c) {
    put(c, 0x100, il(2, 3));
    put(c, 0x104, rr(RDCH, CH_SWITCH_IMAGE, 5));
    put(c, 0x108, il(4, 1));
    put(c, 0x10C, stop(0));
}
static void load_switch_rchcnt_program(spu_context* c) {
    put(c, 0x100, il(2, 3));
    put(c, 0x104, rr(RCHCNT, CH_SWITCH_IMAGE, 5));
    put(c, 0x108, il(4, 1));
    put(c, 0x10C, stop(0));
}

/* A blocking channel leaves the run by longjmp (spu_halt):
 *   100 il r2,1 / 104 wrch HALT,r2 / 108 il r3,2 / 10C stop 0
 * ctx->pc stays at the wrch, nothing after it ran, and the same context then
 * runs on from 0x108 with totals of its own (steps start at zero). */
static void load_halt_program(spu_context* c) {
    put(c, 0x100, il(2, 1));
    put(c, 0x104, rr(WRCH, CH_HALT, 2));
    put(c, 0x108, il(3, 2));
    put(c, 0x10C, stop(0));
}
static void expect_halt_exit(int mode, result r) {
    (void)mode;
    CHECK(r.ret == HALT_LONGJMP && g_ctx.pc == 0x104 && g_ctx.status == SPU_STATUS_RUNNING);
    CHECK(R(2) == 1 && R(3) == 0);
    uint32_t again = spu_interp_run_until(&g_ctx, 0x108, 0);
    CHECK(again == 0 && g_spu_interp_steps == 2 && g_spu_interp_last_pc == 0x10C);
    CHECK(R(3) == 2 && g_ctx.pc == 0x110 && g_ctx.status == SPU_STATUS_STOPPED_BY_STOP);
}

/* A channel that runs a nested interpreter on the same context:
 *   100 il r2,1 / 104 wrch NEST,r2 / 108 il r3,2 / 10C stop 0x44
 *   400 il r9,4 / 404 il r10,5 / 408 il r11,6 / 40C stop 0x66
 * The nested run publishes its own totals (4 steps, last pc 0x40C); the outer
 * run counts only its own 4 steps and publishes those when it returns. */
static void load_nest_program(spu_context* c) {
    put(c, 0x100, il(2, 1));
    put(c, 0x104, rr(WRCH, CH_NEST, 2));
    put(c, 0x108, il(3, 2));
    put(c, 0x10C, stop(0x44));
    put(c, 0x400, il(9, 4));
    put(c, 0x404, il(10, 5));
    put(c, 0x408, il(11, 6));
    put(c, 0x40C, stop(0x66));
}
static void expect_nest(int mode, result r) {
    (void)mode;
    CHECK(g_nest_runs == 1 && g_nest_ret == 0x66 && g_nest_steps == 4 && g_nest_last_pc == 0x40C);
    CHECK(r.ret == 0x44 && g_ctx.pc == 0x110 && g_ctx.stop_code == 0x44);
    CHECK(r.steps == 4 && r.last_pc == 0x10C);
    CHECK(R(2) == 1 && R(3) == 2 && R(9) == 4 && R(10) == 5 && R(11) == 6);
}

/* Every local-store access reads ctx->pc when a watch is armed (the
 * SPU_LS_WATCH diagnostic prints it):
 *   100 il r2,0x400
 *   104 lqd / 108 lqx / 10C lqa / 110 lqr / 114 stqd / 118 stqx / 11C stqa /
 *   120 stqr / 124 stop 0 */
static void load_mem_program(spu_context* c) {
    put(c, 0x100, il(2, 0x400));
    put(c, 0x104, ri10(LQD, 0, 2, 3));
    put(c, 0x108, rrb(LQX, 0, 2, 4));
    put(c, 0x10C, ri16(LQA, 0x40, 5));
    put(c, 0x110, ri16(LQR, 2, 6));
    put(c, 0x114, ri10(STQD, 1, 2, 3));
    put(c, 0x118, rrb(STQX, 0, 2, 3));
    put(c, 0x11C, ri16(STQA, 0x100, 3));
    put(c, 0x120, ri16(STQR, 0x40, 3));
    put(c, 0x124, stop(0));
}
static void expect_mem_pcs(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0 && r.steps == 10 && r.last_pc == 0x124 && R(2) == 0x400);
    CHECK(g_ls_n == 8);
    for (int i = 0; i < 8; i++) {
        CHECK(g_ls_hit[i].write == (i >= 4));
        CHECK(g_ls_hit[i].pc == 0x104u + 4u * (uint32_t)i);
    }
}

/* Conditional halts and the unimplemented-op trap stop the run at the pc of
 * the instruction itself, not the next one:
 *   100 il r2,5 / 104 heqi r2,5 / 108 il r3,1 / 10C stop 0 */
static void load_halt_hit_program(spu_context* c) {
    put(c, 0x100, il(2, 5));
    put(c, 0x104, ri10(HEQI, 5, 2, 0));
    put(c, 0x108, il(3, 1));
    put(c, 0x10C, stop(0));
}
static void expect_halt_hit(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x1000 && g_ctx.stop_code == 0x1000 && g_ctx.status == SPU_STATUS_STOPPED_BY_HALT);
    CHECK(g_ctx.pc == 0x104 && r.steps == 2 && r.last_pc == 0x104);
    CHECK(R(2) == 5 && R(3) == 0);
}
/* The same with an immediate that does not match: runs to the stop. */
static void load_halt_miss_program(spu_context* c) {
    load_halt_hit_program(c);
    put(c, 0x104, ri10(HEQI, 6, 2, 0));
}
static void expect_halt_miss(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0 && g_ctx.pc == 0x110 && g_ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    CHECK(r.steps == 4 && r.last_pc == 0x10C && R(3) == 1);
}
/* hgti is signed and hlgti unsigned: r2 = -1 is not above 0 signed, is
 * unsigned. The first falls through, the second halts.
 *   100 il r2,-1 / 104 hgti r2,0 / 108 hlgti r2,0 / 10C il r3,1 / 110 stop 0 */
static void load_halt_signed_program(spu_context* c) {
    put(c, 0x100, il(2, 0xFFFF));
    put(c, 0x104, ri10(HGTI, 0, 2, 0));
    put(c, 0x108, ri10(HLGTI, 0, 2, 0));
    put(c, 0x10C, il(3, 1));
    put(c, 0x110, stop(0));
}
static void expect_halt_signed(int mode, result r) {
    (void)mode;
    CHECK(r.ret == 0x1000 && g_ctx.pc == 0x108 && r.steps == 3 && r.last_pc == 0x108);
    CHECK(R(2) == 0xFFFFFFFFu && R(3) == 0);
}
/* An opcode with no instruction behind it (one line on stderr per run):
 *   100 il r2,5 / 104 .word 0x00800000 / 108 il r3,1 / 10C stop 0 */
static void load_trap_program(spu_context* c) {
    put(c, 0x100, il(2, 5));
    put(c, 0x104, 0x00800000u);
    put(c, 0x108, il(3, 1));
    put(c, 0x10C, stop(0));
}
static void expect_trap(int mode, result r) {
    (void)mode;
    CHECK(g_ctx.status == SPU_STATUS_STOPPED_BY_HALT && g_ctx.pc == 0x104);
    CHECK(r.steps == 2 && r.last_pc == 0x104 && R(2) == 5 && R(3) == 0);
}

/* The raw-SPU report (spu_raw.c) reads ctx->pc of an interpreting SPU from
 * another thread to say where a spinning SPU is. A fast step does not store it,
 * so a taken branch must: the spin below takes one every other step, and the
 * sampler has to see the loop's pc while it runs, not the entry pc it started
 * with. The sampler then releases the spin through local store; nothing here
 * depends on how long the run takes.
 *   100 il r2,0
 *   104 ai r2,r2,1            the branch target: no instruction there stores pc
 *   108 lqa r6,0x400          spin until the word at 0x400 is nonzero
 *   10C brz r6,0x104
 *   110 stop 0x2A */
static int g_spin_done;                       /* __atomic builtins: volatile does not order */
static uint32_t g_spin_ret;
static void* spin_main(void* arg) {
    (void)arg;
    g_spin_ret = spu_interp_run_until(&g_ctx, 0x100, 0);
    __atomic_store_n(&g_spin_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static void test_pc_sampled_from_another_thread(void) {
    spu_context* c = &g_ctx;
    g_case = "spin/sampled-pc"; g_mode = "fast";
    memset(c, 0, sizeof *c);
    spu_context_init(c, 0);
    c->image_id = -1;
    put(c, 0x100, il(2, 0));
    put(c, 0x104, ai(2, 2, 1));
    put(c, 0x108, ri16(LQA, 0x100, 6));        /* 0x100 << 2 = 0x400 */
    put(c, 0x10C, ri16(0x40, 0xFFFE, 6));      /* brz r6, back two words */
    put(c, 0x110, stop(0x2A));
    __atomic_store_n(&g_spin_done, 0, __ATOMIC_RELAXED);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, spin_main, NULL) == 0);
    int saw_loop_pc = 0;
    time_t t0 = time(NULL);
    while (!__atomic_load_n(&g_spin_done, __ATOMIC_ACQUIRE) && time(NULL) - t0 < 10) {
        if (__atomic_load_n(&c->pc, __ATOMIC_RELAXED) == 0x104) { saw_loop_pc = 1; break; }
    }
    __atomic_store_n(&c->ls[0x403], 1, __ATOMIC_RELAXED);   /* word 0 becomes 1: leave the spin */
    pthread_join(th, NULL);
    CHECK(saw_loop_pc);
    CHECK(g_spin_ret == 0x2A && g_ctx.pc == 0x114);
}

int main(void) {
    all_modes("call/hle", load_call_program, 0, 1, expect_call_hle);
    all_modes("call/native", load_call_program, 0, 0, expect_call_native);

    g_spu_pending_evt[0] = 0x11111111u; g_spu_pending_evt[1] = 0x22222222u;
    g_spu_pending_evt[2] = 0x33333333u;
    g_spu_pending_evt_valid = 1;
    {   /* each mode needs the event pending again */
        snap first; int have = 0;
        for (int m = 0; m < M_COUNT; ++m) {
            obs_count oc = { 0 };
            g_case = "event/serviced"; g_mode = mode_name[m];
            g_spu_pending_evt_valid = 1;
            result r = run_mode(m, load_event_program, 0, 0, &oc);
            expect_event_serviced(m, r);
            snap s; take(&s, r);
            if (have) { same(&first, &s); free(s.ls); } else { first = s; have = 1; }
        }
        free(first.ls);
    }
    g_spu_pending_evt_valid = 0;
    all_modes("event/unserviced", load_event_program, 0, 0, expect_event_unserviced);

    g_lifted_pc = 0x108;
    all_modes("switch/rejoin", load_switch_program, 0, 0, expect_switch_rejoin);
    g_lifted_pc = 0;
    all_modes("switch/stop_lsa", load_switch_program, 0x108, 0, expect_switch_stop_lsa);
    all_modes("switch/runs_on", load_switch_program, 0, 0, expect_switch_runs_on);

    all_modes("chan/pc", load_chan_program, 0, 0, expect_chan_pcs);
    g_lifted_pc = 0x108;
    all_modes("switch/rdch", load_switch_rdch_program, 0, 0, expect_switch_rejoin);
    all_modes("switch/rchcnt", load_switch_rchcnt_program, 0, 0, expect_switch_rejoin);
    g_lifted_pc = 0;
    all_modes("halt/longjmp", load_halt_program, 0, 0, expect_halt_exit);
    all_modes("nested/run", load_nest_program, 0, 0, expect_nest);
    g_arm_watch = 1;
    all_modes("mem/pc", load_mem_program, 0, 0, expect_mem_pcs);
    g_arm_watch = 0;
    all_modes("halt/hit", load_halt_hit_program, 0, 0, expect_halt_hit);
    all_modes("halt/miss", load_halt_miss_program, 0, 0, expect_halt_miss);
    all_modes("halt/signed", load_halt_signed_program, 0, 0, expect_halt_signed);
    all_modes("trap/word", load_trap_program, 0, 0, expect_trap);
    test_pc_sampled_from_another_thread();

    printf("test_spu_interp_loop: PASS\n");
    return 0;
}
