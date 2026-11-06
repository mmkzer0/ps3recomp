/* spuexh.h - shared definitions of the spuexh probe: wire structures (PPU/SPU/host), the op table type, the
 * run configuration and the job plan.
 *
 * Compiled by three programs: the SPU program (spu-gcc), the PPU probe (ppu-gcc, PSL1GHT) and the host dry-run
 * driver (cc). Plain C, <stdint.h> only. The wire structures are big-endian in memory (the byte order of the SPU
 * and the PPU); on the little-endian host every field is converted with W32()/W64() (self-inverse swaps).
 *
 * THE JOB PLAN. A job is one (domain, block) pair: the generator runs once over the 2^20 calls of the block and every
 * active op of the domain is applied to each call (the way the host harness does it, test_spu_exhaustive.c). The
 * jobs of a configuration form one deterministic sequence (domain-major, blocks ascending); SPU thread s of N runs the
 * jobs whose sequence number is congruent to s modulo N. No job table is exchanged: every thread enumerates the
 * same sequence from the same configuration.
 */
#ifndef SPUEXH_H
#define SPUEXH_H

#include <stdint.h>
#include <stddef.h>

#include "spu_exh_gen.h"          /* the portable generator, unchanged (staged from runtime/spu/tests by prebuild.sh) */

/* ---------------------------------------------------------------- byte order of the wire structures */
#if defined(__BIG_ENDIAN__) || defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ || defined(__SPU__) || defined(__PPC__)
#define W16(x) (x)
#define W32(x) (x)
#define W64(x) (x)
#else
#define W16(x) __builtin_bswap16(x)
#define W32(x) __builtin_bswap32(x)
#define W64(x) __builtin_bswap64(x)
#endif

/* ---------------------------------------------------------------- op table */
typedef struct {
    const char *name;        /* host harness name, "cflts@173" for a conversion at I8 = 173 */
    uint32_t word;           /* instruction template: opcode and registers (a=$3 b=$4 c=$5), immediate field zero */
    uint8_t imm_shift;       /* immediate field position (bit number from the LSB), 0 when imm_bits == 0 */
    uint8_t imm_bits;        /* width of the immediate field, 0 = the instruction has none */
    uint8_t need;            /* operand mask of the host harness: 1 = a, 2 = b, 4 = c */
    uint8_t heavy;           /* the 44 non-nominal conversion scales */
    int16_t fixed_imm;       /* >= 0: the op's own I8 (conversion scale); -1: the call's imm */
    uint8_t rt;              /* register the result is read from ($3, or $5 for ops whose rt is an input) */
    uint8_t dom[5];          /* domain indexes (into exh_doms / spuexh_dom_names), 255 = unused */
} spuexh_op_t;

#include "spuexh_ops_gen.h"      /* generated: spuexh_ops[SPUEXH_NOPS], spuexh_dom_names[] */

#if SPUEXH_NDOMS_GEN != 37
#error "domain table of the generator changed: regenerate and review"
#endif

/* ---------------------------------------------------------------- configuration */
#define SPUEXH_CFG_MAGIC 0x53584346u   /* 'SXCF' */
#define SPUEXH_OPWORDS ((SPUEXH_NOPS + 31) / 32)

typedef struct {
    uint32_t magic;
    uint32_t stride_all;               /* a block is hashed when blk % stride == 0 (1 = every block) */
    uint32_t stride_heavy;             /* extra stride of the heavy conversion ops (host default 8) */
    uint32_t bmax;                     /* 0 = all, else only blocks < bmax */
    uint32_t dom_stride[SPUEXH_NDOMS_GEN];   /* 0 = stride_all */
    uint32_t dom_bmax[SPUEXH_NDOMS_GEN];     /* 0 = bmax */
    uint32_t op_en[SPUEXH_OPWORDS];    /* enabled ops */
    uint32_t dom_en_lo, dom_en_hi;     /* enabled domains (bit k = domain k), 37 domains */
} spuexh_cfg_t;                        /* wire format: all fields W32 */

static inline int spuexh_op_enabled(const spuexh_cfg_t *c, int k) { return (W32(c->op_en[k >> 5]) >> (k & 31)) & 1u; }
static inline int spuexh_dom_enabled(const spuexh_cfg_t *c, int d) {
    return d < 32 ? (W32(c->dom_en_lo) >> d) & 1u : (W32(c->dom_en_hi) >> (d - 32)) & 1u;
}
static inline void spuexh_set_op(spuexh_cfg_t *c, int k, int on) {
    uint32_t v = W32(c->op_en[k >> 5]); if (on) v |= 1u << (k & 31); else v &= ~(1u << (k & 31)); c->op_en[k >> 5] = W32(v);
}
static inline void spuexh_set_dom(spuexh_cfg_t *c, int d, int on) {
    uint32_t *p = d < 32 ? &c->dom_en_lo : &c->dom_en_hi; int b = d < 32 ? d : d - 32;
    uint32_t v = W32(*p); if (on) v |= 1u << b; else v &= ~(1u << b); *p = W32(v);
}

/* The ops of domain `dom` that are active in block `blk` (host rule: stride per op). Returns the count; out[] receives
 * op indexes in table order. Needs exh_init() NOT called (only names and block counts are used). */
static inline int spuexh_job_ops(const spuexh_cfg_t *c, int dom, uint64_t blk, uint16_t *out) {
    uint32_t dbmax = W32(c->dom_bmax[dom]), bmax = dbmax ? dbmax : W32(c->bmax);
    if (bmax && blk >= bmax) return 0;
    uint32_t sa = W32(c->dom_stride[dom]); if (!sa) sa = W32(c->stride_all); if (!sa) sa = 1;
    uint32_t sh = W32(c->stride_heavy); if (!sh) sh = 1;
    int n = 0;
    for (int k = 0; k < SPUEXH_NOPS; k++) {
        if (!spuexh_op_enabled(c, k)) continue;
        const spuexh_op_t *o = &spuexh_ops[k];
        int in = 0; for (int q = 0; q < 5; q++) if (o->dom[q] == dom) in = 1;
        if (!in) continue;
        uint32_t st = o->heavy ? (sh > sa ? sh : sa) : sa;
        if (blk % st) continue;
        out[n++] = (uint16_t)k;
    }
    return n;
}

/* ---------------------------------------------------------------- wire structures */
#define SPUEXH_PARAMS_MAGIC 0x53585031u  /* 'SXP1' */
#define SPUEXH_STATUS_MAGIC 0x53585331u  /* 'SXS1' */
#define SPUEXH_MODE_MAIN  0     /* the exhaustive sweep */
#define SPUEXH_MODE_CASES 1     /* special probe A: run a list of (word, a, b, c) cases, record $rt */
#define SPUEXH_MODE_RAW   2     /* special probe B: one raw word with all 128 registers loaded and dumped */
#define SPUEXH_MODE_LANES 3     /* lanes job: the inputs and the result of the first calls of one (op, domain, block) */

typedef struct {                /* PPU -> SPU, 128 bytes, 128-byte aligned in main memory */
    uint32_t magic, mode, spu_index, spu_count;
    uint32_t start_job;         /* MAIN: own jobs to skip (already done or faulted) */
    uint32_t results_cap;       /* MAIN: capacity of the results region in entries */
    uint32_t start_entries;     /* MAIN: entries already in the region */
    uint32_t ncases;            /* CASES/RAW: number of cases */
    uint64_t results_ea;        /* MAIN: entries; CASES/RAW: case outputs (SPUEXH_CASEOUT_SIZE each) */
    uint64_t status_ea;         /* status block of this thread (128 bytes) */
    uint64_t cfg_ea;            /* MAIN: spuexh_cfg_t */
    uint64_t cases_ea;          /* CASES/RAW: spuexh_case_t array */
    uint32_t cfg_size, flags;
    uint32_t lane_op, lane_dom, lane_blk, lane_count;   /* LANES: op table index, domain index, block, calls to dump */
    uint32_t pad[10];
} spuexh_params_t;

typedef struct {                /* SPU -> PPU, 128 bytes, updated by DMA */
    uint32_t magic, state;      /* state: 0 none, 1 running, 2 done */
    uint32_t jobs_started, jobs_done;   /* own jobs */
    uint32_t entries;           /* entries valid in the results region (including start_entries) */
    uint32_t cases_started, cases_done; /* CASES/RAW */
    uint32_t last_dom, last_blk;        /* job in progress */
    uint32_t flags, pad0, pad1;
    uint64_t busy_ticks;        /* SPU decrementer ticks spent in jobs */
    uint64_t heartbeat;         /* incremented while a long job runs */
    uint32_t fpscr_start[4];    /* fscrrd after the program set the defined FPSCR state at thread start (spu_main.c) */
    uint32_t fpscr_end[4];
    uint32_t pad[8];
} spuexh_status_t;

#define SPUEXH_ENT_TIME 0xFFFFu  /* entry type: job timing record (hash field = decrementer ticks) */
#define SPUEXH_ENT_GEN  0xFFFEu  /* entry type: generator self-check of a domain (hash field = digest, see spuexh_gencheck) */
#define SPUEXH_TIME_OVERFLOW 0xFFFFFFFFu  /* TIME sentinel: the job outlasted one decrementer period (~53.8 s at 79.8 MHz) */
typedef struct {                /* one result, 16 bytes */
    uint64_t hash;
    uint16_t op, dom;
    uint32_t blk;
} spuexh_entry_t;

typedef struct {                /* CASES/RAW input, 64 bytes */
    uint32_t word, flags, id, pad;
    uint32_t a[4], b[4], c[4];
} spuexh_case_t;

#define SPUEXH_CASEOUT_SIZE 512
#define SPUEXH_RAW_MAXCH 8
typedef struct {                /* CASES/RAW output, 512 bytes */
    uint32_t magic, state;      /* state: 0 not reached, 1 about to execute, 2 executed and recorded */
    uint32_t id, nchanged;
    uint32_t arena_hit, arena_addr, arena_val, pad0;
    uint32_t r[4];              /* CASES: $rt after the word */
    struct { uint32_t reg, pad[3]; uint32_t v[4]; } ch[SPUEXH_RAW_MAXCH];   /* RAW: first changed registers */
    uint32_t pad[SPUEXH_CASEOUT_SIZE / 4 - 12 - 8 * SPUEXH_RAW_MAXCH];
} spuexh_caseout_t;

/* LANES output: one record per call, 80 bytes (a multiple of 16, so a chunk of records is one legal DMA). The SPU keeps one
 * chunk of records in local store (CHUNK x 80 bytes, 5 KiB, part of the image: the build guard still checks the image
 * against arena A) and DMAs each finished chunk to the results region; status.entries counts the records that are in main memory. */
#define SPUEXH_LANES_MAX_ENTRIES 16     /* lanes= keys per configuration */
#define SPUEXH_LANES_MAX_COUNT 1024     /* calls dumped per entry */
#define SPUEXH_LANES_DEFAULT_COUNT 64
typedef struct {
    uint32_t a[4], b[4], c[4];  /* the operands the generator produced (the host harness' --dump prints them as 8 hex digits per word) */
    uint32_t out[4];            /* $rt after the instruction word */
    uint64_t call;              /* generator call index within the domain (the first column of the harness' --dump) */
    uint32_t imm;               /* the immediate field value patched into the word (call imm masked to the field width); 0 for an op without one */
    uint32_t pad;
} spuexh_lane_t;

#define SPUEXH_ARENA_A 0x1E000u   /* LS arenas the raw-exec probe points registers into */
#define SPUEXH_ARENA_A_SIZE 0x1000u
#define SPUEXH_ARENA_B 0x3C000u
#define SPUEXH_ARENA_B_SIZE 0x1800u

/* LS layout guard. Neither arena is reserved by the linker: the PSL1GHT SPU script (elf32_spu.x) puts the stack at
 * __stack = 0x3FFF0 (top of the 256 KiB local store) and it grows down. These asserts pin the arena-vs-stack side;
 * the loaded-image side (_end < arena A) is checked at link time with spu-nm in spu/Makefile, because _end only
 * exists after linking. spu/Makefile fails the build when a rebuild moves the image into the arenas. */
#define SPUEXH_STACK_TOP 0x3FFF0u
#define SPUEXH_STACK_RESERVE 0x2000u                  /* what the probe itself may use of the stack at most */
_Static_assert(SPUEXH_ARENA_A >= SPUEXH_ARENA_B + SPUEXH_ARENA_B_SIZE || SPUEXH_ARENA_B >= SPUEXH_ARENA_A + SPUEXH_ARENA_A_SIZE,
               "the LS arenas overlap each other");
_Static_assert(SPUEXH_ARENA_B + SPUEXH_ARENA_B_SIZE <= SPUEXH_STACK_TOP - SPUEXH_STACK_RESERVE,
               "arena B reaches into the SPU stack reserve (move the arena or raise SPUEXH_STACK_RESERVE)");

/* Generator self-check: a digest of 256 calls of domain `di` (the generator output itself, no op involved). The SPU
 * emits it at the start of a sweep and the host computes it natively (spuexh_host --gencheck): if the hardware digest
 * differs, the SPU code of the generator (its integer instructions) is at fault, not any op under test. exh_init() first. */
static inline uint64_t spuexh_gencheck(int di) {
    const exh_dom *d = &exh_doms[di];
    uint64_t h = EXH_FNV_OFFSET;
    for (uint32_t k = 0; k < 256; k++) {
        G g;
        const uint64_t i = exh_mix(0x6E6E000000000000ull + (uint64_t)di * 1000ull + k) % d->ncalls;
        d->gen(d, i, &g, 7u);
        h = exh_fold(h, g.a); h = exh_fold(h, g.b); h = exh_fold(h, g.c);
        V4 v; v.w[0] = (uint32_t)g.imm; v.w[1] = (uint32_t)i; v.w[2] = (uint32_t)(i >> 32); v.w[3] = k;
        h = exh_fold(h, v);
    }
    return h;
}

_Static_assert(sizeof(spuexh_params_t) == 128, "params wire size");
_Static_assert(sizeof(spuexh_status_t) == 128, "status wire size");
_Static_assert(sizeof(spuexh_entry_t) == 16, "entry wire size");
_Static_assert(sizeof(spuexh_case_t) == 64, "case wire size");
_Static_assert(sizeof(spuexh_caseout_t) == SPUEXH_CASEOUT_SIZE, "caseout wire size");
_Static_assert(sizeof(spuexh_lane_t) == 80, "lane wire size");

#endif /* SPUEXH_H */
