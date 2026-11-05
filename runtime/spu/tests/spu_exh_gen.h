/*
 * spu_exh_gen.h -- portable input generator, domains and block hash for test_spu_exhaustive.
 *
 * PORTABILITY CONTRACT: this header is plain C11 on <stdint.h> only. It contains no SPU helper,
 * no SIMD, no floating-point operation and no host-endian assumption, so the same source can be
 * compiled for the real SPU (spu-gcc) to produce hardware golden hashes. A port supplies only the
 * "adapter" layer (V4 <-> vec_uint4, one function per op) and a driver; see test_spu_exhaustive.c.
 *
 * Vocabulary
 *   V4       four SPU words as VALUES: w[0] is the preferred slot / most significant word. SPU byte P
 *            (big-endian, 0..15) is bits [8*(3-(P&3)) +: 8] of w[P>>2]. Halfword lane H (0..7) is bits
 *            [16*(1-(H&1)) +: 16] of w[H>>1].
 *   call     one invocation of an op on one (a, b, c, imm) tuple. "Lane packing": every call carries
 *            4/8/16 independent lane inputs, so one call exercises many (a,b) lane pairs.
 *   domain   a named, deterministic family of calls 0 .. ncalls-1; gen(dom, i) is a pure function of
 *            (domain name, i). No state: any call (hence any block) can be regenerated alone.
 *   block    2^20 consecutive calls of one (op, domain); the unit of hashing and of re-running.
 *
 * PRNG: splitmix64 (Vigna), used counter-style: exh_sm(seed, k) is the k-th output of the generator
 * seeded with `seed`. Domain seed = mix(fnv64(domain name) ^ EXH_SEED).
 * Enumerations use exh_perm(), a bijection of [0, 2^bits) built from xorshift/odd-multiply rounds,
 * so an "exhaustive" domain visits every point exactly once, spread pseudo-randomly over lane
 * positions (no lane index correlates with the value).
 *
 * Block hash: FNV-1a-64 applied to 64-bit words (NOT per byte, for speed): h = FNV_OFFSET; for each
 * call output V4 {w0,w1,w2,w3}: h = (h ^ (w0<<32|w1)) * FNV_PRIME; h = (h ^ (w2<<32|w3)) * FNV_PRIME.
 * Each step is a bijection of h, so a single differing output can never be masked by itself.
 */
#ifndef SPU_EXH_GEN_H
#define SPU_EXH_GEN_H

#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t w[4]; } V4;
typedef struct { V4 a, b, c; int32_t imm; } G;

#define EXH_SEED          0x5350555F45584831ULL   /* "SPU_EXH1" */
#define EXH_BLOCK_LOG2    20
#define EXH_BLOCK         (1ULL << EXH_BLOCK_LOG2)
#define EXH_FNV_OFFSET    0xcbf29ce484222325ULL
#define EXH_FNV_PRIME     0x100000001b3ULL

static inline uint64_t exh_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static inline uint64_t exh_sm(uint64_t seed, uint64_t k) { return exh_mix(seed + (k + 1) * 0x9E3779B97F4A7C15ULL); }
static inline uint64_t exh_fold(uint64_t h, V4 v) {
    h = (h ^ (((uint64_t)v.w[0] << 32) | v.w[1])) * EXH_FNV_PRIME;
    h = (h ^ (((uint64_t)v.w[2] << 32) | v.w[3])) * EXH_FNV_PRIME;
    return h;
}
static inline uint64_t exh_perm(uint64_t x, int bits) {          /* bijection on [0, 2^bits), 1 <= bits <= 32 */
    const uint64_t m = (1ULL << bits) - 1; const int s = (bits + 1) / 2;
    x ^= x >> s; x = (x * 0x9E3779B97F4A7C15ULL) & m;
    x ^= x >> s; x = (x * 0xD6E8FEB86659FD93ULL) & m;
    x ^= x >> s; return x;
}

/* ---- lane accessors in SPU (big-endian) lane order ---- */
static inline void exh_put8(V4* v, int l, uint32_t x) {
    const int sh = 8 * (3 - (l & 3)); v->w[l >> 2] = (v->w[l >> 2] & ~(0xFFu << sh)) | ((x & 0xFFu) << sh);
}
static inline void exh_put16(V4* v, int l, uint32_t x) {
    const int sh = 16 * (1 - (l & 1)); v->w[l >> 1] = (v->w[l >> 1] & ~(0xFFFFu << sh)) | ((x & 0xFFFFu) << sh);
}
static inline uint32_t exh_get8(V4 v, int l) { return (v.w[l >> 2] >> (8 * (3 - (l & 3)))) & 0xFFu; }

struct exh_dom;
typedef void (*exh_genfn)(const struct exh_dom*, uint64_t, G*, uint32_t need);
typedef struct exh_dom {
    const char* name;
    uint64_t ncalls;
    exh_genfn gen;
    uint32_t p0;       /* generator parameter (lane mask of w32 domains) */
    uint64_t seed;     /* filled by exh_init */
} exh_dom;

/* ---- random words ---- */
static inline uint64_t exh_r64(const exh_dom* d, uint64_t i, int j) { return exh_sm(d->seed, i * 16 + (uint64_t)j); }

static const uint32_t exh_H16[16] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0xFFFE, 0x00FF, 0xAAAA, 0x5555, 0x0101 };
/* 1024 structured edge words: powers of two, their complements, low/high masks, +-1 neighbours, small
 * signed values, byte-replicated, half-word edge combinations, a fixed pseudo-random remainder. */
static inline uint32_t exh_edge(uint32_t idx) {
    idx &= 1023u;
    const uint32_t k = idx & 31u;
    if (idx < 32)  return 1u << k;
    if (idx < 64)  return ~(1u << k);
    if (idx < 96)  return k == 31 ? 0xFFFFFFFFu : ((1u << (k + 1)) - 1u);
    if (idx < 128) return ~(k == 31 ? 0xFFFFFFFFu : ((1u << (k + 1)) - 1u));
    if (idx < 160) return (1u << k) + 1u;
    if (idx < 192) return (1u << k) - 1u;
    if (idx < 224) return 0u - (idx - 192u);
    if (idx < 256) return idx - 224u;
    if (idx < 512) return (idx - 256u) * 0x01010101u;
    if (idx < 768) { const uint32_t q = idx - 512u; return (exh_H16[q >> 4] << 16) | exh_H16[q & 15]; }
    return (uint32_t)exh_mix(0xED6E000000000000ULL + idx);
}
/* a word from 64 random bits: uniform, edge, sparse, dense or replicated, so random domains still reach corners */
static inline uint32_t exh_mixw(uint64_t r) {
    const uint32_t t = (uint32_t)r & 15u, u = (uint32_t)(r >> 32), v = (uint32_t)r >> 4;
    if (t < 6)  return u;
    if (t < 10) return exh_edge(v);
    if (t < 12) return u & ((u << 11) | (u >> 21)) & (v | (v << 16));
    if (t < 14) return u | ((u << 7) | (u >> 25)) | (v & (v << 9));
    if (t == 14) return (u & 0xFFu) * 0x01010101u;
    return (u & 0xFFFFu) * 0x00010001u;
}
static inline V4 exh_rmix4(const exh_dom* d, uint64_t i, int j0) {            /* uses u64 slots j0 .. j0+3 (of 16) */
    V4 v; for (int k = 0; k < 4; k++) v.w[k] = exh_mixw(exh_r64(d, i, j0 + k)); return v;
}
static inline V4 exh_runi4(const exh_dom* d, uint64_t i, int j0) {            /* uses u64 slots j0 .. j0+1 */
    const uint64_t x = exh_r64(d, i, j0), y = exh_r64(d, i, j0 + 1);
    V4 v; v.w[0] = (uint32_t)(x >> 32); v.w[1] = (uint32_t)x; v.w[2] = (uint32_t)(y >> 32); v.w[3] = (uint32_t)y; return v;
}
static inline V4 exh_z4(void) { V4 v; v.w[0] = v.w[1] = v.w[2] = v.w[3] = 0; return v; }
static inline void exh_zero(G* g) { for (int k = 0; k < 4; k++) g->a.w[k] = g->b.w[k] = g->c.w[k] = 0; g->imm = 0; }

/* ======================================================================================
 * integer domains
 * ====================================================================================== */
static void gen_byte2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* all 2^16 (a,b) byte pairs, 16 lanes */
    exh_zero(g);
    for (int l = 0; l < 16; l++) { const uint32_t p = (uint32_t)exh_perm(i * 16 + (uint64_t)l, 16); exh_put8(&g->a, l, p >> 8); exh_put8(&g->b, l, p & 255u); }
    (void)d;
}
static void gen_half2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* all 2^32 (a,b) halfword pairs, 8 lanes */
    exh_zero(g);
    for (int l = 0; l < 8; l++) { const uint32_t p = (uint32_t)exh_perm(i * 8 + (uint64_t)l, 32); exh_put16(&g->a, l, p >> 16); exh_put16(&g->b, l, p & 0xFFFFu); }
    (void)d;
}
static void gen_wcnt(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* b lane = every 32-bit count exactly once; a random */
    g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) g->b.w[l] = (uint32_t)exh_perm(i * 4 + (uint64_t)l, 32);
}
static const uint16_t exh_hipat[8] = { 0x0000, 0xFFFF, 0x8000, 0x0001, 0x7FFF, 0x0100, 0x00FF, 0xFF00 };
static void gen_qcnt(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* count word: low 16 bits exhaustive x 256 high-16 patterns */
    const uint32_t lo = (uint32_t)(i & 0xFFFFu), j = (uint32_t)(i >> 16);
    const uint32_t hi = j < 8 ? exh_hipat[j] : (uint32_t)exh_sm(d->seed ^ 0x51ULL, j) & 0xFFFFu;   /* j < 256 */
    g->a = (need & 1u) ? exh_rmix4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = 0;
    if (d->p0) g->a.w[0] = (hi << 16) | lo; else g->b.w[0] = (hi << 16) | lo;   /* p0: count in operand a (unary ops) */
}
static void gen_wpool2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {           /* all pairs of the 1024 edge words */
    g->c = (need & 4u) ? exh_rmix4(d, i, 0) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) { const uint32_t p = (uint32_t)exh_perm(i * 4 + (uint64_t)l, 20); g->a.w[l] = exh_edge(p >> 10); g->b.w[l] = exh_edge(p & 1023u); }
}
static void gen_wrand(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* seeded random words (mixed density) */
    g->a = (need & 1u) ? exh_rmix4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = 0;
}
static void gen_w32(const exh_dom* d, uint64_t i, G* g, uint32_t need) { /* lanes in mask p0 enumerate all 2^32 words; others random */
    int nl = 0; for (int l = 0; l < 4; l++) nl += (int)((d->p0 >> l) & 1u);
    int k = 0;
    g->imm = 0;
    for (int l = 0; l < 4; l++) g->a.w[l] = ((d->p0 >> l) & 1u) ? (uint32_t)exh_perm(i * (uint64_t)nl + (uint64_t)(k++), 32) : exh_mixw(exh_r64(d, i, l));
    g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4();
}
static void gen_h16(const exh_dom* d, uint64_t i, G* g, uint32_t need) { /* all 2^32 (a16,b16) pairs of the 16-bit halves a word op reads; the other
                                                                            halves are random. p0: 0 = low x low, 1 = high x high, 2 = a high x b low */
    const uint64_t ra = exh_r64(d, i, 0), rb = exh_r64(d, i, 1);              /* 4 x 16 random bits for each operand's unread halves */
    g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint32_t p = (uint32_t)exh_perm(i * 4 + (uint64_t)l, 32), x = p >> 16, y = p & 0xFFFFu;
        const uint32_t oa = (uint32_t)(ra >> (16 * l)) & 0xFFFFu, ob = (uint32_t)(rb >> (16 * l)) & 0xFFFFu;
        if (d->p0 == 0)      { g->a.w[l] = (oa << 16) | x; g->b.w[l] = (ob << 16) | y; }
        else if (d->p0 == 1) { g->a.w[l] = (x << 16) | oa; g->b.w[l] = (y << 16) | ob; }
        else                 { g->a.w[l] = (x << 16) | oa; g->b.w[l] = (ob << 16) | y; }
    }
}
static void gen_bits16(const exh_dom* d, uint64_t i, G* g, uint32_t need) {           /* bit 0 of each of the 16 bytes = all 2^16 patterns */
    g->a = (need & 1u) ? exh_rmix4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 16; l++) exh_put8(&g->a, l, (exh_get8(g->a, l) & 0xFEu) | (uint32_t)((i >> l) & 1u));
}

/* immediates: i10 range [-1024,1023] plus 128 wide values */
#define EXH_NIMM 2176u
static inline int32_t exh_immv(uint32_t j) {
    static const uint32_t W[16] = { 0x7FFFFFFFu, 0x80000000u, 0xFFFFu, 0x10000u, 0x8000u, 0x7FFFu, 0xFFFF8000u, 0x3FFu,
                                    0x400u, 0xFFFFFBFFu, 0x200u, 0xFFFFFE00u, 0x1FFFFu, 0xFFFFFFFFu, 0x80u, 0xFFFFFF80u };
    if (j < 2048u) return (int32_t)j - 1024;
    j -= 2048u; if (j < 16u) return (int32_t)W[j];
    return (int32_t)(uint32_t)exh_mix(0x1A0000ULL + j);
}
static void gen_immb(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* every imm x every byte value (at rotating lane positions) */
    const uint32_t j = (uint32_t)(i >> 4), c = (uint32_t)(i & 15u);
    exh_zero(g);
    for (int l = 0; l < 16; l++) exh_put8(&g->a, l, c * 16u + (((uint32_t)l + j) & 15u));
    g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = exh_immv(j);
}
static void gen_immh(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* every imm x every halfword value */
    const uint32_t j = (uint32_t)(i >> 13), c = (uint32_t)(i & 8191u);
    exh_zero(g);
    for (int l = 0; l < 8; l++) exh_put16(&g->a, l, c * 8u + (((uint32_t)l + j) & 7u));
    g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = exh_immv(j);
}
static void gen_immw(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* every imm x 256 calls of random words */
    g->a = (need & 1u) ? exh_rmix4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_rmix4(d, i, 4) : exh_z4(); g->c = (need & 4u) ? exh_rmix4(d, i, 8) : exh_z4(); g->imm = exh_immv((uint32_t)(i >> 8));
}
static void gen_fsmbi(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* all 2^16 immediates (random junk above bit 15) */
    exh_zero(g); g->imm = (int32_t)((uint32_t)i | ((uint32_t)exh_r64(d, i, 0) & 0xFFFF0000u));
}
/* shufb: control byte at position p = v (all 256 values, all 16 positions) x 256 random fills of the rest */
static void gen_shufb_pos(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    const uint32_t p = (uint32_t)(i >> 16) & 15u, v = (uint32_t)(i >> 8) & 255u;
    g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_runi4(d, i, 2) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 4) : exh_z4(); g->imm = 0;
    exh_put8(&g->c, (int)p, v);
}
static void gen_shufb_rand(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_runi4(d, i, 2) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 4) : exh_z4(); g->imm = 0;
}
static void gen_shufb_all(const exh_dom* d, uint64_t i, G* g, uint32_t need) {        /* control byte P = v + P*delta: every value at every position at once */
    const uint32_t v = (uint32_t)(i & 255u), dl = (uint32_t)(i >> 8) & 255u;
    exh_zero(g); g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_runi4(d, i, 2) : exh_z4();
    for (int P = 0; P < 16; P++) exh_put8(&g->c, P, v + (uint32_t)P * dl);
}

/* ======================================================================================
 * single-precision float domains (values are raw 32-bit patterns; nothing here is float math)
 * ====================================================================================== */
static const uint32_t exh_FR8[8] = { 0, 1, 0x7FFFFF, 0x400000, 0x2AAAAA, 0x555555, 0x7FFFFE, 0x000FFF };
static const uint32_t exh_EXT8[8] = { 0, 1, 2, 3, 252, 253, 254, 255 };
static inline uint32_t exh_clamp(int32_t v, int32_t lo, int32_t hi) { return (uint32_t)(v < lo ? lo : v > hi ? hi : v); }
static inline uint32_t exh_fpv(uint32_t idx12) {        /* 256 exponents x 8 fraction classes x 2 signs */
    const uint32_t e = (idx12 >> 4) & 255u, v = idx12 & 15u;
    return ((v & 1u) << 31) | (e << 23) | exh_FR8[v >> 1];
}
static inline uint32_t exh_frac23(uint64_t r) {          /* special fraction 3/8 of the time, else random */
    return ((r >> 61) < 3u) ? exh_FR8[(r >> 56) & 7u] : (uint32_t)r & 0x7FFFFFu;
}
static void gen_fpool2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* all pairs of the 4096-value exponent-systematic pool */
    g->c = (need & 4u) ? exh_runi4(d, i, 0) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) { const uint32_t p = (uint32_t)exh_perm(i * 4 + (uint64_t)l, 24); g->a.w[l] = exh_fpv(p >> 12); g->b.w[l] = exh_fpv(p & 4095u); }
}
static void gen_frand2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_runi4(d, i, 2) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 4) : exh_z4(); g->imm = 0;
}
static void gen_fnear2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* exponents within +-40, equal / sign-flipped / adjacent pairs, extremes */
    g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint64_t r = exh_r64(d, i, 2 * l), r2 = exh_r64(d, i, 2 * l + 1);
        const uint32_t var = (uint32_t)r & 7u;
        uint32_t ea = (uint32_t)(r >> 3) & 255u;
        const int32_t dl = (int32_t)((r >> 14) % 81u) - 40;
        const uint32_t sa = (uint32_t)(r >> 11) & 1u, sb = (uint32_t)(r >> 12) & 1u;
        uint32_t eb;
        if (var == 3 || var == 7) ea = exh_EXT8[(r >> 3) & 7u];
        eb = exh_clamp((int32_t)ea + dl, 0, 255);
        if (var == 7) eb = exh_EXT8[(r >> 20) & 7u];
        const uint32_t a = (sa << 31) | (ea << 23) | exh_frac23(r2), b0 = (sb << 31) | (eb << 23) | exh_frac23(r2 << 7 | r2 >> 57);
        uint32_t b = b0;
        if (var == 4) b = a;
        else if (var == 5) b = a ^ 0x80000000u;
        else if (var == 6) b = ((r >> 40) & 1u) ? a + 1u : a - 1u;
        g->a.w[l] = a; g->b.w[l] = b;
    }
}
static void gen_fovf2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* product exponent (ea+eb-127) near 0 and 255: fm overflow / underflow */
    static const int32_t T[16] = { -3, -2, -1, 0, 1, 2, 3, 4, 251, 252, 253, 254, 255, 256, 257, 258 };
    g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint64_t r = exh_r64(d, i, 2 * l), r2 = exh_r64(d, i, 2 * l + 1);
        const int32_t t = T[r & 15u];
        const int32_t lo = (t + 127 - 255) > 0 ? (t + 127 - 255) : 0, hi = (t + 127) < 255 ? (t + 127) : 255;
        const int32_t ea = hi >= lo ? lo + (int32_t)((r >> 4) % (uint64_t)(hi - lo + 1)) : 0;
        const int32_t eb = t + 127 - ea;
        g->a.w[l] = (((uint32_t)(r >> 40) & 1u) << 31) | ((uint32_t)ea << 23) | exh_frac23(r2);
        g->b.w[l] = (((uint32_t)(r >> 41) & 1u) << 31) | ((uint32_t)exh_clamp(eb, 0, 255) << 23) | exh_frac23(r2 << 7 | r2 >> 57);
    }
}
static const uint32_t exh_KE16[16] = { 0, 1, 2, 64, 126, 127, 128, 129, 190, 252, 253, 254, 255, 100, 150, 30 };
static const uint32_t exh_FR4[4] = { 0, 1, 0x7FFFFF, 0x400000 };
static void gen_fpool3(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* x,y: 16 key exps x 4 fractions x 2 signs; z: product exponent +-32 x 16 variants */
    (void)d; g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint32_t t = (uint32_t)exh_perm(i * 4 + (uint64_t)l, 24), xi = t & 127u, yi = (t >> 7) & 127u, zi = t >> 14;
        const uint32_t ex = exh_KE16[xi >> 3], ey = exh_KE16[yi >> 3];
        g->a.w[l] = ((xi & 1u) << 31) | (ex << 23) | exh_FR4[(xi >> 1) & 3u];
        g->b.w[l] = ((yi & 1u) << 31) | (ey << 23) | exh_FR4[(yi >> 1) & 3u];
        const int32_t dz = (int32_t)(zi >> 4) - 32;
        const uint32_t ez = exh_clamp((int32_t)ex + (int32_t)ey - 127 + dz, 0, 255), v = zi & 15u;
        g->c.w[l] = ((v & 1u) << 31) | (ez << 23) | exh_FR8[v >> 1];
    }
}
static void gen_frand3(const exh_dom* d, uint64_t i, G* g, uint32_t need) { gen_frand2(d, i, g, need); }
static void gen_fnear3(const exh_dom* d, uint64_t i, G* g, uint32_t need) {            /* addend exponent within +-40 of the product exponent */
    g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint64_t r = exh_r64(d, i, 3 * l), r2 = exh_r64(d, i, 3 * l + 1), r3 = exh_r64(d, i, 3 * l + 2);
        uint32_t ex = (uint32_t)r & 255u, ey = (uint32_t)(r >> 8) & 255u;
        if (((r >> 16) & 7u) == 0) ex = exh_EXT8[(r >> 20) & 7u];
        if (((r >> 24) & 7u) == 0) ey = exh_EXT8[(r >> 28) & 7u];
        const int32_t dz = (int32_t)((r >> 32) % 81u) - 40;
        const uint32_t ez = exh_clamp((int32_t)ex + (int32_t)ey - 127 + dz, 0, 255);
        g->a.w[l] = (((uint32_t)(r >> 50) & 1u) << 31) | (ex << 23) | exh_frac23(r2);
        g->b.w[l] = (((uint32_t)(r >> 51) & 1u) << 31) | (ey << 23) | exh_frac23(r2 << 7 | r2 >> 57);
        g->c.w[l] = (((uint32_t)(r >> 52) & 1u) << 31) | (ez << 23) | exh_frac23(r3);
    }
}
static void gen_fi(const exh_dom* d, uint64_t i, G* g, uint32_t need) {                /* ra: Y in the low 19 bits; rb: an estimate word (sign,exp,base13,step10) */
    static const uint32_t EXI[9] = { 0, 1, 2, 3, 126, 127, 128, 254, 255 };
    g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 4; l++) {
        const uint64_t r = exh_r64(d, i, 2 * l), r2 = exh_r64(d, i, 2 * l + 1);
        const uint32_t sign = (uint32_t)r & 1u;
        const uint32_t exp = (((r >> 1) & 3u) == 0) ? (uint32_t)(r >> 3) & 255u : EXI[(r >> 3) % 9u];
        const uint32_t base = (((r >> 12) & 3u) == 0) ? (((r >> 14) & 1u) ? 0x1FFFu : 0u) : (uint32_t)(r >> 15) & 0x1FFFu;
        const uint32_t step = (((r >> 29) & 3u) == 0) ? (((r >> 31) & 1u) ? 0x3FFu : 0u) : (uint32_t)(r >> 32) & 0x3FFu;
        g->b.w[l] = (sign << 31) | (exp << 23) | (base << 10) | step;
        const uint32_t low = ((r2 & 3u) == 0) ? (((r2 >> 2) & 1u) ? 0x7FFFFu : 0u) : (uint32_t)(r2 >> 8) & 0x7FFFFu;
        g->a.w[l] = ((uint32_t)(r2 >> 32) & 0xFFF80000u) | low;
    }
}

/* ======================================================================================
 * double-precision domains: one lane = a doubleword = two words (hi, lo)
 * ====================================================================================== */
static const uint64_t exh_DF4[4] = { 0, 1, 0xFFFFFFFFFFFFFULL, 0x8000000000000ULL };
static inline uint32_t exh_dpe128(uint32_t k) { return k < 8 ? k : k < 16 ? 2047u - (k - 8u) : 967u + (k - 16u); }
static inline uint64_t exh_dpv(uint32_t idx10) {
    const uint32_t v = idx10 & 7u;
    return ((uint64_t)(v & 1u) << 63) | ((uint64_t)exh_dpe128((idx10 >> 3) & 127u) << 52) | exh_DF4[v >> 1];
}
static const uint32_t exh_DE32[32] = { 0, 1, 2, 511, 512, 1000, 1010, 1019, 1020, 1021, 1022, 1023, 1024, 1025, 1026, 1027,
                                       1028, 1040, 1050, 1069, 1070, 1071, 1072, 1073, 1074, 1075, 1076, 1077, 1078, 2045, 2046, 2047 };
static inline uint64_t exh_dp256(uint32_t idx8) {
    const uint32_t v = idx8 & 7u;
    return ((uint64_t)(v & 1u) << 63) | ((uint64_t)exh_DE32[(idx8 >> 3) & 31u] << 52) | exh_DF4[v >> 1];
}
static inline void exh_putd(V4* v, int l, uint64_t x) { v->w[2 * l] = (uint32_t)(x >> 32); v->w[2 * l + 1] = (uint32_t)x; }
static inline uint64_t exh_frac52(uint64_t r) { return ((r >> 61) < 3u) ? exh_DF4[(r >> 58) & 3u] : (r & 0xFFFFFFFFFFFFFULL); }
static void gen_dpool2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->c = (need & 4u) ? exh_runi4(d, i, 0) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 2; l++) { const uint32_t p = (uint32_t)exh_perm(i * 2 + (uint64_t)l, 20); exh_putd(&g->a, l, exh_dpv(p >> 10)); exh_putd(&g->b, l, exh_dpv(p & 1023u)); }
}
static void gen_drand2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->a = (need & 1u) ? exh_runi4(d, i, 0) : exh_z4(); g->b = (need & 2u) ? exh_runi4(d, i, 2) : exh_z4(); g->c = (need & 4u) ? exh_runi4(d, i, 4) : exh_z4(); g->imm = 0;
}
static const uint32_t exh_DEX[12] = { 0, 1, 2, 3, 1022, 1023, 1024, 1075, 2044, 2045, 2046, 2047 };
static void gen_dnear2(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->c = (need & 4u) ? exh_runi4(d, i, 8) : exh_z4(); g->imm = 0;
    for (int l = 0; l < 2; l++) {
        const uint64_t r = exh_r64(d, i, 3 * l), r2 = exh_r64(d, i, 3 * l + 1), r3 = exh_r64(d, i, 3 * l + 2);
        const uint32_t var = (uint32_t)r & 7u;
        uint32_t ea = (uint32_t)(r >> 3) & 2047u;
        const int32_t dl = (int32_t)((r >> 16) % 141u) - 70;
        if (var == 3 || var == 7) ea = exh_DEX[(r >> 3) % 12u];
        uint32_t eb = exh_clamp((int32_t)ea + dl, 0, 2047);
        if (var == 7) eb = exh_DEX[(r >> 24) % 12u];
        const uint64_t a = ((uint64_t)((r >> 12) & 1u) << 63) | ((uint64_t)ea << 52) | exh_frac52(r2);
        uint64_t b = ((uint64_t)((r >> 13) & 1u) << 63) | ((uint64_t)eb << 52) | exh_frac52(r3);
        if (var == 4) b = a;
        else if (var == 5) b = a ^ 0x8000000000000000ULL;
        else if (var == 6) b = ((r >> 40) & 1u) ? a + 1u : a - 1u;
        exh_putd(&g->a, l, a); exh_putd(&g->b, l, b);
    }
}
static void gen_dpool3(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    (void)d; exh_zero(g);
    for (int l = 0; l < 2; l++) {
        const uint32_t t = (uint32_t)exh_perm(i * 2 + (uint64_t)l, 24);
        exh_putd(&g->a, l, exh_dp256(t & 255u)); exh_putd(&g->b, l, exh_dp256((t >> 8) & 255u)); exh_putd(&g->c, l, exh_dp256(t >> 16));
    }
}
static void gen_drand3(const exh_dom* d, uint64_t i, G* g, uint32_t need) { gen_drand2(d, i, g, need); }
static void gen_dnear3(const exh_dom* d, uint64_t i, G* g, uint32_t need) {
    g->imm = 0;
    for (int l = 0; l < 2; l++) {
        const uint64_t r = exh_r64(d, i, 4 * l), r2 = exh_r64(d, i, 4 * l + 1), r3 = exh_r64(d, i, 4 * l + 2), r4 = exh_r64(d, i, 4 * l + 3);
        uint32_t ex = (uint32_t)r & 2047u, ey = (uint32_t)(r >> 11) & 2047u;
        if (((r >> 22) & 7u) == 0) ex = exh_DEX[(r >> 25) % 12u];
        if (((r >> 29) & 7u) == 0) ey = exh_DEX[(r >> 32) % 12u];
        const int32_t dz = (int32_t)((r >> 40) % 141u) - 70;
        const uint32_t ez = exh_clamp((int32_t)ex + (int32_t)ey - 1023 + dz, 0, 2047);
        exh_putd(&g->a, l, ((uint64_t)((r >> 60) & 1u) << 63) | ((uint64_t)ex << 52) | exh_frac52(r2));
        exh_putd(&g->b, l, ((uint64_t)((r >> 61) & 1u) << 63) | ((uint64_t)ey << 52) | exh_frac52(r3));
        exh_putd(&g->c, l, ((uint64_t)((r >> 62) & 1u) << 63) | ((uint64_t)ez << 52) | exh_frac52(r4));
    }
}
static void gen_dfrds(const exh_dom* d, uint64_t i, G* g, uint32_t need) {             /* every exponent x sign x rounding-tie pattern x fraction highs */
    static const uint32_t TIE[8] = { 0, 1, 0x0FFFFFFFu, 0x10000000u, 0x10000001u, 0x1FFFFFFFu, 0x0FFFFFFEu, 0x08000000u };
    (void)d; exh_zero(g);
    for (int l = 0; l < 2; l++) {
        const uint32_t p = (uint32_t)exh_perm(i * 2 + (uint64_t)l, 20);
        const uint64_t e = p >> 9, s = (p >> 8) & 1u; const uint32_t tie = (p >> 5) & 7u, h5 = p & 31u;
        const uint32_t hi23 = h5 == 0 ? 0u : h5 == 31 ? 0x7FFFFFu : ((uint32_t)exh_mix(h5 * 77ull + tie) & 0x7FFFFFu);
        exh_putd(&g->a, l, (s << 63) | (e << 52) | ((uint64_t)hi23 << 29) | TIE[tie]);
    }
}
static void gen_dtsv(const exh_dom* d, uint64_t i, G* g, uint32_t need) {              /* every I7 x every pair of special-value classes */
    static const uint64_t CLS[16] = { 0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0x800FFFFFFFFFFFFFULL,
        0x0010000000000000ULL, 0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
        0x7FF8000000000000ULL, 0x7FF0000000000001ULL, 0xFFF8000000000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0x7FEFFFFFFFFFFFFFULL };
    exh_zero(g);
    exh_putd(&g->a, 0, CLS[i & 15u]); exh_putd(&g->a, 1, CLS[(i >> 4) & 15u]);
    g->imm = (int32_t)(((uint32_t)(i >> 8) & 0x7Fu) | ((uint32_t)exh_r64(d, i, 0) & 0xFFFFFF80u));
}

/* ======================================================================================
 * domain table
 * ====================================================================================== */
#define EXH_D(n, calls, fn, p) { n, (uint64_t)(calls), fn, p, 0 }
static exh_dom exh_doms[] = {
    EXH_D("byte2",      1ULL << 12, gen_byte2,   0),
    EXH_D("half2",      1ULL << 29, gen_half2,   0),
    EXH_D("wcnt",       1ULL << 30, gen_wcnt,    0),
    EXH_D("qcnt",       1ULL << 24, gen_qcnt,    0),
    EXH_D("qcnta",      1ULL << 24, gen_qcnt,    1),
    EXH_D("h16ll",      1ULL << 30, gen_h16,     0),
    EXH_D("h16hh",      1ULL << 30, gen_h16,     1),
    EXH_D("h16hl",      1ULL << 30, gen_h16,     2),
    EXH_D("wpool2",     1ULL << 18, gen_wpool2,  0),
    EXH_D("wrand",      1ULL << 22, gen_wrand,   0),
    EXH_D("w32",        1ULL << 30, gen_w32,     0xF),
    EXH_D("w32e",       1ULL << 31, gen_w32,     0x5),
    EXH_D("w32o",       1ULL << 31, gen_w32,     0xA),
    EXH_D("bits16",     1ULL << 16, gen_bits16,  0),
    EXH_D("immb",       (uint64_t)EXH_NIMM * 16, gen_immb, 0),
    EXH_D("immh",       (uint64_t)EXH_NIMM * 8192, gen_immh, 0),
    EXH_D("immw",       (uint64_t)EXH_NIMM * 256, gen_immw, 0),
    EXH_D("fsmbi",      1ULL << 16, gen_fsmbi,   0),
    EXH_D("shufb_pos",  1ULL << 20, gen_shufb_pos, 0),
    EXH_D("shufb_rand", 1ULL << 22, gen_shufb_rand, 0),
    EXH_D("shufb_all",  1ULL << 16, gen_shufb_all, 0),
    EXH_D("fpool2",     1ULL << 22, gen_fpool2,  0),
    EXH_D("frand2",     1ULL << 26, gen_frand2,  0),
    EXH_D("fnear2",     1ULL << 24, gen_fnear2,  0),
    EXH_D("fovf2",      1ULL << 22, gen_fovf2,   0),
    EXH_D("fpool3",     1ULL << 22, gen_fpool3,  0),
    EXH_D("frand3",     1ULL << 25, gen_frand3,  0),
    EXH_D("fnear3",     1ULL << 24, gen_fnear3,  0),
    EXH_D("fi",         1ULL << 25, gen_fi,      0),
    EXH_D("dpool2",     1ULL << 19, gen_dpool2,  0),
    EXH_D("drand2",     1ULL << 22, gen_drand2,  0),
    EXH_D("dnear2",     1ULL << 22, gen_dnear2,  0),
    EXH_D("dpool3",     1ULL << 23, gen_dpool3,  0),
    EXH_D("drand3",     1ULL << 21, gen_drand3,  0),
    EXH_D("dnear3",     1ULL << 21, gen_dnear3,  0),
    EXH_D("dfrds",      1ULL << 19, gen_dfrds,   0),
    EXH_D("dtsv",       1ULL << 15, gen_dtsv,    0),
};
#define EXH_NDOMS ((int)(sizeof exh_doms / sizeof exh_doms[0]))
static inline void exh_init(void) {
    for (int k = 0; k < EXH_NDOMS; k++) {
        uint64_t h = EXH_FNV_OFFSET; for (const char* s = exh_doms[k].name; *s; s++) h = (h ^ (uint8_t)*s) * EXH_FNV_PRIME;
        exh_doms[k].seed = exh_mix(h ^ EXH_SEED);
    }
}
static inline exh_dom* exh_find_dom(const char* name) {
    for (int k = 0; k < EXH_NDOMS; k++) { const char* a = exh_doms[k].name; const char* b = name; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) return &exh_doms[k]; }
    return NULL;
}
static inline uint64_t exh_nblocks(const exh_dom* d) { return (d->ncalls + EXH_BLOCK - 1) >> EXH_BLOCK_LOG2; }

#endif /* SPU_EXH_GEN_H */
