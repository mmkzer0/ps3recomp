/*
 * test_spu_exhaustive -- exhaustive / structured cross-backend parity sweep of the SPU op helpers.
 *
 * Layout (the split matters: only part 2 touches the helpers):
 *   spu_exh_gen.h   portable generator, domain table and block hash (no SPU / SIMD / float code).
 *   part 1 below    adapters V4 <-> u128 and one wrapper per op (the ONLY helper-dependent code), op table.
 *   part 2 below    driver: iterate (op, domain, block), hash, optional fast-vs-_ref check, dump.
 *
 * Every build (scalar, SSE2, SSE4.1, AVX2, AVX-512, NEON) is the same source; the scalar build
 * (-DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT, which also makes spu_shufb a byte loop) is the reference and every
 * other build must hash identically. Output: one text line per block  "op<TAB>domain<TAB>block<TAB>hash16hex".
 *
 *   test_spu_exhaustive [--out F] [--tag T] [--ops a,b*] [--doms d,e] [--shard K/N] [--refcheck]
 *                       [--stride-heavy S] [--stride-all S] [--stride-dom NAME=S]... [--fpcr-rz] [--list]
 *                       [--dump OP DOM BLOCK [--dump-n N]]
 * Build/run: see build_exhaustive.sh, run_exhaustive.sh, compare_exhaustive.py. `ctest -L long` runs a strided
 * sweep of the host's build against the scalar one plus a -DEXH_MUTANT self-test (run_exhaustive_ctest.py).
 */
#define _GNU_SOURCE
#include "spu_helpers.h"
#include "spu_exh_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- part 1: adapters + op table */
static inline u128 ld(V4 v) { u128 r; for (int k = 0; k < 4; k++) r._u32[k] = v.w[k]; return r; }
static inline V4 st(u128 r) { V4 v; for (int k = 0; k < 4; k++) v.w[k] = r._u32[k]; return v; }

#define A1(n)  static V4 f_##n(const G* g) { return st(spu_##n(ld(g->a))); }
#define A2(n)  static V4 f_##n(const G* g) { return st(spu_##n(ld(g->a), ld(g->b))); }
#define A3(n)  static V4 f_##n(const G* g) { return st(spu_##n(ld(g->a), ld(g->b), ld(g->c))); }
/* An immediate-form instruction can only carry its own field, so the generator's wide immediates (spu_exh_gen.h:
 * -1024..1023 plus 128 wide values) are cut to that field before they reach a helper. Field widths and extension
 * follow the decoder forms in spu_interp_tables.inc (I10/I7S/I16S sign-extend; I7U/I16U/I18 zero-extend); the
 * generator stream itself stays untouched. */
static inline int32_t enc_sx(int32_t v, int bits) { const uint32_t m = 1u << (bits - 1); return (int32_t)((((uint32_t)v & ((m << 1) - 1u)) ^ m) - m); }
static inline int32_t enc_zx(int32_t v, int bits) { return (int32_t)((uint32_t)v & ((1u << bits) - 1u)); }
#define E_I10(v)  enc_sx((v), 10)
#define E_I7S(v)  enc_sx((v), 7)
#define E_I7U(v)  enc_zx((v), 7)
#define E_I8(v)   enc_zx((v), 8)
#define E_I16S(v) enc_sx((v), 16)
#define E_I16U(v) enc_zx((v), 16)
#define E_I18(v)  enc_zx((v), 18)
#define AI(n, E)  static V4 f_##n(const G* g) { return st(spu_##n(ld(g->a), E(g->imm))); }
#define R1(n)  static V4 r_##n(const G* g) { return st(spu_##n##_ref(ld(g->a))); }
#define R2(n)  static V4 r_##n(const G* g) { return st(spu_##n##_ref(ld(g->a), ld(g->b))); }
#define R3(n)  static V4 r_##n(const G* g) { return st(spu_##n##_ref(ld(g->a), ld(g->b), ld(g->c))); }
#define RI(n, E)  static V4 r_##n(const G* g) { return st(spu_##n##_ref(ld(g->a), E(g->imm))); }

/* integer, word/half/byte binary */
A2(a) A2(sf) A2(and) A2(or) A2(xor) A2(nand) A2(nor) A2(andc) A2(orc) A2(eqv) A2(ceq) A2(cgt) A2(clgt) A2(cg) A2(bg)
A2(mpy) A2(mpyu) A2(mpyh) A2(mpyhh) A2(mpyhhu) A2(mpys) A2(sumb)
R2(a) R2(sf) R2(and) R2(or) R2(xor) R2(ceq) R2(cgt) R2(clgt) R2(cg) R2(bg) R2(mpy) R2(mpyu) R2(mpyh) R2(mpyhh) R2(mpyhhu) R2(mpys) R2(sumb)
A3(selb) A3(mpya) A3(mpyhha) A3(mpyhhau) A3(addx) A3(sfx) A3(cgx) A3(bgx) A3(shufb)
R3(selb) R3(mpya) R3(mpyhha) R3(mpyhhau) R3(addx) R3(sfx) R3(cgx) R3(bgx) R3(shufb)
A2(absdb) A2(avgb) A2(ceqb) A2(cgtb) A2(clgtb) R2(absdb) R2(avgb) R2(ceqb) R2(cgtb) R2(clgtb)
A2(ceqh) A2(cgth) A2(clgth) A2(ah) A2(sfh) A2(shlh) A2(roth) A2(rothm) A2(rothma)
R2(ceqh) R2(cgth) R2(clgth) R2(ah) R2(sfh) R2(shlh) R2(roth) R2(rothm) R2(rothma)
A2(shl) A2(rot) A2(rotm) A2(rotma) R2(shl) R2(rot) R2(rotm) R2(rotma)
A2(shlqbi) A2(rotqbi) A2(rotqmbi) A2(shlqby) A2(rotqby) A2(rotqmby) A2(shlqbybi) A2(rotqbybi) A2(rotqmbybi)
R2(shlqby) R2(rotqby) R2(rotqmby) R2(shlqbybi) R2(rotqbybi) R2(rotqmbybi)
A2(cbx) A2(chx) A2(cwx) A2(cdx)
A1(cntb) A1(xsbh) A1(xshw) A1(clz) A1(xswd) A1(orx) A1(gb) A1(gbh) A1(gbb) A1(fsm) A1(fsmh) A1(fsmb)
R1(cntb) R1(xsbh) R1(xshw) R1(clz) R1(xswd) R1(orx) R1(gb) R1(gbh) R1(gbb) R1(fsm) R1(fsmh) R1(fsmb)
/* immediate forms */
AI(ai, E_I10) AI(sfi, E_I10) AI(andi, E_I10) AI(ori, E_I10) AI(xori, E_I10) AI(ceqi, E_I10) AI(cgti, E_I10) AI(clgti, E_I10)
RI(ai, E_I10) RI(sfi, E_I10) RI(andi, E_I10) RI(ori, E_I10) RI(xori, E_I10) RI(ceqi, E_I10) RI(cgti, E_I10) RI(clgti, E_I10)
AI(ahi, E_I10) AI(sfhi, E_I10) AI(ceqhi, E_I10) AI(cgthi, E_I10) AI(clgthi, E_I10) AI(andhi, E_I10) AI(orhi, E_I10) AI(xorhi, E_I10)
RI(ahi, E_I10) RI(sfhi, E_I10) RI(ceqhi, E_I10) RI(cgthi, E_I10) RI(clgthi, E_I10)
AI(andbi, E_I10) AI(orbi, E_I10) AI(xorbi, E_I10) AI(ceqbi, E_I10) AI(cgtbi, E_I10) AI(clgtbi, E_I10)
RI(ceqbi, E_I10) RI(cgtbi, E_I10) RI(clgtbi, E_I10)
AI(mpyi, E_I10) AI(mpyui, E_I10) RI(mpyi, E_I10) RI(mpyui, E_I10)
AI(shli, E_I7S) AI(shlhi, E_I7S) AI(roti, E_I7S) AI(rothi, E_I7S) AI(rotmi, E_I7S) AI(rotmhi, E_I7S) AI(rothmi, E_I7S) AI(rotmai, E_I7S) AI(rotmahi, E_I7S)
RI(roti, E_I7S) RI(rothi, E_I7S) RI(rotmai, E_I7S) RI(rotmahi, E_I7S)
AI(shlqbii, E_I7S) AI(rotqbii, E_I7S) AI(rotqmbii, E_I7S) AI(shlqbyi, E_I7S) AI(rotqbyi, E_I7S) AI(rotqmbyi, E_I7S)
RI(shlqbyi, E_I7S) RI(rotqbyi, E_I7S) RI(rotqmbyi, E_I7S)
AI(cbd, E_I7U) AI(chd, E_I7U) AI(cwd, E_I7U) AI(cdd, E_I7U) RI(cwd, E_I7U)
static V4 f_fsmbi(const G* g) { return st(spu_fsmbi(E_I16U(g->imm))); }
static V4 f_il(const G* g)   { return st(spu_il(E_I16S(g->imm))); }
static V4 f_ila(const G* g)  { return st(spu_ila((uint32_t)E_I18(g->imm))); }
static V4 f_ilh(const G* g)  { return st(spu_ilh((uint16_t)E_I16U(g->imm))); }
static V4 f_ilhu(const G* g) { return st(spu_ilhu((uint16_t)E_I16U(g->imm))); }
static V4 f_iohl_(const G* g) { return st(spu_iohl(ld(g->a), (uint16_t)E_I16U(g->imm))); }
/* single precision */
A2(fa) A2(fs) A2(fm) A3(fma) A3(fms) A3(fnms) A2(fceq) A2(fcgt) A2(fcmeq) A2(fcmgt) A2(fi) A1(frest) A1(frsqest)
R2(fa) R2(fs) R2(fm) R3(fma) R3(fms) R3(fnms) R2(fceq) R2(fcgt) R2(fcmeq) R2(fcmgt) R2(fi) R1(frest) R1(frsqest)
AI(cflts, E_I8) AI(cfltu, E_I8) AI(csflt, E_I8) AI(cuflt, E_I8) RI(cflts, E_I8) RI(cfltu, E_I8) RI(csflt, E_I8) RI(cuflt, E_I8)
/* double precision (dfceq/dfcmeq/dfcgt/dfcmgt/dftsv have no rows: a retail CBE
 * SPU raises an invalid-instruction error for them, hwprobe spuexh special A,
 * issue #124 -- nothing models them) */
A2(dfa) A2(dfs) A2(dfm) A3(dfma) A3(dfms) A3(dfnms) A3(dfnma) A1(frds) A1(fesd)

typedef struct {
    const char* name; V4 (*fn)(const G*); V4 (*ref)(const G*); const char* doms;
    int has_imm, imm, heavy, need;
} OP;
#define O(n, nd, doms)        { #n, f_##n, NULL, doms, 0, 0, 0, nd }
#define OR_(n, nd, doms)      { #n, f_##n, r_##n, doms, 0, 0, 0, nd }
#define OFIX(nm, fn, rf, doms, im, hv) { nm, fn, rf, doms, 1, im, hv }
#define WB  "wpool2,wrand"
#define HB  "half2,wrand"
#define BB  "byte2,wpool2,wrand"
#define F2D "fpool2,frand2,fnear2"
#define D2D "dpool2,drand2,dnear2"

static const OP g_ops[] = {
    /* byte lane */
    OR_(absdb, 3, BB), OR_(avgb, 3, BB), OR_(ceqb, 3, BB), OR_(cgtb, 3, BB), OR_(clgtb, 3, BB),
    OR_(cntb, 1, "w32,byte2"), OR_(sumb, 3, WB),
    /* halfword lane: all 2^32 (a,b) pairs */
    OR_(ceqh, 3, HB), OR_(cgth, 3, HB), OR_(clgth, 3, HB), OR_(ah, 3, HB), OR_(sfh, 3, HB), OR_(shlh, 3, HB), OR_(roth, 3, HB), OR_(rothm, 3, HB), OR_(rothma, 3, HB),
    OR_(xsbh, 1, "w32"), OR_(xshw, 1, "w32"),
    /* word lane */
    OR_(a, 3, WB), OR_(sf, 3, WB), OR_(and, 3, WB), OR_(or, 3, WB), OR_(xor, 3, WB), O(nand, 3, WB), O(nor, 3, WB), O(andc, 3, WB), O(orc, 3, WB), O(eqv, 3, WB),
    OR_(ceq, 3, WB), OR_(cgt, 3, WB), OR_(clgt, 3, WB), OR_(cg, 3, WB), OR_(bg, 3, WB),
    OR_(mpy, 3, WB ",h16ll"), OR_(mpyu, 3, WB ",h16ll"), OR_(mpyh, 3, WB ",h16hl"), OR_(mpyhh, 3, WB ",h16hh"), OR_(mpyhhu, 3, WB ",h16hh"), OR_(mpys, 3, WB ",h16ll"),
    OR_(selb, 7, WB), OR_(mpya, 7, WB ",h16ll"), OR_(mpyhha, 7, WB ",h16hh"), OR_(mpyhhau, 7, WB ",h16hh"), OR_(addx, 7, WB), OR_(sfx, 7, WB), OR_(cgx, 7, WB), OR_(bgx, 7, WB),
    OR_(clz, 1, "w32"), OR_(xswd, 1, "w32o,wrand"), OR_(orx, 1, WB),
    OR_(gb, 1, "bits16,wrand"), OR_(gbh, 1, "bits16,wrand"), OR_(gbb, 1, "bits16,wrand"),
    OR_(fsm, 1, "qcnta"), OR_(fsmh, 1, "qcnta"), OR_(fsmb, 1, "qcnta"),
    /* variable shifts / rotates: every 32-bit count */
    OR_(shl, 3, "wcnt,wrand"), OR_(rot, 3, "wcnt,wrand"), OR_(rotm, 3, "wcnt,wrand"), OR_(rotma, 3, "wcnt,wrand"),
    O(shlqbi, 3, "qcnt"), O(rotqbi, 3, "qcnt"), O(rotqmbi, 3, "qcnt"),
    OR_(shlqby, 3, "qcnt"), OR_(rotqby, 3, "qcnt"), OR_(rotqmby, 3, "qcnt"), OR_(shlqbybi, 3, "qcnt"), OR_(rotqbybi, 3, "qcnt"), OR_(rotqmbybi, 3, "qcnt"),
    O(cbx, 3, "qcnt"), O(chx, 3, "qcnt"), O(cwx, 3, "qcnt"), O(cdx, 3, "qcnt"),
    /* shufb */
    OR_(shufb, 7, "shufb_pos,shufb_rand,shufb_all"),
    { "fsmbi", f_fsmbi, NULL, "fsmbi", 0, 0, 0, 0 },
    /* immediates */
    OR_(ai, 1, "immw"), OR_(sfi, 1, "immw"), OR_(andi, 1, "immw"), OR_(ori, 1, "immw"), OR_(xori, 1, "immw"), OR_(ceqi, 1, "immw"), OR_(cgti, 1, "immw"), OR_(clgti, 1, "immw"),
    OR_(ahi, 1, "immh"), OR_(sfhi, 1, "immh"), OR_(ceqhi, 1, "immh"), OR_(cgthi, 1, "immh"), OR_(clgthi, 1, "immh"), O(andhi, 1, "immh"), O(orhi, 1, "immh"), O(xorhi, 1, "immh"),
    O(andbi, 1, "immb"), O(orbi, 1, "immb"), O(xorbi, 1, "immb"), OR_(ceqbi, 1, "immb"), OR_(cgtbi, 1, "immb"), OR_(clgtbi, 1, "immb"),
    OR_(mpyi, 1, "immh"), OR_(mpyui, 1, "immh"),
    O(shli, 1, "immw"), O(shlhi, 1, "immh"), OR_(roti, 1, "immw"), OR_(rothi, 1, "immh"), O(rotmi, 1, "immw"), O(rotmhi, 1, "immh"), O(rothmi, 1, "immh"),
    OR_(rotmai, 1, "immw"), OR_(rotmahi, 1, "immh"),
    O(shlqbii, 1, "immw"), O(rotqbii, 1, "immw"), O(rotqmbii, 1, "immw"), OR_(shlqbyi, 1, "immw"), OR_(rotqbyi, 1, "immw"), OR_(rotqmbyi, 1, "immw"),
    O(cbd, 1, "immw"), O(chd, 1, "immw"), OR_(cwd, 1, "immw"), O(cdd, 1, "immw"),
    { "iohl", f_iohl_, NULL, "immw", 0, 0, 0, 1 },
    { "il", f_il, NULL, "immw", 0, 0, 0, 0 }, { "ila", f_ila, NULL, "immw", 0, 0, 0, 0 }, { "ilh", f_ilh, NULL, "immw", 0, 0, 0, 0 }, { "ilhu", f_ilhu, NULL, "immw", 0, 0, 0, 0 },
    /* single precision */
    OR_(fa, 3, F2D ",fovf2"), OR_(fs, 3, F2D ",fovf2"), OR_(fm, 3, F2D ",fovf2"),
    OR_(fma, 7, "fpool3,frand3,fnear3"), OR_(fms, 7, "fpool3,frand3,fnear3"), OR_(fnms, 7, "fpool3,frand3,fnear3"),
    OR_(fceq, 3, F2D), OR_(fcgt, 3, F2D), OR_(fcmeq, 3, F2D), OR_(fcmgt, 3, F2D),
    OR_(fi, 3, "fi"), OR_(frest, 1, "w32"), OR_(frsqest, 1, "w32"),
    /* double precision */
    O(dfa, 3, D2D), O(dfs, 3, D2D), O(dfm, 3, D2D),
    O(dfma, 7, "dpool3,drand3,dnear3"), O(dfms, 7, "dpool3,drand3,dnear3"), O(dfnms, 7, "dpool3,drand3,dnear3"), O(dfnma, 7, "dpool3,drand3,dnear3"),
    O(frds, 1, "dfrds,dpool2,drand2"), O(fesd, 1, "w32e"),
};
#define NBASE ((int)(sizeof g_ops / sizeof g_ops[0]))

/* conversions: scale i8 is part of the op name ("cflts@173"); built at startup */
static const int g_scales[] = { 0, 1, 2, 126, 127, 128, 154, 155, 156, 157, 173, 255 };
#define NSCALE ((int)(sizeof g_scales / sizeof g_scales[0]))
static OP g_cvt[4 * 12];
static char g_cvt_name[4 * 12][16];
static int g_ncvt;
static void build_cvt(void) {
    static const char* nm[4] = { "cflts", "cfltu", "csflt", "cuflt" };
    V4 (*fn[4])(const G*) = { f_cflts, f_cfltu, f_csflt, f_cuflt };
    V4 (*rf[4])(const G*) = { r_cflts, r_cfltu, r_csflt, r_cuflt };
    for (int o = 0; o < 4; o++) for (int s = 0; s < NSCALE; s++) {
        OP* p = &g_cvt[g_ncvt]; snprintf(g_cvt_name[g_ncvt], 16, "%s@%d", nm[o], g_scales[s]);
        p->name = g_cvt_name[g_ncvt]; p->fn = fn[o]; p->ref = rf[o]; p->doms = "w32"; p->has_imm = 1; p->imm = g_scales[s];
        p->need = 1; p->heavy = !(g_scales[s] == (o < 2 ? 173 : 155)); g_ncvt++;
    }
}
static const OP* op_at(int k) { return k < NBASE ? &g_ops[k] : &g_cvt[k - NBASE]; }

/* ---------------------------------------------------------------- part 2: driver */
#ifdef EXH_MUTANT   /* harness self-test: plant single-point bugs a SIMD port could have, to prove the sweep finds them */
static void mutate(const OP* op, const G* g, V4* o) {
    if (!strcmp(op->name, "cgth")) {                       /* signed compare of exactly 0x8000 > 0x7FFF reported true in lane 0 */
        if ((g->a.w[0] >> 16) == 0x8000u && (g->b.w[0] >> 16) == 0x7FFFu) o->w[0] ^= 0xFFFF0000u;
    } else if (!strcmp(op->name, "frest")) {               /* one input out of 2^32 */
        for (int l = 0; l < 4; l++) if (g->a.w[l] == 0x7F7FFFFFu) o->w[l] ^= 1u;
    } else if (!strcmp(op->name, "fma") || !strcmp(op->name, "fm")) {   /* exact-boundary rounding slip on one rare class */
        for (int l = 0; l < 4; l++) if (g->a.w[l] == 0x00800000u && (g->b.w[l] & 0x7FFFFFFFu) == 0x7F7FFFFFu) o->w[l] += 1u;
    }
}
#define EXH_MUT(op, g, o) mutate(op, g, o)
#else
#define EXH_MUT(op, g, o) ((void)0)
#endif
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int name_match(const char* pat, const char* s) {            /* comma list, trailing * = prefix */
    char buf[2048], *sp; strncpy(buf, pat, sizeof buf - 1); buf[sizeof buf - 1] = 0;
    for (char* t = strtok_r(buf, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
        size_t n = strlen(t);
        if (n && t[n - 1] == '*') { if (!strncmp(t, s, n - 1)) return 1; }
        else if (!strcmp(t, s)) return 1;
    }
    return 0;
}
static uint64_t g_dump_n = 0;       /* --dump-n N: only N calls of the block (from --dump-start) */
static uint64_t g_dump_start = 0;   /* --dump-start S: first call of the dump, counted from the start of the block */
static uint64_t g_dump_hash = 0;    /* --dump-hash C: instead of the calls, one line "start count hash16" per chunk of C calls: the
                                     * block hash (exh_fold, the sweep's) restricted to the chunk, start counted from the block start */
/* --dump accepts a conversion at any I8 scale ("cflts@45": the instruction takes 0..255), not only the sweep's twelve. */
static const OP* dump_op(const char* name, int nops) {
    for (int k = 0; k < nops; k++) if (!strcmp(op_at(k)->name, name)) return op_at(k);
    static OP sy; static char nb[16];
    const char* at = strchr(name, '@'); char* e;
    if (!at || at - name > 5 || at[1] < '0' || at[1] > '9' || strlen(name) >= sizeof nb) return NULL;
    const long s = strtol(at + 1, &e, 10);
    if (*e || s > 255) return NULL;
    for (int k = 0; k < g_ncvt; k++) if (!strncmp(g_cvt[k].name, name, (size_t)(at - name) + 1)) {
        sy = g_cvt[k]; snprintf(nb, sizeof nb, "%s", name); sy.name = nb; sy.imm = (int)s; return &sy;
    }
    return NULL;
}
static int dump_block(const OP* op, const exh_dom* d, uint64_t blk) {
    const uint64_t blo = blk << EXH_BLOCK_LOG2; uint64_t hi = blo + EXH_BLOCK; if (hi > d->ncalls) hi = d->ncalls;
    const uint64_t lo = blo + g_dump_start;
    if (lo >= hi) { fprintf(stderr, "dump: start %llu is beyond the block (%llu calls)\n", (unsigned long long)g_dump_start, (unsigned long long)(hi - blo)); return -1; }
    if (g_dump_n && lo + g_dump_n < hi) hi = lo + g_dump_n;
    if (g_dump_hash) {
        for (uint64_t c = lo; c < hi; c += g_dump_hash) {
            const uint64_t e = hi - c < g_dump_hash ? hi : c + g_dump_hash; uint64_t h = EXH_FNV_OFFSET;
            for (uint64_t i = c; i < e; i++) {
                G g; d->gen(d, i, &g, (uint32_t)op->need); if (op->has_imm) g.imm = op->imm;
                V4 o = op->fn(&g); EXH_MUT(op, &g, &o); h = exh_fold(h, o);
            }
            printf("%llu %llu %016llx\n", (unsigned long long)(c - blo), (unsigned long long)(e - c), (unsigned long long)h);
        }
        return 0;
    }
    for (uint64_t i = lo; i < hi; i++) {
        G g; d->gen(d, i, &g, (uint32_t)op->need); if (op->has_imm) g.imm = op->imm;
        V4 o = op->fn(&g); EXH_MUT(op, &g, &o); const V4 r = op->ref ? op->ref(&g) : o;
        printf("%llu", (unsigned long long)i);
        const V4* in[3] = { &g.a, &g.b, &g.c };
        for (int s = 0; s < 3; s++) printf(" %08x%08x%08x%08x", in[s]->w[0], in[s]->w[1], in[s]->w[2], in[s]->w[3]);
        printf(" %d %08x%08x%08x%08x %08x%08x%08x%08x\n", g.imm, o.w[0], o.w[1], o.w[2], o.w[3], r.w[0], r.w[1], r.w[2], r.w[3]);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char *outp = NULL, *tag = "unknown", *opf = NULL, *domf = NULL; int shk = 0, shn = 1, refcheck = 0, rz = 0, list = 0;
    int stride_heavy = 1, stride_all = 1; const char* dump[3] = { NULL, NULL, NULL };
    char sdom_name[32][24]; int sdom_val[32], nsdom = 0;      /* --stride-dom NAME=S : per-domain block stride (overrides --stride-all) */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) outp = argv[++i];
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) tag = argv[++i];
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) opf = argv[++i];
        else if (!strcmp(argv[i], "--doms") && i + 1 < argc) domf = argv[++i];
        else if (!strcmp(argv[i], "--shard") && i + 1 < argc) { if (sscanf(argv[++i], "%d/%d", &shk, &shn) != 2 || shn < 1 || shk < 0 || shk >= shn) { fprintf(stderr, "bad --shard K/N\n"); return 2; } }
        else if (!strcmp(argv[i], "--refcheck")) refcheck = 1;
        else if (!strcmp(argv[i], "--stride-heavy") && i + 1 < argc) stride_heavy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride-all") && i + 1 < argc) stride_all = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stride-dom") && i + 1 < argc && nsdom < 32) { char* e = strchr(argv[++i], '='); if (!e) return 2; *e = 0; snprintf(sdom_name[nsdom], 24, "%s", argv[i]); sdom_val[nsdom++] = atoi(e + 1); }
        else if (!strcmp(argv[i], "--fpcr-rz")) rz = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--dump-n") && i + 1 < argc) g_dump_n = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dump-start") && i + 1 < argc) g_dump_start = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--dump-hash") && i + 1 < argc) { g_dump_hash = strtoull(argv[++i], NULL, 10); if (!g_dump_hash) { fprintf(stderr, "--dump-hash needs a chunk size > 0\n"); return 2; } }
        else if (!strcmp(argv[i], "--dump") && i + 3 < argc) { dump[0] = argv[++i]; dump[1] = argv[++i]; dump[2] = argv[++i]; }
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (stride_heavy < 1) stride_heavy = 1;
    if (stride_all < 1) stride_all = 1;
    exh_init(); build_cvt();
    const int nops = NBASE + g_ncvt;
    if (rz) {
#if defined(__aarch64__)
        __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)0x01C00000ull));
#else
        fprintf(stderr, "--fpcr-rz is aarch64 only\n"); return 2;
#endif
    }
    if (dump[0]) {
        const OP* op = dump_op(dump[0], nops);
        exh_dom* d = exh_find_dom(dump[1]);
        if (!op || !d) { fprintf(stderr, "dump: unknown op/domain\n"); return 2; }
        return dump_block(op, d, strtoull(dump[2], NULL, 10)) < 0 ? 2 : 0;
    }
    FILE* out = outp ? fopen(outp, "w") : stdout; if (!out) { perror("out"); return 2; }
    if (list) {
        for (int k = 0; k < nops; k++) { char b[512], *sp; strcpy(b, op_at(k)->doms); for (char* t = strtok_r(b, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) { exh_dom* d = exh_find_dom(t);
            fprintf(out, "%s\t%s\t%llu\t%llu\n", op_at(k)->name, t, (unsigned long long)d->ncalls, (unsigned long long)exh_nblocks(d)); } }
        return 0;
    }
    fprintf(out, "# spu_exh v1 tag=%s block=2^%d refcheck=%d fpcr_rz=%d stride_heavy=%d stride_all=%d shard=%d/%d seed=%016llx\n",
            tag, EXH_BLOCK_LOG2, refcheck, rz, stride_heavy, stride_all, shk, shn, (unsigned long long)EXH_SEED);
    double t_all = now();
    /* domain-major: generate each call once and apply every selected op that uses the domain */
    for (int di = 0; di < EXH_NDOMS; di++) {
        const exh_dom* d = &exh_doms[di];
        if (domf && !name_match(domf, d->name)) continue;
        int idx[512], n = 0;
        for (int k = 0; k < nops && n < 512; k++) {
            if (k % shn != shk) continue;
            const OP* op = op_at(k); if (opf && !name_match(opf, op->name)) continue;
            char b[512], *sp; strcpy(b, op->doms);
            for (char* t = strtok_r(b, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) if (!strcmp(t, d->name)) { idx[n++] = k; break; }
        }
        if (!n) continue;
        const uint64_t nb = exh_nblocks(d);
        uint64_t nrun[512], nref[512], firstref[512]; int stride[512];
        for (int j = 0; j < n; j++) { const OP* op = op_at(idx[j]); nrun[j] = nref[j] = firstref[j] = 0;
            int sa = stride_all; for (int q = 0; q < nsdom; q++) if (!strcmp(sdom_name[q], d->name)) sa = sdom_val[q];
            stride[j] = op->heavy ? (stride_heavy > sa ? stride_heavy : sa) : sa; if (stride[j] < 1) stride[j] = 1; }
        const double t0 = now();
        for (uint64_t blk = 0; blk < nb; blk++) {
            int act[512], na = 0; uint32_t need = 0; uint64_t h[512];
            for (int j = 0; j < n; j++) if (!(blk % (uint64_t)stride[j])) { act[na++] = j; h[j] = EXH_FNV_OFFSET; need |= (uint32_t)op_at(idx[j])->need; }
            if (!na) continue;
            if (refcheck) need |= 7u;
            const uint64_t lo = blk << EXH_BLOCK_LOG2; uint64_t hi = lo + EXH_BLOCK; if (hi > d->ncalls) hi = d->ncalls;
            for (uint64_t i = lo; i < hi; i++) {
                G g; d->gen(d, i, &g, need); const int32_t dimm = g.imm;
                for (int q = 0; q < na; q++) {
                    const int j = act[q]; const OP* op = op_at(idx[j]);
                    g.imm = op->has_imm ? op->imm : dimm;
                    V4 o = op->fn(&g); EXH_MUT(op, &g, &o); h[j] = exh_fold(h[j], o);
                    if (refcheck && op->ref && !op->heavy) { const V4 r = op->ref(&g); if (memcmp(&o, &r, sizeof o)) { if (!nref[j]) firstref[j] = i; nref[j]++; } }
                }
            }
            for (int q = 0; q < na; q++) { const int j = act[q]; nrun[j]++;
                fprintf(out, "%s\t%s\t%llu\t%016llx\n", op_at(idx[j])->name, d->name, (unsigned long long)blk, (unsigned long long)h[j]); }
            fflush(out);
        }
        const double dt = now() - t0;
        for (int j = 0; j < n; j++) fprintf(out, "#cov\t%s\t%s\t%llu\t%llu\t%d\t%.2f\t%llu\t%llu\n", op_at(idx[j])->name, d->name,
                (unsigned long long)nrun[j], (unsigned long long)nb, stride[j], dt / n, (unsigned long long)nref[j], (unsigned long long)firstref[j]);
        fprintf(out, "#dom\t%s\tops=%d\tseconds=%.1f\n", d->name, n, dt);
        fflush(out);
    }
    fprintf(out, "# done total_seconds=%.1f\n", now() - t_all);
    if (outp) fclose(out);
    return 0;
}
