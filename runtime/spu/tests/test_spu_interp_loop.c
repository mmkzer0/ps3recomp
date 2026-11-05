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
 * Hand-encoded instructions only; no title or firmware bytes. */
#include "../spu_interp.c"
#include <string.h>

/* No program here reads a channel. wrch is the one channel op used: it plays
 * the DMA command that changes the context's image mid-run (spu_dma.h). */
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

enum { CH_SWITCH_IMAGE = 70 };
static int g_switch_calls;
void spu_wrch(spu_context* c, uint32_t ch, u128 v) {
    (void)v;
    if (ch != CH_SWITCH_IMAGE) abort();
    g_switch_calls++;
    c->image_id = 5;
}

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
static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put(spu_context* c, uint32_t at, uint32_t insn) { be32(c->ls + at, insn); }

enum { IL = 0x081, BRNZ = 0x042, BRSL = 0x066, AI = 0x1C, BI = 0x1A8, WRCH = 0x10D };
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

typedef struct { int calls; } obs_count;
static int obs_on_insn(void* user, spu_context* c, uint32_t pc) {
    (void)c; (void)pc; ((obs_count*)user)->calls++; return 0;
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
    g_spu_interp_steps = 0xDEADBEEFDEADBEEFull;   /* must be rewritten by the run */
    g_spu_interp_last_pc = 0xFEEDFACEu;
    result r;
    r.ret = spu_interp_run_until(c, 0x100, stop_lsa);
    r.steps = (uint32_t)g_spu_interp_steps;
    r.last_pc = g_spu_interp_last_pc;
    CHECK(g_spu_interp_steps >> 32 == 0);
    CHECK(mode != M_OBS || oc->calls == (int)r.steps);   /* the observer saw every step */
    CHECK(g_lookup_with_pure_image == 0);   /* pure interpretation never asks the registry */
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
    CHECK(a->r.ret == b->r.ret && a->r.steps == b->r.steps && a->r.last_pc == b->r.last_pc);
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

    printf("test_spu_interp_loop: PASS\n");
    return 0;
}
