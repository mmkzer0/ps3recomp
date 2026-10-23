/* test_spurs_job_crt_main.c - the SPURS job loader's JOBCRT gate, Ver14 jm
 * interface, kernel view and bss (runtime/spu/spurs_job.c; docs/RUNTIME.md,
 * "SPURS job start"). Driven by tools/test_spurs_job_crt.py, one mode per run:
 * the stops end the process.
 *
 * Everything here is synthetic: job images are built from hand-encoded SPU
 * words and a deterministic junk pattern, no game or firmware bytes. The real
 * loader, channel layer, interpreter and lifted-call drain are linked; the
 * two "entry" shims below stand for the two ways a job runs:
 *
 *   interp   spu_interp_run() over the live local store, from LS 0
 *   lifted   a C function doing what the lifter emits: reads through the
 *            context, writes channels, calls through a register with the
 *            bisl bracket (spu_indirect_branch + spu_drain_call)
 *
 * Modes (argv[1]). A run prints PASS or FAIL lines and exits 0 / 1; a named
 * stop exits 70:
 *
 *   ver13 [dump]     Ver13 start state; dump = file to write the entry LS to
 *   ver14            Ver14 start state with bss; the CRT-like program reaches main
 *   fit              the cache buffer ends exactly at the job runtime area's base: no stop
 *   dma-fit          the input DMA list exactly fills ioBuffer: both elements land
 *   dma-over         the second element overruns ioBuffer by one byte: spu-job-dma-list
 *   dma-wrap         ea + size wraps 32-bit: spu-job-dma-list (64-bit range)
 *   dma-beyond       an element EA past guest memory: spu-job-dma-list
 *   dma-zero-ea      a non-zero size from the reserved null page: spu-job-dma-list
 *   dma-size-zero    a zero-size element (never skipped): spu-job-dma-list
 *   dma-size-limit   an element over 16 KB: spu-job-dma-list
 *   dma-size-align   an element size that is not 1/2/4/8 or a 16-byte multiple
 *   dma-misaligned   8-byte transfer from a 4-byte EA: spu-job-dma-list
 *   dma-desc-range   the descriptor's list leaves guest memory: spu-job-dma-list
 *   bin-ea-zero      eaBinary = 0: spu-job-dma-list
 *   bin-size-zero    sizeBinary = 0: spu-job-dma-list
 *   swap-interp      jm word 1 called with an incomplete tag, interpreted job
 *   swap-lifted      the same, lifted-style job
 *   swap-idle-interp / swap-idle-lifted   word 1 called with every tag complete
 *   word0-interp / word0-lifted           jm word 0 called: spu-job-jm-entry
 *   ver12 | ver15 | garbage | empty       spu-job-crt-version
 *   overlap          one quadword too much memory: spu-job-memory (region image)
 *   image-edge       image + bss end exactly at the runtime area: the io buffer above stops
 *   stack-max        sizeStack = 0xFFFF: spu-job-memory (region stack)
 *   cache-over       the cache buffer ends one byte past the runtime area: stop
 *   huge-io          sizeIn/Out = 0xFFFFFFFF: spu-job-memory (region io), no wrap
 */
#include "../spu_context.h"
#include "../spu_dma.h"      /* vm_base */
#include "../spu_helpers.h"
#include "../spu_interp.h"
#include "../spu_observe.h"
#include "../spu_workload.h"  /* spu_run_spurs_job */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spurs_job_set_spu_num(uint32_t n);

static uint8_t g_mem[1u << 20];

/* ---- layout the loader promises, pinned independently of its macros ------ */
enum {
    DESC_LS  = 0x3FC00,          /* SPU_LS_SIZE - 0x400 */
    CTX_LS   = 0x3FBC0,          /* DESC_LS - 0x40      */
    JM_LS    = 0x3FBF0,          /* CTX_LS + 0x30       */
    RT_LS    = 0x3FAC0,          /* CTX_LS - 0x100      */
    RT_SWAP  = RT_LS + 0x10,
    RT_TRAP_STOP = 0x3B0E,
    RET_LS   = 0x3FF00,          /* the link the loader plants */
};

/* guest memory */
enum {
    JOB_EA   = 0x10000,          /* CellSpursJob256 */
    BIN_EA   = 0x20000,
    IO_EA    = 0x30000,
    CACHE_EA = 0x31000,
    IMAGE_SIZE = 0x1000,
    NUM_SPU  = 3,
    IMAGE_ID = 7,
};

/* image layout */
enum {
    CODE_LS   = 0x300,           /* clear of the header and the kernel-view zones */
    LIST_LS   = 0x700,           /* DMA list: one stall-and-notify element */
    MARK_LS   = 0x610,           /* main stores its marker here */
    STALL_TAG = 5,
    MARKER    = 0x5A5A,
    LIFTED_RET = 0x2F0,          /* the address after the lifted bisl */
};

/* "fit": io starts at 0x3E800 (image + bss end exactly there, 1 KB aligned),
 * io/out/scratch+stack take 0x1000, and the cache buffer's 0x2C0 bytes end at
 * RT_LS. */
enum {
    FIT_BSS_QW = (0x3E800 - IMAGE_SIZE) / 16,
    FIT_CACHE  = 0x2C0,
};

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

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

/* A byte that is never zero and varies with position, so "zeroed" and "left
 * alone" are both observable on it. */
static uint8_t junk(uint32_t i) { return (uint8_t)(((i * 167u) ^ (i >> 5)) | 1u); }

/* ---- instruction encoders (big-endian SPU words) ------------------------- */
static uint32_t ek_il(int rt, int imm)  { return (0x81u << 23) | (((uint32_t)imm & 0xFFFFu) << 7) | (uint32_t)rt; }
static uint32_t ek_ilhu(int rt, uint32_t imm) { return (0x82u << 23) | ((imm & 0xFFFFu) << 7) | (uint32_t)rt; }
static uint32_t ek_iohl(int rt, uint32_t imm) { return (0xC1u << 23) | ((imm & 0xFFFFu) << 7) | (uint32_t)rt; }
static uint32_t ek_and(int rt, int ra, int rb) { return (0xC1u << 21) | ((uint32_t)rb << 14) | ((uint32_t)ra << 7) | (uint32_t)rt; }
static uint32_t ek_ori0(int rt, int ra) { return (0x04u << 24) | ((uint32_t)ra << 7) | (uint32_t)rt; }
static uint32_t ek_lqd(int rt, int ra, int off) { return (0x34u << 24) | ((((uint32_t)(off / 16)) & 0x3FFu) << 14) | ((uint32_t)ra << 7) | (uint32_t)rt; }
static uint32_t ek_rotqbyi(int rt, int ra, int n) { return (0x1FCu << 21) | (((uint32_t)n & 0x7Fu) << 14) | ((uint32_t)ra << 7) | (uint32_t)rt; }
static uint32_t ek_stqa(int rt, uint32_t lsa) { return (0x41u << 23) | (((lsa >> 2) & 0xFFFFu) << 7) | (uint32_t)rt; }
static uint32_t ek_wrch(uint32_t ch, int rt) { return (0x10Du << 21) | (ch << 7) | (uint32_t)rt; }
static uint32_t ek_rdch(int rt, uint32_t ch) { return (0x00Du << 21) | (ch << 7) | (uint32_t)rt; }
static uint32_t ek_bisl(int rt, int ra) { return (0x1A9u << 21) | ((uint32_t)ra << 7) | (uint32_t)rt; }
static uint32_t ek_stop(uint32_t code) { return code & 0x3FFFu; }
static uint32_t ek_rel16(uint32_t pc, uint32_t target) { return (uint32_t)(((int32_t)target - (int32_t)pc) / 4) & 0xFFFFu; }
static uint32_t ek_br(uint32_t pc, uint32_t target) { return (0x64u << 23) | (ek_rel16(pc, target) << 7); }
static uint32_t ek_brz(int rt, uint32_t pc, uint32_t target) { return (0x40u << 23) | (ek_rel16(pc, target) << 7) | (uint32_t)rt; }

/* Register r's value after the swap programs set the pattern. */
static uint32_t pat(int r) { return 0x1000u + (uint32_t)r; }

/* ---- the job image ------------------------------------------------------- */
typedef enum { PROG_MAIN, PROG_SWAP, PROG_WORD0 } prog_kind;

typedef struct job_cfg {
    const char* crt;           /* 12 bytes at LS 0x30 */
    uint32_t    bss_qw;        /* eaBinary high word */
    prog_kind   prog;
    int         stall;         /* leave tag STALL_TAG incomplete before the call */
    uint32_t    stack_qw;      /* sizeStack in quadwords; 0 = 0x40 */
    uint32_t    cache_bytes;   /* the one cache entry's size; 0 = 0x80 */
    uint32_t    io_bytes;      /* sizeIn/Out; 0 = 0x80 */
    uint32_t    job_ea;        /* descriptor EA; 0 = JOB_EA */
} job_cfg;

typedef struct emitter { uint8_t* base; uint32_t pc; } emitter;
static void emit(emitter* e, uint32_t w) { put32(e->base + e->pc, w); e->pc += 4; }

static void build_job(const job_cfg* c)
{
    memset(g_mem, 0xC3, sizeof g_mem);             /* junk past the binary too */
    uint8_t* d = g_mem + (c->job_ea ? c->job_ea : JOB_EA);
    memset(d, 0, 0x100);
    uint8_t* img = g_mem + BIN_EA;
    for (uint32_t i = 0; i < IMAGE_SIZE; i++) img[i] = junk(i);

    memcpy(img + 0x30, c->crt, 12);
    put32(img + 0x3C, IMAGE_SIZE + c->bss_qw * 16u);   /* memory size  */
    put32(img + 0x40, IMAGE_SIZE);                     /* binary size  */
    memset(img + LIST_LS, 0, 8);

    /* LS 0: jump over the header to the code */
    put32(img, ek_br(0, CODE_LS));

    emitter e = { img, CODE_LS };
    /* The CRT preflight (jq2e2-022): ctx+0x14 & 0x0FFFFFFF, zero -> stop 0x0A11
     * before main; otherwise the jm object it names gives r84 = word 1 (or
     * word 0 for the word-0 program). */
    emit(&e, ek_lqd(5, 3, 0x10));
    emit(&e, ek_rotqbyi(5, 5, 4));
    emit(&e, ek_ilhu(6, 0x0FFF));
    emit(&e, ek_iohl(6, 0xFFFF));
    emit(&e, ek_and(7, 5, 6));
    const uint32_t brz_pc = e.pc;
    emit(&e, 0);                                       /* brz r7, fail: patched below */
    emit(&e, ek_lqd(8, 7, 0));
    emit(&e, c->prog == PROG_WORD0 ? ek_ori0(84, 8) : ek_rotqbyi(84, 8, 4));
    if (c->prog == PROG_MAIN) {
        emit(&e, ek_il(10, MARKER));
        emit(&e, ek_stqa(10, MARK_LS));
        emit(&e, ek_stop(0));
    } else {
        if (c->stall) {
            /* GETL on the stall tag: one list element with the stall bit and
             * no transfer, so the tag stays incomplete until the stall is
             * acknowledged. */
            put32(img + LIST_LS, 0x80000000u);
            emit(&e, ek_il(5, 0x800));        emit(&e, ek_wrch(MFC_LSA, 5));
            emit(&e, ek_il(5, 0));            emit(&e, ek_wrch(MFC_EAH, 5));
            emit(&e, ek_il(5, LIST_LS));      emit(&e, ek_wrch(MFC_EAL, 5));
            emit(&e, ek_il(5, 8));            emit(&e, ek_wrch(MFC_Size, 5));
            emit(&e, ek_il(5, STALL_TAG));    emit(&e, ek_wrch(MFC_TagID, 5));
            emit(&e, ek_il(5, MFC_GETL_CMD)); emit(&e, ek_wrch(MFC_Cmd, 5));
        }
        /* The CRT's own tag poll: mask -1, immediate update, read the status. */
        emit(&e, ek_il(5, -1));  emit(&e, ek_wrch(MFC_WrTagMask, 5));
        emit(&e, ek_il(5, 0));   emit(&e, ek_wrch(MFC_WrTagUpdate, 5));
        emit(&e, ek_rdch(3, MFC_RdTagStat));
        for (int r = 2; r < 128; r++)
            if (r != 3 && r != 84) emit(&e, ek_il(r, (int)pat(r)));
        emit(&e, ek_bisl(0, 84));
        emit(&e, ek_stop(0));                          /* the call returns here */
    }
    put32(img + brz_pc, ek_brz(7, brz_pc, e.pc));
    emit(&e, ek_stop(0x0A11));

    /* descriptor */
    put64(d + 0x00, ((uint64_t)c->bss_qw << 32) | (BIN_EA | 1u));   /* bit 0: cache inhibit */
    put16(d + 0x08, IMAGE_SIZE >> 4);
    put16(d + 0x0A, 8);                                /* one input DMA entry */
    put32(d + 0x14, c->io_bytes ? c->io_bytes : 0x80);   /* ioBuffer bytes */
    put32(d + 0x18, 0x100);                            /* oBuffer bytes */
    put16(d + 0x1C, c->stack_qw ? c->stack_qw : 0x40);   /* stack, quadwords */
    put16(d + 0x1E, 0x20);                             /* scratch, quadwords */
    put32(d + 0x24, 8);                                /* one cache entry */
    put64(d + 0x30, (0x80ull << 32) | IO_EA);
    put64(d + 0x38, ((uint64_t)(c->cache_bytes ? c->cache_bytes : 0x80u) << 32) | CACHE_EA);
    for (uint32_t i = 0; i < 0x100; i++) {
        g_mem[IO_EA + i] = (uint8_t)(0x10 + i);
        if (i < 0x80) g_mem[CACHE_EA + i] = (uint8_t)(0x90 + i);
    }
}

static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* One input DMA list element: the MFC's 8-byte {size word, EAL} pair. */
static uint64_t dma_elem(uint32_t size, uint32_t ea)
{
    return ((uint64_t)size << 32) | ea;
}

/* Replace the built descriptor's input list with `n` elements. build_job left
 * one element and one cache entry; the loader reads the cache list straight
 * after the input list, so the cache entry moves with it. Every caller keeps
 * the descriptor at JOB_EA, inside guest memory. */
static void patch_dma_list(const uint64_t* elems, uint32_t n)
{
    uint8_t* d = g_mem + JOB_EA;
    const uint64_t cache = be64(d + 0x38);
    put16(d + 0x0A, (uint16_t)(n * 8));
    for (uint32_t i = 0; i < n; i++) put64(d + 0x30 + i * 8, elems[i]);
    put64(d + 0x30 + n * 8, cache);
}

/* ---- what the shims record ----------------------------------------------- */
typedef struct snap { uint8_t ls[SPU_LS_SIZE]; u128 gpr[128]; uint32_t stop_code; } snap;
static snap g_start, g_end;
static int  g_ran;

static void take(snap* s, const spu_context* ctx)
{
    memcpy(s->ls, ctx->ls, SPU_LS_SIZE);
    memcpy(s->gpr, ctx->gpr, sizeof s->gpr);
    s->stop_code = ctx->stop_code;
}

/* The MFC finishes the stalled transfer after the routine has polled it three
 * times: the stall acknowledge a real MFC's host would deliver. */
static int g_stall, g_word, g_tag_reads, g_acks;
static void on_channel(void* user, spu_context* ctx, int is_write, uint32_t ch, uint32_t value)
{
    (void)user; (void)value;
    if (is_write) {
        if (ch == MFC_WrListStallAck) g_acks++;
        return;
    }
    if (ch != MFC_RdTagStat) return;
    g_tag_reads++;
    if (g_stall && g_tag_reads == 4)
        spu_wrch(ctx, MFC_WrListStallAck, spu_splat_u32(STALL_TAG));
}
static const spu_observer g_obs = { 0, 0, 0, 0, on_channel };

static void entry_interp(spu_context* ctx)
{
    g_ran = 1;
    spu_observer_set(ctx, &g_obs);
    take(&g_start, ctx);
    spu_interp_run(ctx, 0);
    take(&g_end, ctx);
}

static void entry_lifted(spu_context* ctx)
{
    g_ran = 1;
    spu_observer_set(ctx, &g_obs);
    take(&g_start, ctx);

    /* the preflight, as lifted loads would do it: ctx+0x14 names the jm object */
    const uint32_t iface = be32(ctx->ls + ctx->gpr[3]._u32[0] + 0x14) & 0x0FFFFFFFu;
    CHECK(iface != 0, "lifted preflight: ctx+0x14 low bits are zero");
    ctx->gpr[84] = spu_splat_u32(be32(ctx->ls + iface + 4u * (uint32_t)g_word));
    if (g_stall) {
        spu_wrch(ctx, MFC_LSA, spu_splat_u32(0x800));
        spu_wrch(ctx, MFC_EAH, spu_splat_u32(0));
        spu_wrch(ctx, MFC_EAL, spu_splat_u32(LIST_LS));
        spu_wrch(ctx, MFC_Size, spu_splat_u32(8));
        spu_wrch(ctx, MFC_TagID, spu_splat_u32(STALL_TAG));
        spu_wrch(ctx, MFC_Cmd, spu_splat_u32(MFC_GETL_CMD));
    }
    spu_wrch(ctx, MFC_WrTagMask, spu_splat_u32(0xFFFFFFFFu));
    spu_wrch(ctx, MFC_WrTagUpdate, spu_splat_u32(0));
    ctx->gpr[3] = spu_rdch(ctx, MFC_RdTagStat);
    for (int r = 2; r < 128; r++)
        if (r != 3 && r != 84) ctx->gpr[r] = spu_splat_u32(pat(r));

    /* bisl $r0, $r84, as the lifter emits it */
    { uint32_t _tg = ctx->gpr[84]._u32[0];
      ctx->gpr[0] = spu_link(LIFTED_RET);
      int32_t _si = (int32_t)ctx->image_id;
      ctx->pc = _tg; ctx->host_depth++;
      spu_indirect_branch(ctx); spu_drain_call(ctx, LIFTED_RET);
      ctx->host_depth--; spu_img_restore(ctx, _si); }
    take(&g_end, ctx);
}

static void run_built(uint32_t job_ea, void (*entry)(spu_context*))
{
    g_ran = 0;
    spurs_job_set_spu_num(NUM_SPU);
    const int rc = spu_run_spurs_job(entry, IMAGE_ID, job_ea, 256);
    CHECK(rc == 0, "spu_run_spurs_job returned %d", rc);
}

static void run_job(const job_cfg* c, void (*entry)(spu_context*))
{
    build_job(c);
    run_built(c->job_ea ? c->job_ea : JOB_EA, entry);
}

/* ---- start-state checks -------------------------------------------------- */
static int all_zero(const uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

static void check_start(const job_cfg* c, int ver14)
{
    const uint8_t* ls = g_start.ls;
    const uint8_t* img = g_mem + BIN_EA;
    const uint32_t bss = c->bss_qw * 16u;
    const uint32_t mem = IMAGE_SIZE + bss;

    /* the image, apart from the kernel view's three zones, byte for byte */
    for (uint32_t i = 0; i < IMAGE_SIZE; i++) {
        const int zone = (i >= 0x170 && i < 0x180) || (i >= 0x1C0 && i < 0x1D0) ||
                         (i >= 0x210 && i < 0x220);
        if (!zone && ls[i] != img[i]) { CHECK(0, "image byte 0x%X is 0x%02X, not 0x%02X", i, ls[i], img[i]); break; }
    }
    /* kernel view: 0x175 tracing off, 0x1C8 the SPU number, 0x1CC the DMA tag,
     * 0x210 trace buffer cleared; the neighbours are the job's own bytes */
    CHECK(all_zero(ls + 0x170, 0x10) && ls[0x175] == 0, "0x170 block not zero");
    CHECK(all_zero(ls + 0x1C0, 8), "0x1C0..0x1C7 not zero");
    CHECK(be32(ls + 0x1C8) == NUM_SPU, "0x1C8 = 0x%X, want the SPU number", be32(ls + 0x1C8));
    CHECK(be32(ls + 0x1CC) == 20, "0x1CC = 0x%X, want the DMA tag 20", be32(ls + 0x1CC));
    CHECK(all_zero(ls + 0x210, 0x10), "0x210 block not zero");

    /* bss: zero, though the guest bytes after the binary are junk */
    CHECK(g_mem[BIN_EA + IMAGE_SIZE] == 0xC3, "fixture: no junk after the guest binary");
    CHECK(all_zero(ls + IMAGE_SIZE, bss), "bss region not zero");

    /* JobContext2 */
    const uint8_t* ctx = ls + CTX_LS;
    CHECK(be32(ctx + 0x14) == ((2u << 28) | JM_LS), "ctx+0x14 = 0x%08X, want 0x%08X",
          be32(ctx + 0x14), (2u << 28) | JM_LS);
    CHECK(be32(ctx + 0x24) == 20, "ctx dmaTag = %u", be32(ctx + 0x24));
    CHECK(be32(ctx + 0x2C) == JOB_EA, "ctx eaJobDescriptor low = 0x%X", be32(ctx + 0x2C));
    CHECK(g_start.gpr[3]._u32[0] == CTX_LS && g_start.gpr[4]._u32[0] == DESC_LS,
          "r3/r4 = 0x%X/0x%X", g_start.gpr[3]._u32[0], g_start.gpr[4]._u32[0]);
    CHECK(g_start.gpr[0]._u32[0] == RET_LS, "r0 = 0x%X", g_start.gpr[0]._u32[0]);
    CHECK(memcmp(ls + DESC_LS, g_mem + JOB_EA, 0x100) == 0, "descriptor copy differs");

    /* buffers sit above image + bss: _cellSpursCheckJob's align1024 arithmetic */
    const uint32_t io = (mem + 1023u) & ~1023u;
    const uint32_t stack_top = io + 0x400 + 0x400 + 0x800;
    CHECK(be32(ctx + 0x00) == io, "ioBuffer = 0x%X, want 0x%X", be32(ctx + 0x00), io);
    CHECK(be32(ctx + 0x1C) == io + 0x400, "oBuffer = 0x%X", be32(ctx + 0x1C));
    CHECK(be32(ctx + 0x20) == io + 0x800, "sBuffer = 0x%X", be32(ctx + 0x20));
    CHECK(be32(ctx + 0x04) == stack_top, "cacheBuffer[0] = 0x%X, want 0x%X", be32(ctx + 0x04), stack_top);
    CHECK(g_start.gpr[1]._u32[0] == ((stack_top - 32u) & ~15u), "r1 = 0x%X, want 0x%X",
          g_start.gpr[1]._u32[0], (stack_top - 32u) & ~15u);
    CHECK(g_start.gpr[1]._u32[0] > mem, "r1 = 0x%X is not above the image and bss (0x%X)",
          g_start.gpr[1]._u32[0], mem);
    CHECK(memcmp(ls + io, g_mem + IO_EA, 0x80) == 0, "ioBuffer DMA data");
    CHECK(memcmp(ls + stack_top, g_mem + CACHE_EA, 0x80) == 0, "cache buffer data");

    /* jm interface and the runtime area below the context */
    const uint8_t* jm = ls + JM_LS;
    static const uint32_t swap[] = { 0x40FFFF83u, 0x21A00B03u, 0x40800003u, 0x21A00B83u,
                                     0x01A00C03u, 0x7CFFC183u, 0x207FFD03u, 0x35000000u };
    if (ver14) {
        CHECK(be32(jm) == RT_LS, "jm word 0 = 0x%X", be32(jm));
        CHECK(be32(jm + 4) == RT_SWAP, "jm word 1 = 0x%X", be32(jm + 4));
        CHECK(all_zero(jm + 8, 8), "jm object tail not zero");
        CHECK(be32(ls + RT_LS) == RT_TRAP_STOP, "trap word = 0x%X", be32(ls + RT_LS));
        int ok = all_zero(ls + RT_LS + 4, 12);
        for (unsigned i = 0; i < sizeof swap / sizeof swap[0]; i++)
            ok &= be32(ls + RT_SWAP + 4 * i) == swap[i];
        CHECK(ok, "the swap routine bytes differ");
        CHECK(all_zero(ls + RT_SWAP + sizeof swap, CTX_LS - (RT_SWAP + (uint32_t)sizeof swap)),
              "the runtime area has stray bytes");
    } else {
        /* Ver13: an empty jm object, and nothing planted below it */
        CHECK(all_zero(jm, 16), "Ver13 jm object not zero");
        CHECK(all_zero(ls + RT_LS, CTX_LS - RT_LS), "Ver13 runtime area not zero");
    }
}

/* ---- modes --------------------------------------------------------------- */
static void mode_start(int ver14, const char* dump)
{
    job_cfg c = { ver14 ? "JOBCRT Ver14" : "JOBCRT Ver13", ver14 ? 0x2C4 : 0, PROG_MAIN, 0 };
    run_job(&c, entry_interp);
    CHECK(g_ran, "the job never ran");
    check_start(&c, ver14);
    /* main ran: the preflight passed (it stops with 0x0A11 otherwise) */
    CHECK(g_end.stop_code == 0, "the job ended with stop 0x%X, not main's stop 0", g_end.stop_code);
    CHECK(be32(g_end.ls + MARK_LS) == MARKER, "main did not run: marker word 0x%08X",
          be32(g_end.ls + MARK_LS));
    if (dump) {
        FILE* f = fopen(dump, "wb");
        if (!f || fwrite(g_start.ls, 1, SPU_LS_SIZE, f) != SPU_LS_SIZE) {
            printf("FAIL: cannot write %s\n", dump);
            g_fail++;
        }
        if (f) fclose(f);
    }
}

static void mode_fit(void)
{
    /* The last region, the cache buffer, ends exactly at the runtime area's
     * base, to the byte: image+bss end at io, io/out/scratch+stack fill the
     * 0x1000 below the cache buffer. */
    job_cfg c = { "JOBCRT Ver14", FIT_BSS_QW, PROG_MAIN, 0, 0, FIT_CACHE, 0 };
    run_job(&c, entry_interp);
    CHECK(g_ran, "an exactly fitting job did not run");
    CHECK(all_zero(g_start.ls + IMAGE_SIZE, FIT_BSS_QW * 16u), "bss not zero");
    const uint32_t cache = be32(g_start.ls + CTX_LS + 0x04);
    CHECK(cache == RT_LS - FIT_CACHE, "cacheBuffer[0] = 0x%X, want 0x%X", cache, RT_LS - FIT_CACHE);
    CHECK(memcmp(g_start.ls + cache, g_mem + CACHE_EA, FIT_CACHE) == 0, "cache buffer data");
    CHECK(be32(g_start.ls + RT_LS) == RT_TRAP_STOP, "the runtime area's trap word was overwritten");
}

static void mode_swap(int lifted, int stall)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_SWAP, stall };
    g_stall = stall;
    g_word = 1;
    run_job(&c, lifted ? entry_lifted : entry_interp);
    CHECK(g_ran, "the job never ran");

    const snap* e = &g_end;
    /* The link: the instruction after bisl. The lifted shim planted
     * LIFTED_RET; the interpreted program has `stop 0` there. */
    uint32_t want_r0 = LIFTED_RET;
    if (!lifted)
        for (uint32_t pc = CODE_LS; pc < LIST_LS; pc += 4)
            if ((be32(g_start.ls + pc) & 0xFFE00000u) == (0x1A9u << 21)) { want_r0 = pc + 4; break; }
    CHECK(e->gpr[0]._u32[0] == want_r0, "r0 = 0x%X after the call, want 0x%X", e->gpr[0]._u32[0], want_r0);
    CHECK(e->gpr[3]._u32[0] == 0xFFFFFFFFu && !e->gpr[3]._u32[1] && !e->gpr[3]._u32[2] && !e->gpr[3]._u32[3],
          "r3 = %08X %08X %08X %08X, want the all-complete tag status",
          e->gpr[3]._u32[0], e->gpr[3]._u32[1], e->gpr[3]._u32[2], e->gpr[3]._u32[3]);
    CHECK(e->gpr[1]._u32[0] == g_start.gpr[1]._u32[0], "r1 changed: 0x%X", e->gpr[1]._u32[0]);
    CHECK(e->gpr[84]._u32[0] == RT_SWAP, "r84 = 0x%X, want the swap routine", e->gpr[84]._u32[0]);
    int bad = 0;
    for (int r = 2; r < 128; r++) {
        if (r == 3 || r == 84) continue;
        for (int w = 0; w < 4; w++)
            if (e->gpr[r]._u32[w] != pat(r)) {
                if (!bad) printf("FAIL: r%d word %d = 0x%08X, want 0x%08X: the call clobbered it\n",
                                 r, w, e->gpr[r]._u32[w], pat(r));
                bad++;
            }
    }
    g_fail += bad != 0;
    /* The wait: the program read the status once, then the routine polled it
     * until the stall was acknowledged on the 4th read; the 5th is complete. */
    CHECK(g_tag_reads == (stall ? 5 : 2), "RdTagStat read %d times, want %d", g_tag_reads, stall ? 5 : 2);
    CHECK(g_acks == (stall ? 1 : 0), "stall acknowledged %d times", g_acks);
    CHECK(lifted || e->stop_code == 0, "the job ended with stop 0x%X", e->stop_code);
}

static void mode_word0(int lifted)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_WORD0, 0 };
    g_stall = 0;
    g_word = 0;
    run_job(&c, lifted ? entry_lifted : entry_interp);
    printf("FAIL: a call through jm word 0 returned instead of stopping\n");
    g_fail++;
}

/* A job that must stop before it runs. */
static void mode_stop_cfg(const job_cfg* c)
{
    run_job(c, entry_interp);
    printf("FAIL: the job ran (%d)\n", g_ran);
    g_fail++;
}

/* A descriptor that must stop before any input byte is staged. */
static void mode_dma_stop(const uint64_t* elems, uint32_t n, uint32_t io_bytes)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0, 0, 0, io_bytes };
    build_job(&c);
    patch_dma_list(elems, n);
    run_built(JOB_EA, entry_interp);
    printf("FAIL: the job ran (%d)\n", g_ran);
    g_fail++;
}

/* The list exactly fills sizeInOut: it must run, both elements in place. */
static void mode_dma_fit(void)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0, 0, 0, 0x90 };
    const uint64_t elems[] = { dma_elem(0x80, IO_EA), dma_elem(0x10, IO_EA + 0x80) };
    build_job(&c);
    patch_dma_list(elems, 2);
    run_built(JOB_EA, entry_interp);
    CHECK(g_ran, "an exactly filling DMA list did not run");
    CHECK(g_end.stop_code == 0, "the job ended with stop 0x%X", g_end.stop_code);
    CHECK(be32(g_end.ls + MARK_LS) == MARKER, "main did not run");
    const uint32_t io = be32(g_start.ls + CTX_LS);
    CHECK(io == 0x1000, "ioBuffer = 0x%X, want 0x1000", io);
    CHECK(memcmp(g_start.ls + io, g_mem + IO_EA, 0x80) == 0, "first element data");
    CHECK(memcmp(g_start.ls + io + 0x80, g_mem + IO_EA + 0x80, 0x10) == 0,
          "second element data did not land at ioBuffer+0x80");
}

/* sizeDmaList describes a list that leaves guest memory before its last
 * element: reading it would run past the VM. */
static void mode_dma_desc_range(void)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0 };
    c.job_ea = 0xFFF00;
    build_job(&c);
    put16(g_mem + c.job_ea + 0x0A, 0x100);   /* 32 entries: 0xFFF30 + 0x100 > 1 MB */
    put32(g_mem + c.job_ea + 0x24, 0);       /* no cache list to read past it */
    run_built(c.job_ea, entry_interp);
    printf("FAIL: the job ran (%d)\n", g_ran);
    g_fail++;
}

/* eaBinary or sizeBinary zero: there is no binary to run. */
static void mode_bin_zero(int ea_zero)
{
    job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0 };
    build_job(&c);
    if (ea_zero) put64(g_mem + JOB_EA, 0);
    else put16(g_mem + JOB_EA + 0x08, 0);
    run_built(JOB_EA, entry_interp);
    printf("FAIL: the job ran (%d)\n", g_ran);
    g_fail++;
}

static void mode_stop(const char* crt, uint32_t bss_qw)
{
    job_cfg c = { crt, bss_qw, PROG_MAIN, 0, 0, 0, 0 };
    mode_stop_cfg(&c);
}

int main(int argc, char** argv)
{
    vm_base = g_mem;
    ppu_vm_size = sizeof g_mem;
    if (argc < 2) { fprintf(stderr, "usage: %s mode [dump]\n", argv[0]); return 2; }
    const char* m = argv[1];

    if (!strcmp(m, "ver13")) mode_start(0, argc > 2 ? argv[2] : NULL);
    else if (!strcmp(m, "ver14")) mode_start(1, NULL);
    else if (!strcmp(m, "fit")) mode_fit();
    else if (!strcmp(m, "swap-interp")) mode_swap(0, 1);
    else if (!strcmp(m, "swap-lifted")) mode_swap(1, 1);
    else if (!strcmp(m, "swap-idle-interp")) mode_swap(0, 0);
    else if (!strcmp(m, "swap-idle-lifted")) mode_swap(1, 0);
    else if (!strcmp(m, "word0-interp")) mode_word0(0);
    else if (!strcmp(m, "word0-lifted")) mode_word0(1);
    else if (!strcmp(m, "ver12")) mode_stop("JOBCRT Ver12", 0);
    else if (!strcmp(m, "ver15")) mode_stop("JOBCRT Ver15", 0);
    else if (!strcmp(m, "garbage")) mode_stop("\x01\x02JOBCRT ver", 0);
    else if (!strcmp(m, "empty")) mode_stop("\0\0\0\0\0\0\0\0\0\0\0", 0);
    else if (!strcmp(m, "overlap")) mode_stop("JOBCRT Ver14", (RT_LS - IMAGE_SIZE) / 16u + 1u);
    else if (!strcmp(m, "image-edge")) {
        /* image + bss ending exactly at the runtime area: the image fits, the
         * stack above it does not */
        job_cfg c = { "JOBCRT Ver14", (RT_LS - IMAGE_SIZE) / 16u, PROG_MAIN, 0, 0, 0, 0 };
        mode_stop_cfg(&c);
    }
    else if (!strcmp(m, "stack-max")) {
        job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0, 0xFFFFu, 0, 0 };
        mode_stop_cfg(&c);
    }
    else if (!strcmp(m, "cache-over")) {
        job_cfg c = { "JOBCRT Ver14", FIT_BSS_QW, PROG_MAIN, 0, 0, FIT_CACHE + 1u, 0 };
        mode_stop_cfg(&c);
    }
    else if (!strcmp(m, "huge-io")) {
        job_cfg c = { "JOBCRT Ver14", 0, PROG_MAIN, 0, 0, 0, 0xFFFFFFFFu };
        mode_stop_cfg(&c);
    }
    else if (!strcmp(m, "dma-fit")) mode_dma_fit();
    else if (!strcmp(m, "dma-over")) {
        /* 0x80-byte buffer, first element exactly fills it, second overruns by
         * one byte */
        const uint64_t e[] = { dma_elem(0x80, IO_EA), dma_elem(1, IO_EA + 0x80) };
        mode_dma_stop(e, 2, 0x80);
    }
    else if (!strcmp(m, "dma-wrap")) {
        /* 32-bit ea + size wraps to 0; only the 64-bit bound sees the overrun */
        ppu_vm_size = 0;
        const uint64_t e[] = { dma_elem(0x20, 0xFFFFFFF0u) };
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-beyond")) {
        const uint64_t e[] = { dma_elem(0x10, 0x200000u) };   /* past the 1 MB VM */
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-zero-ea")) {
        const uint64_t e[] = { dma_elem(0x10, 0) };
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-size-zero")) {
        const uint64_t e[] = { dma_elem(0, IO_EA) };
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-size-limit")) {
        const uint64_t e[] = { dma_elem(0x4001, IO_EA) };   /* one past 16 KB */
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-size-align")) {
        const uint64_t e[] = { dma_elem(0x18, IO_EA) };     /* not a 16-byte multiple */
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-misaligned")) {
        const uint64_t e[] = { dma_elem(8, IO_EA + 4) };
        mode_dma_stop(e, 1, 0x80);
    }
    else if (!strcmp(m, "dma-desc-range")) mode_dma_desc_range();
    else if (!strcmp(m, "bin-ea-zero")) mode_bin_zero(1);
    else if (!strcmp(m, "bin-size-zero")) mode_bin_zero(0);
    else { fprintf(stderr, "unknown mode %s\n", m); return 2; }

    if (g_fail) return 1;
    printf("PASS: %s\n", m);
    return 0;
}

/* ---- link-only boundaries ------------------------------------------------
 * The channel layer references the SPURS workload/taskset layer and the
 * DMA diagnostics, none of which the synthetic jobs reach: a SPURS syscall, an
 * event-flag path, a repeated identical DMA. They abort if that stops holding. */
static void unused(const char* what) __attribute__((noreturn, cold));
static void unused(const char* what)
{
    fprintf(stderr, "test_spurs_job_crt: unexpected call into %s\n", what);
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
void spu_taskset_line_committed(uint32_t line) { (void)line; }
