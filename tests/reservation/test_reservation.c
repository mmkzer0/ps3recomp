/*
 * The reservation model (runtime/memory/reservation.h), tested the way a guest
 * meets it: PPU lwarx/ldarx + stwcx./stdcx. against SPU GETLLAR + PUTLLC /
 * PUTLLUC / PUT, on the real SPU channel path (spu_wrch / spu_rdch, so what runs
 * is spu_mfc_atomic() and the DMA engine, not a copy of their rules) and the
 * real PPU store path (resv_*, ppu_memory.h's vm_write32).
 *
 *   contract      deterministic, single thread, no sleeps: who kills whose
 *                 reservation, same value and different value, in both
 *                 directions, plus what must NOT kill one (another line) and
 *                 the documented limits, asserted as limits
 *   jq2 N         the JQ2 port line: an SPU PUTLLC increments the credit at
 *                 +0x00 while PPU lwarx/stwcx. (word) and ldarx/stdcx.
 *                 (doubleword) loops increment +0x10 and +0x18 on the same
 *                 128-byte line, N increments per counter, all concurrently.
 *                 No update may be lost
 *   aba_ppu N     N rounds, each: PPU lwarx; SPU GETLLAR + PUTLLC of the SAME
 *                 bytes; PPU stwcx. of the same value. Every stwcx. must fail
 *   aba_spu N     N rounds, each: SPU GETLLAR; PPU lwarx + stwcx. of the SAME
 *                 value; SPU PUTLLC. Every PUTLLC must fail
 *
 * The two aba modes are handshakes on atomics, not sleeps: the interleaving
 * that matters is built, not raced for. Exit status 0 = pass.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reservation.h"
#include "spu_coherency.h"
#include "spu_context.h"
#include "ppu_memory.h"

/* The channel ABI the lifter emits externs for (spu_channels.c). */
u128     spu_rdch(spu_context* ctx, uint32_t channel);
void     spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

/* Guest memory, and what the SPU channel layer expects of its host: the PPU's
 * watch hooks, the host clock and the title-specific SPURS hooks. None of them
 * is on a path this test drives. */
uint8_t* vm_base;
uint32_t ppu_vm_size = 0;
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

#define GUEST_BYTES  (4u << 20)
/* Lines well above the null-destination guard of the DMA engine. */
#define LINE_L   0x00100000u
#define LINE_M   (LINE_L + 128u)      /* the neighbour of L */
#define LINE_U   0x00200000u          /* a line no SPU ever reserves */
#define LINE_V   (LINE_U + 128u)      /* its neighbour */

#define WORD     0x10u                /* the PPU's word inside the line */
#define DWORD    0x18u

#define LSA      0x1000u              /* staging area of the SPU under test */
#define LSB      0x2000u

static int g_pass, g_fail;
static const char* g_current = "(none)";
#define TEST(name) (g_current = (name))
#define CHECK(cond) do {                                                    \
    if (cond) g_pass++;                                                     \
    else { g_fail++; fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",          \
                             g_current, __FILE__, __LINE__, #cond); }       \
} while (0)

/* ---- guest memory, single-threaded phases ------------------------------- */
static uint32_t get32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
static void     put32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(vm_base + ea, &v, 4); }
static uint64_t get64(uint32_t ea) { uint64_t v; memcpy(&v, vm_base + ea, 8); return __builtin_bswap64(v); }

/* ---- the SPU, through the channel path ---------------------------------- */
static spu_context* spu_new(uint32_t id)
{
    spu_context* c = (spu_context*)calloc(1, sizeof *c);   /* 256 KB LS: not on the stack */
    if (!c) abort();
    spu_context_init(c, 0);
    c->spu_id = id;
    return c;
}

static void wrch(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, spu_make_preferred_u32(v)); }

static void mfc_atomic(spu_context* c, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(c, MFC_LSA, lsa); wrch(c, MFC_EAH, 0); wrch(c, MFC_EAL, ea); wrch(c, MFC_Cmd, cmd);
}

static void mfc_put(spu_context* c, uint32_t lsa, uint32_t ea, uint32_t size)
{
    wrch(c, MFC_LSA, lsa); wrch(c, MFC_EAH, 0); wrch(c, MFC_EAL, ea);
    wrch(c, MFC_Size, size); wrch(c, MFC_TagID, 0); wrch(c, MFC_Cmd, MFC_PUT_CMD);
}

static uint32_t atomic_status(spu_context* c) { return spu_rdch(c, MFC_RdAtomicStat)._u32[0]; }

#define ST_PUTLLC_OK    0u
#define ST_PUTLLC_FAIL  1u
#define ST_PUTLLUC      2u
#define ST_GETLLAR      4u

static uint32_t spu_getllar(spu_context* c, uint32_t lsa, uint32_t ea)
{
    mfc_atomic(c, lsa, ea, MFC_GETLLAR_CMD);
    return atomic_status(c);
}
/* Returns the AtomicStat the guest would read. */
static uint32_t spu_putllc(spu_context* c, uint32_t lsa, uint32_t ea)
{
    mfc_atomic(c, lsa, ea, MFC_PUTLLC_CMD);
    return atomic_status(c);
}
static uint32_t spu_putlluc(spu_context* c, uint32_t lsa, uint32_t ea)
{
    mfc_atomic(c, lsa, ea, MFC_PUTLLUC_CMD);
    return atomic_status(c);
}

static uint32_t ls_get32(spu_context* c, uint32_t lsa, uint32_t off)
{
    uint32_t v; memcpy(&v, &c->ls[lsa + off], 4); return __builtin_bswap32(v);
}
static void ls_put32(spu_context* c, uint32_t lsa, uint32_t off, uint32_t v)
{
    v = __builtin_bswap32(v); memcpy(&c->ls[lsa + off], &v, 4);
}
static int lr_raised(spu_context* c) { return (c->event_status & SPU_EVENT_LR) != 0; }

/* A fresh line: zero it, forget every reservation anywhere. Epochs are never
 * reset (a line keeps its history, as in a running title), so no test relies
 * on an epoch value, only on epochs differing. */
static void reset(spu_context* a, spu_context* b, ppu_context* p, ppu_context* q)
{
    memset(vm_base + LINE_L, 0, 256);
    memset(vm_base + LINE_U, 0, 256);
    if (a) { spu_context_init(a, 0); a->spu_id = 0; }
    if (b) { spu_context_init(b, 0); b->spu_id = 1; }
    if (p) ppu_context_clear(p);
    if (q) ppu_context_clear(q);
}

/* ===========================================================================
 * contract
 * ===========================================================================*/

/* PPU lwarx -> SPU commits to the line -> PPU stwcx. */
static void test_spu_commit_kills_ppu(spu_context* s, ppu_context* p)
{
    reset(s, NULL, p, NULL);
    put32(LINE_L + WORD, 0x1111);

    TEST("PPU lwarx, SPU PUTLLC of the SAME bytes, PPU stwcx. fails");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);       /* it rewrote what it read */
    CHECK(get32(LINE_L + WORD) == 0x1111);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x2222) == 0);
    CHECK(get32(LINE_L + WORD) == 0x1111);                    /* and stored nothing */

    TEST("PPU lwarx, SPU PUTLLC changing the PPU's word, PPU stwcx. fails");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    ls_put32(s, LSA, WORD, 0x3333);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x2222) == 0);
    CHECK(get32(LINE_L + WORD) == 0x3333);

    TEST("PPU lwarx, SPU PUTLLC changing ANOTHER word of the line, PPU stwcx. fails");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x3333);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    ls_put32(s, LSA, 0, 0xCAFE);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
    CHECK(get32(LINE_L) == 0xCAFE);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x4444) == 0);
    CHECK(get32(LINE_L + WORD) == 0x3333);

    TEST("the same through ldarx/stdcx.");
    CHECK(resv_ldarx64(p, LINE_L + DWORD) == 0);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
    CHECK(resv_stcx64(p, LINE_L + DWORD, 0x55) == 0);
    CHECK(get64(LINE_L + DWORD) == 0);

    TEST("a PUTLLC on the NEXT line (L+128) does not kill a reservation on L");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x3333);
    CHECK(spu_getllar(s, LSA, LINE_M) == ST_GETLLAR);
    ls_put32(s, LSA, 0, 0x9999);
    CHECK(spu_putllc(s, LSA, LINE_M) == ST_PUTLLC_OK);
    CHECK(get32(LINE_M) == 0x9999);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x5555) == 1);
    CHECK(get32(LINE_L + WORD) == 0x5555);

    TEST("a failed PUTLLC stored nothing, so the PPU reservation stands");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x5555);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    s->resv_valid = 0;                                        /* it lost its reservation */
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x6666) == 1);
}

/* SPU GETLLAR -> PPU stwcx. -> SPU PUTLLC */
static void test_ppu_commit_kills_spu(spu_context* s, ppu_context* p)
{
    reset(s, NULL, p, NULL);
    put32(LINE_L + WORD, 0x1111);

    TEST("SPU GETLLAR, PPU stwcx. of the SAME value, PUTLLC fails and says so");
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x1111) == 1);       /* a store of the value it holds */
    CHECK(lr_raised(s));                                      /* SPU_EVENT_LR */
    ls_put32(s, LSA, 0, 0xBAD);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);      /* MFC_RdAtomicStat */
    CHECK(get32(LINE_L) == 0);                                /* and wrote nothing */

    TEST("SPU GETLLAR, PPU stwcx. of a DIFFERENT value, PUTLLC fails");
    reset(s, NULL, p, NULL);
    put32(LINE_L + WORD, 0x1111);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x2222) == 1);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);
    CHECK(get32(LINE_L + WORD) == 0x2222);

    TEST("the same through ldarx/stdcx.");
    reset(s, NULL, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_ldarx64(p, LINE_L + DWORD) == 0);
    CHECK(resv_stcx64(p, LINE_L + DWORD, 0) == 1);            /* zero over zero */
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);

    TEST("a stwcx. on the NEXT line (L+128) does not kill an SPU reservation on L");
    reset(s, NULL, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_lwarx32(p, LINE_M + WORD) == 0);
    CHECK(resv_stcx32(p, LINE_M + WORD, 0) == 1);
    CHECK(!lr_raised(s));
    ls_put32(s, LSA, 0, 0x7777);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
    CHECK(get32(LINE_L) == 0x7777);

    TEST("a FAILED stwcx. stored nothing, so the SPU reservation stands");
    reset(s, NULL, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_stcx32(p, LINE_L + WORD, 1) == 0);             /* no lwarx */
    CHECK(!lr_raised(s));
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
}

/* PUTLLUC, DMA PUT, and the SPU's own MFC write */
static void test_other_spu_writers(spu_context* s, spu_context* t, ppu_context* p)
{
    reset(s, t, p, NULL);
    put32(LINE_L + WORD, 0x1111);

    TEST("SPU PUTLLUC kills a PPU reservation, even of the same bytes");
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);         /* marks the line, fills s's LSA */
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    memcpy(&t->ls[LSA], &s->ls[LSA], 128);                    /* t stores the line as it is */
    CHECK(spu_putlluc(t, LSA, LINE_L) == ST_PUTLLUC);
    CHECK(get32(LINE_L + WORD) == 0x1111);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x2222) == 0);

    TEST("SPU PUTLLUC kills an SPU reservation");
    reset(s, t, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(spu_putlluc(t, LSB, LINE_L) == ST_PUTLLUC);
    CHECK(lr_raised(s));
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);

    TEST("a plain DMA PUT of the same bytes into a reserved line kills a PPU reservation");
    reset(s, t, p, NULL);
    put32(LINE_L + WORD, 0x1111);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);         /* s marks the line */
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0x1111);
    memcpy(&t->ls[LSB], &s->ls[LSA], 128);
    mfc_put(t, LSB, LINE_L, 128);
    CHECK(get32(LINE_L + WORD) == 0x1111);
    CHECK(resv_stcx32(p, LINE_L + WORD, 0x2222) == 0);
    CHECK(lr_raised(s));

    TEST("a plain DMA PUT into the NEXT line leaves a reservation on L alone");
    reset(s, t, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 0);
    mfc_put(t, LSB, LINE_M, 128);
    CHECK(resv_stcx32(p, LINE_L + WORD, 1) == 1);

    TEST("an SPU's own DMA PUT of the bytes it holds does not cost it its reservation");
    reset(s, t, p, NULL);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    memcpy(&s->ls[LSB], &s->ls[LSA], 128);
    mfc_put(s, LSB, LINE_L, 128);
    CHECK(!lr_raised(s));
    ls_put32(s, LSA, 0, 0xFEED);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_OK);
    CHECK(get32(LINE_L) == 0xFEED);

    TEST("a reservation lost to an epoch-only write raises SPU_EVENT_LR when polled");
    reset(s, NULL, p, NULL);
    wrch(s, SPU_WrEventMask, SPU_EVENT_LR);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);
    CHECK(spu_rchcnt(s, SPU_RdEventStat) == 0);
    resv_bump(LINE_L);                                        /* an unreported same-bytes writer */
    CHECK(spu_rchcnt(s, SPU_RdEventStat) == 1);
    CHECK(spu_putllc(s, LSA, LINE_L) == ST_PUTLLC_FAIL);
}

/* PPU against PPU, and what a plain store does. The lines used here are ones no
 * SPU has reserved (LINE_U, LINE_V), so a stwcx. takes the line lock alone;
 * the marked-line path (lock-line lock first) is what the tests above run. */
static void test_ppu_side(spu_context* s, ppu_context* p, ppu_context* q)
{
    reset(s, NULL, p, q);
    put32(LINE_U + WORD, 7);

    TEST("PPU stwcx. of the same value kills another thread's reservation");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 7);
    CHECK(resv_lwarx32(q, LINE_U + WORD) == 7);
    CHECK(resv_stcx32(q, LINE_U + WORD, 7) == 1);
    CHECK(resv_stcx32(p, LINE_U + WORD, 8) == 0);
    CHECK(get32(LINE_U + WORD) == 7);

    TEST("...and a stdcx. to ANOTHER word of the same line (the line is the granule)");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 7);
    CHECK(resv_ldarx64(q, LINE_U + DWORD) == 0);
    CHECK(resv_stcx64(q, LINE_U + DWORD, 9) == 1);
    CHECK(resv_stcx32(p, LINE_U + WORD, 8) == 0);

    TEST("...but not a stwcx. on the next line");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 7);
    CHECK(resv_lwarx32(q, LINE_V + WORD) == 0);
    CHECK(resv_stcx32(q, LINE_V + WORD, 1) == 1);
    CHECK(resv_stcx32(p, LINE_U + WORD, 8) == 1);

    TEST("a reservation is consumed by its stwcx., win or lose");
    CHECK(resv_stcx32(p, LINE_U + WORD, 9) == 0);

    TEST("stwcx. without a matching reservation: wrong address, wrong width, none");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 8);
    CHECK(resv_stcx32(p, LINE_U + WORD + 4, 1) == 0);
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 8);
    CHECK(resv_stcx64(p, LINE_U + WORD, 1) == 0);
    CHECK(resv_stcx32(q, LINE_U + WORD, 1) == 0);
    CHECK(get32(LINE_U + WORD) == 8);

    TEST("a plain PPU store of a DIFFERENT value kills a reservation (value compare)");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 8);
    vm_write32(LINE_U + WORD, 12);
    CHECK(resv_stcx32(p, LINE_U + WORD, 13) == 0);

    TEST("a plain PPU store into a line an SPU has reserved kills a reservation even of the same value");
    put32(LINE_L + WORD, 12);
    CHECK(spu_getllar(s, LSA, LINE_L) == ST_GETLLAR);         /* marks L */
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 12);
    vm_write32(LINE_L + WORD, 12);
    CHECK(resv_stcx32(p, LINE_L + WORD, 13) == 0);
    CHECK(lr_raised(s));

    /* The loader's store path (VM_WRITE_COH) calls resv_keep_own with the
     * storing thread's context after reporting the store. */
    TEST("a thread's OWN plain store into its reserved line keeps its reservation; another thread's does not");
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 12);
    CHECK(resv_lwarx32(q, LINE_L + 0x40) == 0);
    vm_write32(LINE_L + 0x44, 5);                             /* p stores elsewhere in the line ... */
    resv_keep_own(p, LINE_L + 0x44);                          /* ... as the loader reports it */
    CHECK(resv_stcx32(q, LINE_L + 0x40, 1) == 0);             /* q was not the storer */
    CHECK(resv_stcx32(p, LINE_L + WORD, 13) == 1);
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 13);
    vm_write32(LINE_L + 0x44, 6);                             /* q stores: p gets no keep_own */
    CHECK(resv_stcx32(p, LINE_L + WORD, 14) == 0);
    CHECK(resv_lwarx32(p, LINE_L + WORD) == 13);
    vm_write32(LINE_L + 0x44, 7);                             /* the SAME thread, but a store to the reserved word ... */
    resv_keep_own(p, LINE_L + 0x44);
    vm_write32(LINE_L + WORD, 99);                            /* ... changes the bytes: the value compare kills it */
    resv_keep_own(p, LINE_L + WORD);
    CHECK(resv_stcx32(p, LINE_L + WORD, 14) == 0);

    /* LIMIT 1 (reservation.h): the lifter's plain stores do not consult the
     * epoch on a line no SPU has reserved. If this starts to fail the hole is
     * closed: update reservation.h and AM-10, then this test. */
    TEST("LIMIT: a same-value plain PPU store to a never-SPU-reserved line does NOT kill one");
    put32(LINE_U + WORD, 7);
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 7);
    vm_write32(LINE_U + WORD, 7);
    CHECK(resv_stcx32(p, LINE_U + WORD, 8) == 1);

    /* LIMIT 2: the same for an SPU DMA PUT into a line only a PPU reserved. */
    TEST("LIMIT: a same-value SPU DMA PUT to a never-SPU-reserved line does NOT kill a PPU reservation");
    CHECK(resv_lwarx32(p, LINE_U + WORD) == 8);
    memcpy(&s->ls[LSB], vm_base + LINE_U, 128);
    mfc_put(s, LSB, LINE_U, 128);
    CHECK(resv_stcx32(p, LINE_U + WORD, 9) == 1);
}

static int contract(void)
{
    vm_base = (uint8_t*)aligned_alloc(4096, GUEST_BYTES);
    if (!vm_base) return 2;
    memset(vm_base, 0, GUEST_BYTES);
    spu_context* s = spu_new(0);
    spu_context* t = spu_new(1);
    ppu_context p, q;

    test_spu_commit_kills_ppu(s, &p);
    test_ppu_commit_kills_spu(s, &p);
    test_other_spu_writers(s, t, &p);
    test_ppu_side(s, &p, &q);

    spu_coh_unregister(s);
    spu_coh_unregister(t);
    printf("reservation contract: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* ===========================================================================
 * jq2: the port line
 * ===========================================================================*/
typedef struct {
    uint64_t        n;
    spu_context*    spu;
    ppu_context     ppu;
    uint64_t        retries;
} worker;

static void* spu_credit(void* arg)
{
    worker* w = (worker*)arg;
    spu_context* c = w->spu;
    for (uint64_t i = 0; i < w->n; i++) {
        for (;;) {
            spu_getllar(c, LSA, LINE_L);
            ls_put32(c, LSA, 0, ls_get32(c, LSA, 0) + 1);
            if (spu_putllc(c, LSA, LINE_L) == ST_PUTLLC_OK) break;
            w->retries++;
        }
    }
    return NULL;
}

static void* ppu_submit32(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        uint32_t v;
        do { v = resv_lwarx32(&w->ppu, LINE_L + WORD); w->retries++; }
        while (!resv_stcx32(&w->ppu, LINE_L + WORD, v + 1));
        w->retries--;                      /* the first attempt is not a retry */
    }
    return NULL;
}

static void* ppu_submit64(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        uint64_t v;
        do { v = resv_ldarx64(&w->ppu, LINE_L + DWORD); w->retries++; }
        while (!resv_stcx64(&w->ppu, LINE_L + DWORD, v + 1));
        w->retries--;
    }
    return NULL;
}

static int jq2(uint64_t n)
{
    vm_base = (uint8_t*)aligned_alloc(4096, GUEST_BYTES);
    if (!vm_base) return 2;
    memset(vm_base, 0, GUEST_BYTES);

    worker spu_w = { .n = n, .spu = spu_new(0) }, w32 = { .n = n }, w64 = { .n = n };
    ppu_context_clear(&w32.ppu);
    ppu_context_clear(&w64.ppu);

    pthread_t t[3];
    pthread_create(&t[0], NULL, spu_credit,   &spu_w);
    pthread_create(&t[1], NULL, ppu_submit32, &w32);
    pthread_create(&t[2], NULL, ppu_submit64, &w64);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);

    const uint32_t credit = get32(LINE_L);
    const uint32_t subs32 = get32(LINE_L + WORD);
    const uint64_t subs64 = get64(LINE_L + DWORD);
    printf("jq2 n=%llu: credit(+0x00)=%u submissions(+0x10)=%u (+0x18)=%llu"
           "  retries spu=%llu ppu32=%llu ppu64=%llu\n",
           (unsigned long long)n, credit, subs32, (unsigned long long)subs64,
           (unsigned long long)spu_w.retries, (unsigned long long)w32.retries,
           (unsigned long long)w64.retries);

    spu_coh_unregister(spu_w.spu);
    int bad = credit != (uint32_t)n || subs32 != (uint32_t)n || subs64 != n;
    if (bad) fprintf(stderr, "FAIL: a counter on the port line lost an update\n");
    return bad ? 1 : 0;
}

/* ===========================================================================
 * aba: same-value writes, in the order that must kill the reservation
 * ===========================================================================*/
/* One counter owns the interleaving. Each round is three steps (k = 3i, 3i+1,
 * 3i+2); the thread that owns step k waits for exactly k, acts, and stores
 * k+1. A value is only ever advanced by the one thread waiting on it, so no
 * edge can be missed and nothing sleeps. */
static _Atomic uint64_t g_step;
static _Atomic uint64_t g_wrong;

static void spin_until(uint64_t k)
{
    while (atomic_load_explicit(&g_step, memory_order_acquire) != k) { }
}
static void advance(uint64_t k) { atomic_store_explicit(&g_step, k + 1, memory_order_release); }

/* aba_ppu: 3i PPU lwarx | 3i+1 SPU GETLLAR + PUTLLC of the same bytes | 3i+2 PPU stwcx. of the same value */
static void* aba_ppu_cpu(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        spin_until(3 * i);
        uint32_t v = resv_lwarx32(&w->ppu, LINE_L + WORD);
        advance(3 * i);
        spin_until(3 * i + 2);
        if (resv_stcx32(&w->ppu, LINE_L + WORD, v)) atomic_fetch_add(&g_wrong, 1);
        advance(3 * i + 2);
    }
    return NULL;
}
static void* aba_ppu_spu(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        spin_until(3 * i + 1);
        spu_getllar(w->spu, LSA, LINE_L);
        if (spu_putllc(w->spu, LSA, LINE_L) != ST_PUTLLC_OK) atomic_fetch_add(&g_wrong, 1);
        advance(3 * i + 1);
    }
    return NULL;
}

/* aba_spu: 3i SPU GETLLAR | 3i+1 PPU lwarx + stwcx. of the same value | 3i+2 SPU PUTLLC */
static void* aba_spu_spu(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        spin_until(3 * i);
        spu_getllar(w->spu, LSA, LINE_L);
        advance(3 * i);
        spin_until(3 * i + 2);
        if (spu_putllc(w->spu, LSA, LINE_L) != ST_PUTLLC_FAIL) atomic_fetch_add(&g_wrong, 1);
        advance(3 * i + 2);
    }
    return NULL;
}
static void* aba_spu_cpu(void* arg)
{
    worker* w = (worker*)arg;
    for (uint64_t i = 0; i < w->n; i++) {
        spin_until(3 * i + 1);
        uint32_t v = resv_lwarx32(&w->ppu, LINE_L + WORD);
        if (!resv_stcx32(&w->ppu, LINE_L + WORD, v)) atomic_fetch_add(&g_wrong, 1);
        advance(3 * i + 1);
    }
    return NULL;
}

static int aba(int ppu_reserves, uint64_t n)
{
    vm_base = (uint8_t*)aligned_alloc(4096, GUEST_BYTES);
    if (!vm_base) return 2;
    memset(vm_base, 0, GUEST_BYTES);
    put32(LINE_L + WORD, 0x1234);

    worker w = { .n = n, .spu = spu_new(0) };
    ppu_context_clear(&w.ppu);
    pthread_t a, b;
    pthread_create(&a, NULL, ppu_reserves ? aba_ppu_cpu : aba_spu_cpu, &w);
    pthread_create(&b, NULL, ppu_reserves ? aba_ppu_spu : aba_spu_spu, &w);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    spu_coh_unregister(w.spu);

    const uint64_t wrong = atomic_load(&g_wrong);
    printf("%s n=%llu: %llu rounds where the %s survived a same-value store\n",
           ppu_reserves ? "aba_ppu" : "aba_spu", (unsigned long long)n,
           (unsigned long long)wrong, ppu_reserves ? "PPU reservation" : "SPU reservation");
    return wrong ? 1 : 0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "contract";
    const uint64_t n = argc > 2 ? strtoull(argv[2], NULL, 10) : 100000;
    if (!strcmp(mode, "contract")) return contract();
    if (!strcmp(mode, "jq2"))      return jq2(n);
    if (!strcmp(mode, "aba_ppu"))  return aba(1, n);
    if (!strcmp(mode, "aba_spu"))  return aba(0, n);
    fprintf(stderr, "usage: test_reservation contract | jq2 N | aba_ppu N | aba_spu N\n");
    return 2;
}
