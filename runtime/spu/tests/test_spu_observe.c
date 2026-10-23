/* test_spu_observe.c - synthetic proof of the SPU observer API (spu_observe.h).
 *
 * Hand-encoded SPU instructions only; no assembler, no game or firmware bytes.
 * Links the real interpreter, channel handlers, DMA engine and lock-line
 * runtime, so the hook sites under test are the production ones:
 *
 *   (a) on_insn sees every interpreted pc, in order, across a loop and a
 *       `bisl` call/return;
 *   (b) yielding after k instructions and resuming at ctx->pc produces the
 *       same final registers/local store as an unobserved run, for k values
 *       inside the called function included;
 *   (c) two contexts round-robined on one host thread by yield both finish;
 *   (d) on_mfc reports a GET and a PUT (cmd/tag/lsa/ea/size) and on_channel
 *       reports the wrch/rdch values;
 *   (e) GETLLAR/PUTLLC report on_atomic with 4/0, and a PUTLLC whose line was
 *       committed by another context reports 1.
 */
#include "../spu_context.h"
#include "../spu_dma.h"      /* vm_base */
#include "../spu_interp.h"
#include "../spu_observe.h"
#include <stdio.h>
#include <string.h>

/* The DMA guard resolves EAs through vm_base and asks the host whether the
 * page is mapped; a static arena is, so no page-table shimming is needed. */
static uint8_t g_mem[0x40000];
uint8_t* vm_base = g_mem;
uint32_t ppu_vm_size = sizeof g_mem;

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

/* ---- instruction encoders (big-endian SPU words) ------------------------ */
static uint32_t ek_il(int rt, uint32_t imm16)
{
    return (0x81u << 23) | ((imm16 & 0xFFFFu) << 7) | (uint32_t)rt;
}
static uint32_t ek_ila(int rt, uint32_t imm18)
{
    return (0x21u << 25) | ((imm18 & 0x3FFFFu) << 7) | (uint32_t)rt;
}
static uint32_t ek_ai(int rt, int ra, int imm10)
{
    return (0x1Cu << 24) | (((uint32_t)imm10 & 0x3FFu) << 14) |
           ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_a(int rt, int ra, int rb)
{
    return (0xC0u << 21) | ((uint32_t)rb << 14) | ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_ceq(int rt, int ra, int rb)
{
    return (0x3C0u << 21) | ((uint32_t)rb << 14) | ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_brz(int rt, uint32_t pc, uint32_t target)
{
    int32_t off = ((int32_t)target - (int32_t)pc) / 4;
    return (0x40u << 23) | (((uint32_t)off & 0xFFFFu) << 7) | (uint32_t)rt;
}
static uint32_t ek_lqd(int rt, int ra, int off)
{
    return (0x34u << 24) | ((((uint32_t)(off / 16)) & 0x3FFu) << 14) |
           ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_stqd(int rt, int ra, int off)
{
    return (0x24u << 24) | ((((uint32_t)(off / 16)) & 0x3FFu) << 14) |
           ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_wrch(uint32_t ch, int rt)
{
    return (0x10Du << 21) | (ch << 7) | (uint32_t)rt;
}
static uint32_t ek_rdch(int rt, uint32_t ch)
{
    return (0x00Du << 21) | (ch << 7) | (uint32_t)rt;
}
static uint32_t ek_bisl(int rt, int ra)
{
    return (0x1A9u << 21) | ((uint32_t)ra << 7) | (uint32_t)rt;
}
static uint32_t ek_bi(int ra)
{
    return (0x1A8u << 21) | ((uint32_t)ra << 7);
}
static uint32_t ek_stop(uint32_t code) { return code & 0x3FFFu; }
static uint32_t ek_nop(void) { return 0x40200000u; }

static void emit(uint8_t* ls, uint32_t at, uint32_t w)
{
    ls[at]     = (uint8_t)(w >> 24);
    ls[at + 1] = (uint8_t)(w >> 16);
    ls[at + 2] = (uint8_t)(w >> 8);
    ls[at + 3] = (uint8_t)w;
}

/* ---- observer plumbing -------------------------------------------------- */
typedef struct {
    uint32_t* pcs;
    int n, cap;
    int yield_at;          /* one-shot: yield before hook call #yield_at */
    int every;             /* repeating: yield after this many executed insns */
    int executed;
    int yields;
} obs_log;

static int obs_insn(void* user, spu_context* ctx, uint32_t pc)
{
    obs_log* l = (obs_log*)user;
    (void)ctx;
    int yield = 0;
    if (l->yield_at && l->n + 1 == l->yield_at) {
        l->yield_at = 0;
        l->yields++;
        yield = 1;
    } else if (l->every && l->executed >= l->every) {
        l->executed = 0;
        l->yields++;
        yield = 1;
    }
    if (yield) return 1;   /* the instruction is NOT executed: do not log it */
    if (l->pcs && l->n < l->cap) l->pcs[l->n] = pc;
    l->n++;
    l->executed++;
    return 0;
}

/* ---- program: counter loop with a bisl call and a return ---------------- */
/*  0x00 il   $2,0           counter
 *  0x04 il   $3,bound
 *  0x08 il   $5,0x30        callee address
 *  0x0C ai   $2,$2,1        <- loop
 *  0x10 bisl $0,$5          call, link = 0x14
 *  0x14 ceq  $4,$2,$3
 *  0x18 brz  $4,0x0C
 *  0x1C stop 0
 *  0x30 ai   $6,$6,1        <- callee: call count
 *  0x34 a    $7,$7,$2       sum of counters
 *  0x38 bi   $0             return
 */
static void build_loop_program(uint8_t* ls, uint32_t bound)
{
    memset(ls, 0, SPU_LS_SIZE);
    emit(ls, 0x00, ek_il(2, 0));
    emit(ls, 0x04, ek_il(3, bound));
    emit(ls, 0x08, ek_il(5, 0x30));
    emit(ls, 0x0C, ek_ai(2, 2, 1));
    emit(ls, 0x10, ek_bisl(0, 5));
    emit(ls, 0x14, ek_ceq(4, 2, 3));
    emit(ls, 0x18, ek_brz(4, 0x18, 0x0C));
    emit(ls, 0x1C, ek_stop(0));
    for (uint32_t at = 0x20; at < 0x30; at += 4) emit(ls, at, ek_nop());
    emit(ls, 0x30, ek_ai(6, 6, 1));
    emit(ls, 0x34, ek_a(7, 7, 2));
    emit(ls, 0x38, ek_bi(0));
}

/* The exact instruction stream for build_loop_program(ls, 3). */
static const uint32_t kLoopTrace[] = {
    0x00, 0x04, 0x08,
    0x0C, 0x10, 0x30, 0x34, 0x38, 0x14, 0x18,
    0x0C, 0x10, 0x30, 0x34, 0x38, 0x14, 0x18,
    0x0C, 0x10, 0x30, 0x34, 0x38, 0x14, 0x18,
    0x1C,
};

static int trace_matches(const obs_log* l)
{
    int want = (int)(sizeof kLoopTrace / sizeof kLoopTrace[0]);
    if (l->n != want) return 0;
    for (int i = 0; i < want; i++)
        if (l->pcs[i] != kLoopTrace[i]) return 0;
    return 1;
}

/* ---- whole architectural state, for yield/resume equivalence ------------ */
typedef struct {
    u128 gpr[128];
    uint8_t ls[SPU_LS_SIZE];
    uint32_t pc, status, stop_code;
} spu_snapshot;

static void snap_take(spu_snapshot* s, const spu_context* ctx)
{
    memcpy(s->gpr, ctx->gpr, sizeof s->gpr);
    memcpy(s->ls, ctx->ls, sizeof s->ls);
    s->pc = ctx->pc;
    s->status = ctx->status;
    s->stop_code = ctx->stop_code;
}

static int snap_eq(const spu_snapshot* a, const spu_snapshot* b)
{
    return memcmp(a->gpr, b->gpr, sizeof a->gpr) == 0 &&
           memcmp(a->ls, b->ls, sizeof a->ls) == 0 &&
           a->pc == b->pc && a->status == b->status &&
           a->stop_code == b->stop_code;
}

static spu_snapshot g_base, g_trial;

static void test_insn_trace(void)
{
    static spu_context ctx;
    static uint32_t pcs[64];
    obs_log log;
    memset(&log, 0, sizeof log);
    log.pcs = pcs;
    log.cap = (int)(sizeof pcs / sizeof pcs[0]);
    spu_observer obs;
    memset(&obs, 0, sizeof obs);
    obs.user = &log;
    obs.on_insn = obs_insn;

    spu_context_init(&ctx, 0);
    ctx.image_id = -1;                    /* pure interpretation */
    spu_observer_set(&ctx, &obs);
    build_loop_program(ctx.ls, 3);
    uint32_t r = spu_interp_run(&ctx, 0);

    CHECK(r == 0, "loop program returns stop code 0 (got 0x%X)", r);
    CHECK(ctx.status == SPU_STATUS_STOPPED_BY_STOP, "loop program stopped by stop");
    CHECK(log.n == (int)(sizeof kLoopTrace / sizeof kLoopTrace[0]),
          "on_insn saw %d instructions, want %d", log.n,
          (int)(sizeof kLoopTrace / sizeof kLoopTrace[0]));
    if (!trace_matches(&log)) {
        printf("FAIL: executed pc trace differs:");
        for (int i = 0; i < log.n && i < 32; i++) printf(" %02X", pcs[i]);
        printf("\n");
        g_fail++;
    }
    CHECK(ctx.gpr[2]._u32[0] == 3 && ctx.gpr[6]._u32[0] == 3 &&
          ctx.gpr[7]._u32[0] == 6,
          "loop/callee counters: r2=%u r6=%u r7=%u (want 3/3/6)",
          ctx.gpr[2]._u32[0], ctx.gpr[6]._u32[0], ctx.gpr[7]._u32[0]);
    CHECK(ctx.gpr[0]._u32[0] == 0x14, "link register r0=0x%X (want 0x14)",
          ctx.gpr[0]._u32[0]);

    /* NULL clears the observer: the same program must now run silently. */
    spu_observer_set(&ctx, NULL);
    CHECK(ctx.observer == NULL, "spu_observer_set(NULL) clears the context");
    int before = log.n;
    build_loop_program(ctx.ls, 3);
    r = spu_interp_run(&ctx, 0);
    CHECK(r == 0 && log.n == before, "cleared observer sees no callbacks");
}

static void test_yield_resume(void)
{
    static const int ks[] = { 1, 2, 3, 4, 5, 6, 7, 10, 12, 13, 14, 20, 24 };
    /* Baseline: unobserved run of build_loop_program(ls, 3). */
    static spu_context base;
    spu_context_init(&base, 0);
    base.image_id = -1;
    build_loop_program(base.ls, 3);
    CHECK(spu_interp_run(&base, 0) == 0, "baseline run stops");
    snap_take(&g_base, &base);

    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) {
        int k = ks[i];
        static spu_context ctx;
        static uint32_t pcs[64];
        obs_log log;
        memset(&log, 0, sizeof log);
        log.pcs = pcs;
        log.cap = (int)(sizeof pcs / sizeof pcs[0]);
        log.yield_at = k + 1;             /* k executed, yield before k+1 */
        spu_observer obs;
        memset(&obs, 0, sizeof obs);
        obs.user = &log;
        obs.on_insn = obs_insn;

        spu_context_init(&ctx, 0);
        ctx.image_id = -1;
        spu_observer_set(&ctx, &obs);
        build_loop_program(ctx.ls, 3);

        uint32_t r = spu_interp_run(&ctx, 0);
        CHECK(r == SPU_INTERP_YIELDED, "k=%d: first run returned 0x%X", k, r);
        CHECK(ctx.pc == kLoopTrace[k],
              "k=%d: yielded at pc 0x%X, want 0x%X", k, ctx.pc, kLoopTrace[k]);
        CHECK(ctx.status == SPU_STATUS_RUNNING, "k=%d: still running", k);
        CHECK(log.yields == 1, "k=%d: exactly one yield (got %d)", k, log.yields);

        r = spu_interp_run(&ctx, ctx.pc);  /* resume exactly where it stopped */
        CHECK(r == 0 && ctx.status == SPU_STATUS_STOPPED_BY_STOP,
              "k=%d: resumed run finished (r=0x%X status=%u)", k, r, ctx.status);
        CHECK(trace_matches(&log),
              "k=%d: resume continued the same instruction stream (%d pcs)", k, log.n);

        snap_take(&g_trial, &ctx);
        CHECK(snap_eq(&g_base, &g_trial),
              "k=%d: final registers/LS/pc match the unobserved run", k);
    }
}

static void test_round_robin(void)
{
    static spu_context a, b;
    obs_log la, lb;
    spu_observer oa, ob;
    memset(&la, 0, sizeof la);
    memset(&lb, 0, sizeof lb);
    memset(&oa, 0, sizeof oa);
    memset(&ob, 0, sizeof ob);
    la.every = 4;
    lb.every = 4;
    oa.user = &la;
    oa.on_insn = obs_insn;
    ob.user = &lb;
    ob.on_insn = obs_insn;

    spu_context_init(&a, 0);
    spu_context_init(&b, 1);
    a.image_id = -1;
    b.image_id = -1;
    build_loop_program(a.ls, 3);
    build_loop_program(b.ls, 7);
    spu_observer_set(&a, &oa);
    spu_observer_set(&b, &ob);

    int done_a = 0, done_b = 0;
    for (int guard = 0; guard < 4096 && !(done_a && done_b); guard++) {
        if (!done_a) {
            uint32_t r = spu_interp_run(&a, a.pc);
            if (r != SPU_INTERP_YIELDED) {
                CHECK(r == 0 && a.status == SPU_STATUS_STOPPED_BY_STOP,
                      "context A finished normally (r=0x%X status=%u)", r, a.status);
                done_a = 1;
            }
        }
        if (!done_b) {
            uint32_t r = spu_interp_run(&b, b.pc);
            if (r != SPU_INTERP_YIELDED) {
                CHECK(r == 0 && b.status == SPU_STATUS_STOPPED_BY_STOP,
                      "context B finished normally (r=0x%X status=%u)", r, b.status);
                done_b = 1;
            }
        }
    }
    CHECK(done_a && done_b, "both contexts completed");
    CHECK(la.yields >= 2 && lb.yields >= 2,
          "both contexts were preempted (A=%d B=%d yields)", la.yields, lb.yields);
    CHECK(a.gpr[2]._u32[0] == 3 && a.gpr[6]._u32[0] == 3 && a.gpr[7]._u32[0] == 6,
          "A result: r2=%u r6=%u r7=%u (want 3/3/6)",
          a.gpr[2]._u32[0], a.gpr[6]._u32[0], a.gpr[7]._u32[0]);
    CHECK(b.gpr[2]._u32[0] == 7 && b.gpr[6]._u32[0] == 7 && b.gpr[7]._u32[0] == 28,
          "B result: r2=%u r6=%u r7=%u (want 7/7/28)",
          b.gpr[2]._u32[0], b.gpr[6]._u32[0], b.gpr[7]._u32[0]);
}

/* ---- (d) MFC GET/PUT and channel values --------------------------------- */
typedef struct {
    spu_obs_mfc mfc[8];
    int n_mfc;
    struct { int is_write; uint32_t ch, value; } ch[32];
    int n_ch;
} mfc_log;

static void mfc_on_mfc(void* user, spu_context* ctx, const spu_obs_mfc* m)
{
    mfc_log* l = (mfc_log*)user;
    (void)ctx;
    if (l->n_mfc < (int)(sizeof l->mfc / sizeof l->mfc[0])) l->mfc[l->n_mfc++] = *m;
}
static void mfc_on_channel(void* user, spu_context* ctx, int is_write,
                           uint32_t ch, uint32_t value)
{
    mfc_log* l = (mfc_log*)user;
    (void)ctx;
    if (l->n_ch < (int)(sizeof l->ch / sizeof l->ch[0])) {
        l->ch[l->n_ch].is_write = is_write;
        l->ch[l->n_ch].ch = ch;
        l->ch[l->n_ch].value = value;
        l->n_ch++;
    }
}

/*  GET 16 bytes from EA 0x20000 to LS 0x100 (tag 3), read the first word,
 *  increment it, store to LS 0x200 and PUT 16 bytes back to the same EA.
 *  Then read MFC_RdTagStat for tag mask 8 (tag 3 set) with update-all. */
static void build_mfc_program(uint8_t* ls)
{
    memset(ls, 0, SPU_LS_SIZE);
    emit(ls, 0x00, ek_il(2, 0x100));            /* GET destination */
    emit(ls, 0x04, ek_il(3, 0x200));            /* PUT source */
    emit(ls, 0x08, ek_wrch(MFC_LSA, 2));
    emit(ls, 0x0C, ek_wrch(MFC_EAH, 0));        /* r0 is zero here */
    emit(ls, 0x10, ek_ila(4, 0x20000));
    emit(ls, 0x14, ek_wrch(MFC_EAL, 4));
    emit(ls, 0x18, ek_il(5, 16));
    emit(ls, 0x1C, ek_wrch(MFC_Size, 5));
    emit(ls, 0x20, ek_il(6, 3));
    emit(ls, 0x24, ek_wrch(MFC_TagID, 6));
    emit(ls, 0x28, ek_il(7, MFC_GET_CMD));
    emit(ls, 0x2C, ek_wrch(MFC_Cmd, 7));
    emit(ls, 0x30, ek_lqd(8, 2, 0));
    emit(ls, 0x34, ek_ai(8, 8, 1));
    emit(ls, 0x38, ek_stqd(8, 3, 0));
    emit(ls, 0x3C, ek_wrch(MFC_LSA, 3));
    emit(ls, 0x40, ek_il(9, MFC_PUT_CMD));
    emit(ls, 0x44, ek_wrch(MFC_Cmd, 9));
    emit(ls, 0x48, ek_il(6, 8));                /* tag mask = 1 << 3 */
    emit(ls, 0x4C, ek_wrch(MFC_WrTagMask, 6));
    emit(ls, 0x50, ek_il(7, 2));                /* update-all */
    emit(ls, 0x54, ek_wrch(MFC_WrTagUpdate, 7));
    emit(ls, 0x58, ek_rdch(10, MFC_RdTagStat));
    emit(ls, 0x5C, ek_stop(0));
}

/* wrch/rdch sequence the program above must report, in order. */
static const struct { int is_write; uint32_t ch, value; } kMfcChannels[] = {
    { 1, MFC_LSA,          0x100 },
    { 1, MFC_EAH,          0 },
    { 1, MFC_EAL,          0x20000 },
    { 1, MFC_Size,         16 },
    { 1, MFC_TagID,        3 },
    { 1, MFC_Cmd,          MFC_GET_CMD },
    { 1, MFC_LSA,          0x200 },
    { 1, MFC_Cmd,          MFC_PUT_CMD },
    { 1, MFC_WrTagMask,    8 },
    { 1, MFC_WrTagUpdate,  2 },
    { 0, MFC_RdTagStat,    8 },
};

static void test_mfc_and_channels(void)
{
    static spu_context ctx;
    static mfc_log log;
    spu_observer obs;
    memset(&log, 0, sizeof log);
    memset(&obs, 0, sizeof obs);
    obs.user = &log;
    obs.on_mfc = mfc_on_mfc;
    obs.on_channel = mfc_on_channel;

    memset(g_mem, 0, sizeof g_mem);
    g_mem[0x20000] = 0x11; g_mem[0x20001] = 0x22;
    g_mem[0x20002] = 0x33; g_mem[0x20003] = 0x44;
    g_mem[0x20004] = 0x55; g_mem[0x20005] = 0x66;
    g_mem[0x20006] = 0x77; g_mem[0x20007] = 0x88;

    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
    spu_observer_set(&ctx, &obs);
    build_mfc_program(ctx.ls);
    uint32_t r = spu_interp_run(&ctx, 0);
    CHECK(r == 0 && ctx.status == SPU_STATUS_STOPPED_BY_STOP,
          "MFC program finished (r=0x%X status=%u)", r, ctx.status);

    /* GET moved the source bytes into LS unchanged (the PUT only rewrote
     * memory from LS 0x200, so the GET copy at 0x100 is still intact). */
    static const uint8_t kFetched[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    CHECK(memcmp(ctx.ls + 0x100, kFetched, sizeof kFetched) == 0,
          "GET landed at LS 0x100 verbatim");
    /* The program read the block, added one to every word (SPU `ai` is a
     * four-lane operation) and PUT it back. */
    static const uint8_t kPutBack[16] = {
        0x11, 0x22, 0x33, 0x45, 0x55, 0x66, 0x77, 0x89,
        0, 0, 0, 1, 0, 0, 0, 1,
    };
    CHECK(memcmp(g_mem + 0x20000, kPutBack, sizeof kPutBack) == 0,
          "PUT wrote the incremented block: "
          "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
          g_mem[0x20000], g_mem[0x20001], g_mem[0x20002], g_mem[0x20003],
          g_mem[0x20004], g_mem[0x20005], g_mem[0x20006], g_mem[0x20007],
          g_mem[0x20008], g_mem[0x20009], g_mem[0x2000A], g_mem[0x2000B],
          g_mem[0x2000C], g_mem[0x2000D], g_mem[0x2000E], g_mem[0x2000F]);
    CHECK(ctx.gpr[10]._u32[0] == 8, "rdch MFC_RdTagStat gave %u (want 8)",
          ctx.gpr[10]._u32[0]);

    CHECK(log.n_mfc == 2, "on_mfc reported %d commands (want 2)", log.n_mfc);
    if (log.n_mfc == 2) {
        const spu_obs_mfc* g = &log.mfc[0];
        const spu_obs_mfc* p = &log.mfc[1];
        CHECK(g->cmd == MFC_GET_CMD && g->tag == 3 && g->lsa == 0x100 &&
              g->ea == 0x20000 && g->size == 16,
              "GET report cmd=0x%X tag=%u lsa=0x%X ea=0x%llX size=%u",
              g->cmd, g->tag, g->lsa, (unsigned long long)g->ea, g->size);
        CHECK(p->cmd == MFC_PUT_CMD && p->tag == 3 && p->lsa == 0x200 &&
              p->ea == 0x20000 && p->size == 16,
              "PUT report cmd=0x%X tag=%u lsa=0x%X ea=0x%llX size=%u",
              p->cmd, p->tag, p->lsa, (unsigned long long)p->ea, p->size);
    }

    int want = (int)(sizeof kMfcChannels / sizeof kMfcChannels[0]);
    CHECK(log.n_ch == want, "on_channel reported %d accesses (want %d)",
          log.n_ch, want);
    for (int i = 0; i < want && i < log.n_ch; i++) {
        CHECK(log.ch[i].is_write == kMfcChannels[i].is_write &&
              log.ch[i].ch == kMfcChannels[i].ch &&
              log.ch[i].value == kMfcChannels[i].value,
              "channel[%d] got %s ch%u = 0x%X, want %s ch%u = 0x%X", i,
              log.ch[i].is_write ? "wrch" : "rdch", log.ch[i].ch,
              log.ch[i].value, kMfcChannels[i].is_write ? "wrch" : "rdch",
              kMfcChannels[i].ch, kMfcChannels[i].value);
    }

    spu_observer_set(&ctx, NULL);
    spu_mfc_release(&ctx);
}

/* ---- (e) atomic status -------------------------------------------------- */
typedef struct {
    struct { uint32_t cmd; uint64_t ea; uint32_t status; } a[8];
    int n;
} atom_log;

static void atom_on_atomic(void* user, spu_context* ctx, uint32_t cmd,
                           uint64_t ea, uint32_t status)
{
    atom_log* l = (atom_log*)user;
    (void)ctx;
    if (l->n < (int)(sizeof l->a / sizeof l->a[0])) {
        l->a[l->n].cmd = cmd;
        l->a[l->n].ea = ea;
        l->a[l->n].status = status;
        l->n++;
    }
}

/* Stage the lock-line operands (LSA 0x300, EA 0x30000, 128 bytes, tag 5). */
static void stage_atomic(uint8_t* ls, uint32_t at)
{
    emit(ls, at + 0x00, ek_il(2, 0x300));
    emit(ls, at + 0x04, ek_ila(3, 0x30000));
    emit(ls, at + 0x08, ek_wrch(MFC_LSA, 2));
    emit(ls, at + 0x0C, ek_wrch(MFC_EAH, 0));
    emit(ls, at + 0x10, ek_wrch(MFC_EAL, 3));
    emit(ls, at + 0x14, ek_il(4, 128));
    emit(ls, at + 0x18, ek_wrch(MFC_Size, 4));
    emit(ls, at + 0x1C, ek_il(5, 5));
    emit(ls, at + 0x20, ek_wrch(MFC_TagID, 5));
}

/* GETLLAR, then read MFC_RdAtomicStat into r9 and stop. */
static void build_getllar_program(uint8_t* ls, uint32_t at)
{
    memset(ls, 0, SPU_LS_SIZE);
    stage_atomic(ls, at);
    emit(ls, at + 0x24, ek_il(6, MFC_GETLLAR_CMD));
    emit(ls, at + 0x28, ek_wrch(MFC_Cmd, 6));
    emit(ls, at + 0x2C, ek_rdch(9, MFC_RdAtomicStat));
    emit(ls, at + 0x30, ek_stop(0));
}

/* GETLLAR, bump the line's first word, PUTLLC it back, report status. */
static void build_getllar_putllc_program(uint8_t* ls, uint32_t at)
{
    memset(ls, 0, SPU_LS_SIZE);
    stage_atomic(ls, at);
    emit(ls, at + 0x24, ek_il(6, MFC_GETLLAR_CMD));
    emit(ls, at + 0x28, ek_wrch(MFC_Cmd, 6));
    emit(ls, at + 0x2C, ek_lqd(7, 2, 0));
    emit(ls, at + 0x30, ek_ai(7, 7, 1));
    emit(ls, at + 0x34, ek_stqd(7, 2, 0));
    emit(ls, at + 0x38, ek_il(8, MFC_PUTLLC_CMD));
    emit(ls, at + 0x3C, ek_wrch(MFC_Cmd, 8));
    emit(ls, at + 0x40, ek_rdch(9, MFC_RdAtomicStat));
    emit(ls, at + 0x44, ek_stop(0));
}

/* PUTLLC only: no fresh GETLLAR, so an invalidated reservation must fail. */
static void build_putllc_program(uint8_t* ls, uint32_t at)
{
    memset(ls, 0, SPU_LS_SIZE);
    stage_atomic(ls, at);
    emit(ls, at + 0x24, ek_il(8, MFC_PUTLLC_CMD));
    emit(ls, at + 0x28, ek_wrch(MFC_Cmd, 8));
    emit(ls, at + 0x2C, ek_rdch(9, MFC_RdAtomicStat));
    emit(ls, at + 0x30, ek_stop(0));
}

static void test_atomics(void)
{
    static spu_context a, b;
    static atom_log la, lb, la2;
    spu_observer oa, ob, oa2;
    memset(&la, 0, sizeof la);
    memset(&lb, 0, sizeof lb);
    memset(&la2, 0, sizeof la2);
    memset(&oa, 0, sizeof oa);
    memset(&ob, 0, sizeof ob);
    memset(&oa2, 0, sizeof oa2);
    oa.user = &la;
    oa.on_atomic = atom_on_atomic;
    ob.user = &lb;
    ob.on_atomic = atom_on_atomic;
    oa2.user = &la2;
    oa2.on_atomic = atom_on_atomic;

    memset(g_mem, 0, sizeof g_mem);
    spu_context_init(&a, 0);
    spu_context_init(&b, 1);
    a.image_id = -1;
    b.image_id = -1;

    /* A reserves the line (GETLLAR -> status 4). */
    spu_observer_set(&a, &oa);
    build_getllar_program(a.ls, 0);
    uint32_t r = spu_interp_run(&a, 0);
    CHECK(r == 0 && a.gpr[9]._u32[0] == 4,
          "A GETLLAR status %u (want 4)", a.gpr[9]._u32[0]);
    CHECK(a.resv_valid && (a.resv_ea & ~127u) == 0x30000u,
          "A holds the line reservation");

    /* B takes it, changes it and commits: GETLLAR 4, PUTLLC 0. */
    spu_observer_set(&b, &ob);
    build_getllar_putllc_program(b.ls, 0);
    r = spu_interp_run(&b, 0);
    CHECK(r == 0 && b.gpr[9]._u32[0] == 0,
          "B PUTLLC status %u (want 0)", b.gpr[9]._u32[0]);
    CHECK(g_mem[0x30003] == 1,
          "B committed the line (word0 = %02X%02X%02X%02X)",
          g_mem[0x30000], g_mem[0x30001], g_mem[0x30002], g_mem[0x30003]);
    CHECK(!a.resv_valid, "B's commit invalidated A's reservation");

    /* A's PUTLLC against the moved line must fail -> status 1. */
    spu_observer_set(&a, &oa2);
    build_putllc_program(a.ls, 0x800);
    r = spu_interp_run(&a, 0x800);
    CHECK(r == 0 && a.gpr[9]._u32[0] == 1,
          "A PUTLLC status %u (want 1)", a.gpr[9]._u32[0]);

    CHECK(la.n == 1 && la.a[0].cmd == MFC_GETLLAR_CMD &&
          la.a[0].ea == 0x30000 && la.a[0].status == 4,
          "A on_atomic: n=%d cmd=0x%X ea=0x%llX status=%u", la.n,
          la.n ? la.a[0].cmd : 0, (unsigned long long)(la.n ? la.a[0].ea : 0),
          la.n ? la.a[0].status : 0);
    CHECK(lb.n == 2 && lb.a[0].cmd == MFC_GETLLAR_CMD && lb.a[0].status == 4 &&
          lb.a[1].cmd == MFC_PUTLLC_CMD && lb.a[1].status == 0,
          "B on_atomic: n=%d [0]=0x%X/%u [1]=0x%X/%u", lb.n,
          lb.n ? lb.a[0].cmd : 0, lb.n ? lb.a[0].status : 0,
          lb.n > 1 ? lb.a[1].cmd : 0, lb.n > 1 ? lb.a[1].status : 0);
    CHECK(la2.n == 1 && la2.a[0].cmd == MFC_PUTLLC_CMD &&
          la2.a[0].ea == 0x30000 && la2.a[0].status == 1,
          "A failed PUTLLC on_atomic: n=%d status=%u", la2.n,
          la2.n ? la2.a[0].status : 0);

    spu_observer_set(&a, NULL);
    spu_observer_set(&b, NULL);
    spu_coh_unregister(&a);
    spu_coh_unregister(&b);
    spu_mfc_release(&a);
    spu_mfc_release(&b);
}

int main(void)
{
    test_insn_trace();
    test_yield_resume();
    test_round_robin();
    test_mfc_and_channels();
    test_atomics();

    if (g_fail) {
        printf("test_spu_observe: FAIL (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_spu_observe: PASS\n");
    return 0;
}

/* ---- link-only boundaries ----------------------------------------------
 * The channel TM references the SPURS workload/taskset layer, which this
 * CPU-only fixture does not run. Every one of these is reached only by a
 * SPURS syscall or event-flag path, never by the synthetic programs above, so
 * they abort if that assumption ever stops holding. */
static void unused(const char* what) __attribute__((noreturn, cold));
static void unused(const char* what)
{
    fprintf(stderr, "test_spu_observe: unexpected call into %s\n", what);
    fflush(stderr);
    abort();
}

unsigned long long ps3_ms_now(void) { unused("ps3_ms_now"); }
uint32_t g_barrier_sync_watch;
int g_cri_video_dma;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_ydkj_real_taskset_ea;
SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_exited;
SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_polls;
SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
uint16_t spu_ef_bits_take(uint32_t obj) { (void)obj; unused("spu_ef_bits_take"); }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits)
{
    (void)ea; (void)bits; unused("spurs_ef_set_from_spu");
}
int spu_taskset_wait_signal(uint32_t ts, uint32_t tid)
{
    (void)ts; (void)tid; unused("spu_taskset_wait_signal");
}
void spu_taskset_parked_add(uint32_t ts, uint32_t tid, uint32_t obj)
{
    (void)ts; (void)tid; (void)obj; unused("spu_taskset_parked_add");
}
void spu_taskset_parked_del(uint32_t ts, uint32_t tid)
{
    (void)ts; (void)tid; unused("spu_taskset_parked_del");
}
int spu_taskset_consume_wake(uint32_t ts)
{
    (void)ts; unused("spu_taskset_consume_wake");
}
void spu_taskset_line_committed(uint32_t line)
{
    /* Wakes tasks parked on this lock line; this fixture has no SPURS tasks,
     * so there is nothing to wake and the true behaviour is a no-op. */
    (void)line;
}
