/* spuexh SPU program: the exhaustive SPU instruction sweep on a real SPU.
 *
 * Compiles runtime/spu/tests/spu_exh_gen.h UNCHANGED (generator, domains, block hash), so a hash computed here is
 * directly comparable with the host harness (test_spu_exhaustive). The only SPU-specific code is below: how an op is
 * executed and how results leave the SPU.
 *
 * EXECUTING AN OP. Every op runs as the real instruction. The word is built from the generated template
 * (spuexh_ops_gen.h: opcode, registers a=$3 b=$4 c=$5, immediate field), stored into a four-word stub in local store,
 * `sync`ed (the SPU's instruction-fetch barrier after a store into code) and called as a function:
 *
 *     stub:  <instruction word>   ; computes into $rt
 *            ori $3,$rt,0         ; result to the return register
 *            bi  $0
 *            lnop
 *
 * No compiler intrinsic is involved, so nothing can be folded, contracted, reassociated or "improved" by the compiler.
 * An op with an immediate field patches the word when the call's immediate changes (the immediate domains hold one
 * immediate over thousands of calls). The same stub executes the special probes' words.
 *
 * MODES (spuexh_params_t.mode): MAIN sweep of the job plan; CASES a list of (word, a, b, c) with $rt recorded;
 * RAW one word with all 128 registers loaded and dumped (spuexh_rawexec_gen.S).
 *
 * This file is big-endian code (SPU): the wire structures are used without byte swapping.
 */
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <string.h>
#include <stdint.h>

#include "spuexh.h"

extern void spu_thread_exit(int status);
extern void spuexh_rawexec(void);
extern volatile uint32_t spuexh_slot[];
extern vec_uint4 spuexh_save[128], spuexh_init[128], spuexh_dump[128];

#define TAG 1
#define CHUNK 64                         /* calls generated per chunk; each active op then runs over the chunk */
#define ENT_MAX 640                      /* entries one job can produce (ops of a domain + timing record) */
#define MAXCASES 96

typedef vec_uint4 (*stubfn_t)(vec_uint4, vec_uint4, vec_uint4);

static volatile uint32_t stubmem[4] __attribute__((aligned(16))) = { 0x40200000u, 0x04000183u, 0x35000000u, 0x00200000u };

static spuexh_params_t P __attribute__((aligned(128)));
static spuexh_status_t ST __attribute__((aligned(128)));
static union { spuexh_cfg_t c; uint8_t raw[512]; } CFGU __attribute__((aligned(16)));   /* the config DMA is rounded up to 16 bytes */
#define CFG (CFGU.c)
static spuexh_entry_t ENT[ENT_MAX] __attribute__((aligned(128)));
static spuexh_case_t CASES[MAXCASES] __attribute__((aligned(128)));
static spuexh_caseout_t CO __attribute__((aligned(128)));
static vec_uint4 A[CHUNK], B[CHUNK], C[CHUNK];
static int32_t IMM[CHUNK];

static uint32_t g_entries;               /* entries stored in the results region so far */

/* ------------------------------------------------------------------ DMA */
static void dma_wait(void) {
    mfc_write_tag_mask(1u << TAG);
    mfc_read_tag_status_all();
}
static void dma_get(void *ls, uint64_t ea, uint32_t size) {
    while (size) {
        uint32_t n = size > 16384 ? 16384 : size;
        mfc_get(ls, ea, n, TAG, 0, 0);
        ls = (char *)ls + n; ea += n; size -= n;
    }
    dma_wait();
}
static void dma_put(const void *ls, uint64_t ea, uint32_t size, int fenced) {
    while (size) {
        uint32_t n = size > 16384 ? 16384 : size;
        if (fenced) mfc_putf((void *)ls, ea, n, TAG, 0, 0); else mfc_put((void *)ls, ea, n, TAG, 0, 0);
        ls = (const char *)ls + n; ea += n; size -= n;
    }
}
static void status_put(void) {          /* the previous status DMA must have finished: the buffer is reused */
    dma_wait();
    dma_put(&ST, P.status_ea, sizeof ST, 1);
}

/* ------------------------------------------------------------------ the stub */
static inline void stub_set(uint32_t word, int rt) {
    stubmem[0] = word;
    stubmem[1] = 0x04000003u | ((uint32_t)rt << 7);
    __asm__ volatile("sync" ::: "memory");
}
static inline V4 to_v4(vec_uint4 r) {
    union { vec_uint4 v; V4 s; } u;
    u.v = r;
    return u.s;
}

/* One op over a chunk of n calls; h = running block hash of this op. */
static uint64_t op_chunk(const spuexh_op_t *op, int n, uint64_t h) {
    const stubfn_t fn = (stubfn_t)stubmem;
    if (!op->imm_bits) {
#ifdef SPUEXH_MUTANT          /* test_hwprobe_spuexh.py plants a defect: absdb executes avgb's word, to prove a wrong adapter is caught */
        if (!strcmp(op->name, "absdb")) { for (int k = 0; k < SPUEXH_NOPS; k++) if (!strcmp(spuexh_ops[k].name, "avgb")) op = &spuexh_ops[k]; }
#endif
        stub_set(op->word, op->rt);
        for (int j = 0; j < n; j++) h = exh_fold(h, to_v4(fn(A[j], B[j], C[j])));
        return h;
    }
    const uint32_t mask = (1u << op->imm_bits) - 1u;
    int32_t cur = 0; int have = 0;
    for (int j = 0; j < n; j++) {
        const int32_t im = op->fixed_imm >= 0 ? op->fixed_imm : IMM[j];
        if (!have || im != cur) {
            stub_set(op->word | (((uint32_t)im & mask) << op->imm_shift), op->rt);
            cur = im; have = 1;
        }
        h = exh_fold(h, to_v4(fn(A[j], B[j], C[j])));
    }
    return h;
}

/* ------------------------------------------------------------------ MAIN mode */
static void run_job(int di, uint64_t blk, const uint16_t *act, int na) {
    const exh_dom *d = &exh_doms[di];
    uint64_t h[ENT_MAX];
    uint32_t need = 0;
    for (int q = 0; q < na; q++) { h[q] = EXH_FNV_OFFSET; need |= spuexh_ops[act[q]].need; }
    const uint64_t lo = blk << EXH_BLOCK_LOG2;
    uint64_t hi = lo + EXH_BLOCK; if (hi > d->ncalls) hi = d->ncalls;

    spu_writech(SPU_WrDec, 0xFFFFFFFFu);
    uint64_t chunks = 0;
    /* The decrementer counts down one period of ~53.8 s at 79.8 MHz (2^32 ticks). Sampling it as the job runs
     * detects a wrap; without that a long job would report a small tick count (0xFFFFFFFF - remaining undercounts
     * by whole periods). A wrapped job is reported as SPUEXH_TIME_OVERFLOW, not as a number. */
    uint32_t dec_prev = 0xFFFFFFFFu, dec_wraps = 0;
    for (uint64_t i = lo; i < hi; i += CHUNK, chunks++) {
        const int n = (hi - i) < CHUNK ? (int)(hi - i) : CHUNK;
        G g __attribute__((aligned(16)));
        for (int j = 0; j < n; j++) {
            d->gen(d, i + (uint64_t)j, &g, need);
            memcpy(&A[j], &g.a, 16); memcpy(&B[j], &g.b, 16); memcpy(&C[j], &g.c, 16);
            IMM[j] = g.imm;
        }
        for (int q = 0; q < na; q++) h[q] = op_chunk(&spuexh_ops[act[q]], n, h[q]);
        { const uint32_t cur = spu_readch(SPU_RdDec); if (cur > dec_prev) dec_wraps++; dec_prev = cur; }
        if ((chunks & 63u) == 63u) {            /* heartbeat: lets the PPU tell a long job from a hang */
            dma_wait();
            ST.heartbeat++;
            dma_put(&ST, P.status_ea, sizeof ST, 1);
        }
    }
    { const uint32_t cur = spu_readch(SPU_RdDec); if (cur > dec_prev) dec_wraps++; dec_prev = cur; }
    const uint32_t ticks = dec_wraps ? SPUEXH_TIME_OVERFLOW : 0xFFFFFFFFu - dec_prev;

    dma_wait();                                  /* the previous entries/status DMA is done: the buffers are free */
    int ne = 0;
    for (int q = 0; q < na; q++, ne++) {
        ENT[ne].hash = h[q]; ENT[ne].op = act[q]; ENT[ne].dom = (uint16_t)di; ENT[ne].blk = (uint32_t)blk;
    }
    ENT[ne].hash = ticks; ENT[ne].op = SPUEXH_ENT_TIME; ENT[ne].dom = (uint16_t)di; ENT[ne].blk = (uint32_t)blk; ne++;
    if (g_entries + (uint32_t)ne > P.results_cap) {          /* cannot happen with the PPU's sizing; refuse rather than overrun */
        ST.flags |= 1u;
        return;
    }
    dma_put(ENT, P.results_ea + (uint64_t)g_entries * sizeof(spuexh_entry_t), (uint32_t)ne * sizeof(spuexh_entry_t), 0);
    g_entries += (uint32_t)ne;
    ST.entries = g_entries;
    ST.jobs_done++;
    ST.busy_ticks += ticks;
    dma_put(&ST, P.status_ea, sizeof ST, 1);     /* fenced: after the entries, same tag */
}

static void run_main(void) {
    uint16_t act[SPUEXH_NOPS];
    exh_init();
    if (P.cfg_size > sizeof CFGU.raw) return;
    dma_get(&CFG, P.cfg_ea, (P.cfg_size + 15u) & ~15u);
    g_entries = P.start_entries;
    ST.entries = g_entries;
    if (P.spu_index == 0 && P.start_entries == 0 && P.start_job == 0) {         /* generator self-check, once per run */
        for (int di = 0; di < EXH_NDOMS; di++) {
            ENT[di].hash = spuexh_gencheck(di); ENT[di].op = SPUEXH_ENT_GEN; ENT[di].dom = (uint16_t)di; ENT[di].blk = 0;
        }
        dma_put(ENT, P.results_ea, (uint32_t)EXH_NDOMS * sizeof(spuexh_entry_t), 0);
        g_entries += (uint32_t)EXH_NDOMS;
        ST.entries = g_entries;
        dma_put(&ST, P.status_ea, sizeof ST, 1);
    }
    uint32_t job = 0, mine = 0;
    for (int di = 0; di < EXH_NDOMS; di++) {
        if (!spuexh_dom_enabled(&CFG, di)) continue;
        const uint64_t nb = exh_nblocks(&exh_doms[di]);
        for (uint64_t blk = 0; blk < nb; blk++) {
            const int na = spuexh_job_ops(&CFG, di, blk, act);
            if (!na) continue;
            if ((job++ % P.spu_count) != P.spu_index) continue;
            if (mine++ < P.start_job) continue;
            ST.jobs_started = mine; ST.last_dom = (uint32_t)di; ST.last_blk = (uint32_t)blk;
            status_put();                         /* "job in progress" first: a fault names the job */
            run_job(di, blk, act, na);
            if (ST.flags & 1u) return;
        }
    }
}

/* ------------------------------------------------------------------ CASES mode (special probe A) */
static void run_cases(void) {
    const uint32_t n = P.ncases > MAXCASES ? MAXCASES : P.ncases;
    dma_get(CASES, P.cases_ea, n * (uint32_t)sizeof(spuexh_case_t));
    for (uint32_t i = 0; i < n; i++) {
        memset(&CO, 0, sizeof CO);
        CO.magic = SPUEXH_STATUS_MAGIC; CO.id = CASES[i].id; CO.state = 1;
        dma_wait();
        dma_put(&CO, P.results_ea + (uint64_t)i * SPUEXH_CASEOUT_SIZE, SPUEXH_CASEOUT_SIZE, 1);
        dma_wait();                               /* "about to execute" is in main memory before the word runs */
        ST.cases_started = i + 1; status_put(); dma_wait();
        const int rt = (int)(CASES[i].flags & 0x7Fu);
        stub_set(CASES[i].word, rt);
        const stubfn_t fn = (stubfn_t)stubmem;
        vec_uint4 a, b, c;
        memcpy(&a, CASES[i].a, 16); memcpy(&b, CASES[i].b, 16); memcpy(&c, CASES[i].c, 16);
        const vec_uint4 r = fn(a, b, c);
        memcpy(CO.r, &r, 16);
        CO.state = 2;
        dma_put(&CO, P.results_ea + (uint64_t)i * SPUEXH_CASEOUT_SIZE, SPUEXH_CASEOUT_SIZE, 1);
        ST.cases_done = i + 1; status_put();
    }
}

/* ------------------------------------------------------------------ RAW mode (special probe B) */
static void arena_fill(void) {
    for (uint32_t a = SPUEXH_ARENA_A; a < SPUEXH_ARENA_A + SPUEXH_ARENA_A_SIZE; a += 4) *(volatile uint32_t *)a = 0xA0000000u ^ a * 0x9E3779B1u;
    for (uint32_t a = SPUEXH_ARENA_B; a < SPUEXH_ARENA_B + SPUEXH_ARENA_B_SIZE; a += 4) *(volatile uint32_t *)a = 0xB0000000u ^ a * 0x85EBCA6Bu;
}
static void arena_check(spuexh_caseout_t *co) {
    for (uint32_t a = SPUEXH_ARENA_A; a < SPUEXH_ARENA_A + SPUEXH_ARENA_A_SIZE; a += 4)
        if (*(volatile uint32_t *)a != (0xA0000000u ^ a * 0x9E3779B1u)) { co->arena_hit = 1; co->arena_addr = a; co->arena_val = *(volatile uint32_t *)a; return; }
    for (uint32_t a = SPUEXH_ARENA_B; a < SPUEXH_ARENA_B + SPUEXH_ARENA_B_SIZE; a += 4)
        if (*(volatile uint32_t *)a != (0xB0000000u ^ a * 0x85EBCA6Bu)) { co->arena_hit = 2; co->arena_addr = a; co->arena_val = *(volatile uint32_t *)a; return; }
}
static void init_regs(void) {
    /* word 0 of every register points into arena A (so a register used as an address stays in the arena, and the sum
       of two lands in arena B); the other words are distinct patterns so a changed register is recognisable */
    for (uint32_t n = 0; n < 128; n++) {
        vec_uint4 v = { SPUEXH_ARENA_A + 16u * n, 0x11111111u * (n & 15u) + n, 0x5A5A0000u + n * 0x101u, ~(n * 0x01010101u) };
        spuexh_init[n] = v;
    }
}
static void run_raw(void) {
    const uint32_t n = P.ncases > MAXCASES ? MAXCASES : P.ncases;
    dma_get(CASES, P.cases_ea, n * (uint32_t)sizeof(spuexh_case_t));
    for (uint32_t i = 0; i < n; i++) {
        init_regs(); arena_fill();
        memset(&CO, 0, sizeof CO);
        CO.magic = SPUEXH_STATUS_MAGIC; CO.id = CASES[i].id; CO.state = 1;
        dma_wait();
        dma_put(&CO, P.results_ea + (uint64_t)i * SPUEXH_CASEOUT_SIZE, SPUEXH_CASEOUT_SIZE, 1);
        dma_wait();
        ST.cases_started = i + 1; status_put(); dma_wait();
        spuexh_slot[0] = CASES[i].word;
        __asm__ volatile("sync" ::: "memory");
        spuexh_rawexec();                         /* returns only if the word fell through */
        uint32_t nch = 0;
        for (uint32_t r = 0; r < 128; r++) {
            const uint32_t *x = (const uint32_t *)&spuexh_init[r], *y = (const uint32_t *)&spuexh_dump[r];
            if (x[0] != y[0] || x[1] != y[1] || x[2] != y[2] || x[3] != y[3]) {
                if (nch < SPUEXH_RAW_MAXCH) { CO.ch[nch].reg = r; memcpy(CO.ch[nch].v, y, 16); }
                nch++;
            }
        }
        CO.nchanged = nch;
        arena_check(&CO);
        CO.state = 2;
        dma_put(&CO, P.results_ea + (uint64_t)i * SPUEXH_CASEOUT_SIZE, SPUEXH_CASEOUT_SIZE, 1);
        ST.cases_done = i + 1; status_put();
    }
}

int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4) {
    (void)arg2; (void)arg3; (void)arg4;
    dma_get(&P, arg1, sizeof P);
    memset(&ST, 0, sizeof ST);
    ST.magic = SPUEXH_STATUS_MAGIC; ST.state = 1;
    /* Defined FPSCR before anything runs (SPU ISA v1.2 section 9.3 "Floating-Point Status and Control Register",
     * printed pp. 199-201): the write puts 0 in every bit, i.e. RN0/RN1 (bits 20:21/22:23) = 00 round-to-nearest-even
     * for the 2-way SIMD double-precision operations, and every sticky status bit (single-precision OVF/UNF/DIFF at
     * 29:31, 61:63, 93:95, 125:127 and per-slice DBZ at 116:119; double-precision OVF/UNF/INX/INV/NaN/DENORM at
     * 50:55 and 82:87) is cleared. fscrrd then reads the value back for the per-launch META rows. */
    spu_mtfpscr(spu_splats(0u));
    { const vec_uint4 f = spu_mffpscr(); memcpy(ST.fpscr_start, &f, 16); }
    if (P.magic != SPUEXH_PARAMS_MAGIC) { ST.state = 3; ST.flags |= 2u; status_put(); dma_wait(); spu_thread_exit(2); }
    status_put();
    const uint32_t t0 = 0;
    (void)t0;
    switch (P.mode) {
    case SPUEXH_MODE_MAIN: run_main(); break;
    case SPUEXH_MODE_CASES: run_cases(); break;
    case SPUEXH_MODE_RAW: run_raw(); break;
    default: ST.flags |= 4u; break;
    }
    { const vec_uint4 f = spu_mffpscr(); memcpy(ST.fpscr_end, &f, 16); }
    ST.state = 2;
    status_put();
    dma_wait();
    spu_thread_exit(0);
    return 0;
}
