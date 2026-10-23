/* See jq2_port_harness.h. The stubs below are what the SPU channel layer expects
 * of its host (the same set tests/reservation/test_reservation.c supplies); none
 * of them is on a path these tests drive. */
#include "jq2_port_harness.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reservation.h"
#include "spu_coherency.h"
#include "spu_context.h"

u128     spu_rdch(spu_context* ctx, uint32_t channel);
void     spu_wrch(spu_context* ctx, uint32_t channel, u128 value);

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

/* ---- guest memory ------------------------------------------------------- */
void jq2h_guest_init(uint32_t bytes)
{
    vm_base = (uint8_t*)aligned_alloc(4096, bytes);
    if (!vm_base) abort();
    memset(vm_base, 0, bytes);
}

void jq2h_guest_free(void)
{
    free(vm_base);
    vm_base = NULL;
}

uint32_t jq2h_get32(uint32_t ea) { uint32_t v; memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
void jq2h_put32(uint32_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(vm_base + ea, &v, 4); }
uint8_t jq2h_get8(uint32_t ea) { return vm_base[ea]; }
void jq2h_put8(uint32_t ea, uint8_t v) { vm_base[ea] = v; }

/* ---- PPU ---------------------------------------------------------------- */
struct jq2h_ppu { ppu_context ctx; };

jq2h_ppu* jq2h_ppu_new(void)
{
    jq2h_ppu* p = (jq2h_ppu*)calloc(1, sizeof *p);
    if (!p) abort();
    ppu_context_clear(&p->ctx);
    return p;
}
void jq2h_ppu_free(jq2h_ppu* p) { free(p); }
uint32_t jq2h_lwarx32(jq2h_ppu* p, uint32_t ea) { return resv_lwarx32(&p->ctx, ea); }
uint64_t jq2h_ldarx64(jq2h_ppu* p, uint32_t ea) { return resv_ldarx64(&p->ctx, ea); }
int jq2h_stcx32(jq2h_ppu* p, uint32_t ea, uint32_t v) { return resv_stcx32(&p->ctx, ea, v); }
int jq2h_stcx64(jq2h_ppu* p, uint32_t ea, uint64_t v) { return resv_stcx64(&p->ctx, ea, v); }

/* ---- SPU ---------------------------------------------------------------- */
#define LSA 0x1000u /* staging line in the SPU's local store */

struct jq2h_spu { spu_context* ctx; };

static void wrch(spu_context* c, uint32_t ch, uint32_t v) { spu_wrch(c, ch, spu_make_preferred_u32(v)); }

static uint32_t atomic_status(spu_context* c) { return spu_rdch(c, MFC_RdAtomicStat)._u32[0]; }

static void mfc_atomic(spu_context* c, uint32_t ea, uint32_t cmd)
{
    wrch(c, MFC_LSA, LSA); wrch(c, MFC_EAH, 0); wrch(c, MFC_EAL, ea); wrch(c, MFC_Cmd, cmd);
}

jq2h_spu* jq2h_spu_new(uint32_t id)
{
    jq2h_spu* s = (jq2h_spu*)calloc(1, sizeof *s);
    if (!s) abort();
    s->ctx = (spu_context*)calloc(1, sizeof *s->ctx); /* 256 KB LS: not on the stack */
    if (!s->ctx) abort();
    spu_context_init(s->ctx, 0);
    s->ctx->spu_id = id;
    return s;
}

void jq2h_spu_free(jq2h_spu* s)
{
    spu_coh_unregister(s->ctx);
    free(s->ctx);
    free(s);
}

void jq2h_getllar(jq2h_spu* s, uint32_t ea)
{
    mfc_atomic(s->ctx, ea, MFC_GETLLAR_CMD);
    (void)atomic_status(s->ctx);
}

int jq2h_putllc(jq2h_spu* s, uint32_t ea)
{
    mfc_atomic(s->ctx, ea, MFC_PUTLLC_CMD);
    return atomic_status(s->ctx) == 0u; /* MFC_PUTLLC_SUCCESS */
}

uint32_t jq2h_stage_get32(jq2h_spu* s, uint32_t off)
{
    uint32_t v; memcpy(&v, &s->ctx->ls[LSA + off], 4); return __builtin_bswap32(v);
}

void jq2h_stage_put32(jq2h_spu* s, uint32_t off, uint32_t v)
{
    v = __builtin_bswap32(v); memcpy(&s->ctx->ls[LSA + off], &v, 4);
}

int jq2h_lr_raised(jq2h_spu* s) { return (s->ctx->event_status & SPU_EVENT_LR) != 0; }

/* PM LS 0x3404-0x3444, word 0 only, one SPU operation per line:
 *   r90 = shli  w, 3              r87 = rotmai r90, -3     (sext29)
 *   r91 = rotmi w, -31 (bit 31)   r6  = ceqi  r87, -1      (A == -1)
 *   r86 = ai    r87, 1            r84 = ceqi  r6, 0        (A != -1)
 *   r78 = and   r91, r84          (bit 31, dropped when A == -1)
 *   r76 = selb  w, r86, 0x1FFFFFFF  (bits 28:0 from A + 1, bits 31:29 from w)
 *   r74 = selb  r76, r78 << 31, 0x80000000 (bit 31 from r78). */
uint32_t jq2h_pm_credit_word(uint32_t w)
{
    const uint32_t r90 = w << 3;
    const int32_t r87 = (int32_t)r90 >> 3;
    const uint32_t r91 = w >> 31;
    const uint32_t r6 = r87 == -1 ? 0xFFFFFFFFu : 0u;
    const uint32_t r86 = (uint32_t)(r87 + 1);
    const uint32_t r84 = r6 == 0 ? 0xFFFFFFFFu : 0u;
    const uint32_t r78 = r91 & r84;
    const uint32_t r76 = (w & ~0x1FFFFFFFu) | (r86 & 0x1FFFFFFFu);
    return (r76 & ~0x80000000u) | (r78 << 31);
}

int jq2h_spu_credit(jq2h_spu* s, uint32_t ea, uint64_t* retries)
{
    for (;;) {
        jq2h_getllar(s, ea);
        const uint32_t w = jq2h_stage_get32(s, 0);
        jq2h_stage_put32(s, 0, jq2h_pm_credit_word(w));
        if (jq2h_putllc(s, ea)) return (w >> 31) != 0 && ((int32_t)(w << 3) >> 3) == -1;
        if (retries) (*retries)++;
    }
}
