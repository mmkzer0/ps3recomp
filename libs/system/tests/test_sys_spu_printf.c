/* _sys_spu_printf_* (sysPrxForUser, F6): initialize stores four guest OPDs;
 * attach/detach_group/thread dispatch to exactly the matching one through
 * ppu_guest_call and return its result; with no callback registered (never
 * initialised, or after finalize) they return CELL_ESTAT and call nothing.
 *
 * Two ways in. Direct C calls check the store/dispatch/finalize logic and the
 * lock under threads. The generated NID table + the real ppu_hle.cpp
 * (ps3_hle_call) check what a title sees: the handler's declared type is the
 * firmware's s32, so the typed adapter (runtime/ppu/ppu_hle_adapt.hpp)
 * sign-extends the result into guest r3 (0xFFFFFFFF8001000F for CELL_ESTAT),
 * not zero-extended.
 *
 * Built and run by tools/test_sys_spu_printf.py. ppu_guest_call is faked: it
 * records (opd, args) and returns a value the test chooses. */
#include "../sys_spu_printf.c"
#include "ppu_context.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#error "POSIX fixture"
#endif

#define OPD_AG 0x00A00010u
#define OPD_DG 0x00A00020u
#define OPD_AT 0x00A00030u
#define OPD_DT 0x00A00040u

static pthread_mutex_t g_rec_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_calls;
static u32 g_last_opd;
static u64 g_last_args[8];
static u64 g_ret;
static unsigned g_per_opd[4];

u64 ppu_guest_call(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3,
                   u64 a4, u64 a5, u64 a6, u64 a7)
{
    pthread_mutex_lock(&g_rec_lock);
    g_calls++;
    g_last_opd = opd;
    g_last_args[0] = a0; g_last_args[1] = a1; g_last_args[2] = a2; g_last_args[3] = a3;
    g_last_args[4] = a4; g_last_args[5] = a5; g_last_args[6] = a6; g_last_args[7] = a7;
    if (opd == OPD_AG) g_per_opd[0]++;
    if (opd == OPD_DG) g_per_opd[1]++;
    if (opd == OPD_AT) g_per_opd[2]++;
    if (opd == OPD_DT) g_per_opd[3]++;
    u64 r = g_ret;
    pthread_mutex_unlock(&g_rec_lock);
    return r;
}

static void reset_rec(u64 ret)
{
    pthread_mutex_lock(&g_rec_lock);
    g_calls = 0; g_last_opd = 0; memset(g_last_args, 0xEE, sizeof g_last_args);
    memset(g_per_opd, 0, sizeof g_per_opd);
    g_ret = ret;
    pthread_mutex_unlock(&g_rec_lock);
}

/* One dispatch: exact result, exactly one call, to `opd`, with `id` in r3 and
 * nothing else touched. */
static void expect_dispatch(s32 (*fn)(u32), u32 id, u32 opd, u64 guest_r3, s32 want)
{
    reset_rec(guest_r3);
    assert(fn(id) == want);
    assert(g_calls == 1);
    assert(g_last_opd == opd);
    assert(g_last_args[0] == id);
    for (int i = 1; i < 8; i++) assert(g_last_args[i] == 0);
}

static void expect_unregistered(void)
{
    s32 (*const fns[4])(u32) = {
        _sys_spu_printf_attach_group, _sys_spu_printf_detach_group,
        _sys_spu_printf_attach_thread, _sys_spu_printf_detach_thread };
    for (int i = 0; i < 4; i++) {
        reset_rec(0);
        assert(fns[i](0x1234) == (s32)CELL_ESTAT);
        assert(g_calls == 0);
    }
}

#define CONC_THREADS 4
#define CONC_ITERS   20000

static void* hammer(void* arg)
{
    (void)arg;
    for (int i = 0; i < CONC_ITERS; i++) {
        assert(_sys_spu_printf_attach_group((u32)i) == 7);
        assert(_sys_spu_printf_detach_thread((u32)i) == 7);
    }
    return NULL;
}

static void* churn(void* arg)
{
    atomic_int* stop = arg;
    while (!atomic_load_explicit(stop, memory_order_acquire))   /* replacing the same OPDs must never expose a torn/zero set */
        assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
    return NULL;
}

/* ---- the real HLE bridge (runtime/ppu/ppu_hle.cpp) and its collaborators ----
 * ppu_hle.cpp is linked in as is; everything it reaches for beyond the generated
 * table (tools/test_sys_spu_printf.py builds it) is a no-op here. */
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void* g_ps3_guest_caller;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea;
uint32_t prx_resolve_export(uint32_t nid) { (void)nid; return 0; }
void ps3_indirect_call(ppu_context* ctx) { (void)ctx; abort(); }
void ppu_prof_stamp(void* ctx, unsigned pc) { (void)ctx; (void)pc; }
void ppu_gcm_pump(void) {}
uint32_t ppu_prof_resolve_host(void* ra) { (void)ra; return 0; }
void ps3_msf(const char* fmt, ...) { (void)fmt; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea, uint64_t v, int w) { (void)ea; (void)v; (void)w; }
uint32_t vm_read32(uint64_t a) { uint32_t x; memcpy(&x, vm_base + a, 4); return __builtin_bswap32(x); }
void vm_write32(uint64_t a, uint32_t x) { x = __builtin_bswap32(x); memcpy(vm_base + a, &x, 4); }
uint64_t vm_read64(uint64_t a) { uint64_t x; memcpy(&x, vm_base + a, 8); return __builtin_bswap64(x); }
/* ppu_hle.cpp declares vm_write64 with C++ linkage: tools/test_sys_spu_printf.py links a one-line
 * C++ shim that forwards it here. */
void ps3_test_vm_write64(uint64_t a, uint64_t x) { x = __builtin_bswap64(x); memcpy(vm_base + a, &x, 8); }
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
void ppu_dump_bctrl_ring(uint32_t tid, const char* tag) { (void)tid; (void)tag; }
void ppu_guest_callstack(const char* tag) { (void)tag; }

void ppu_hle_register_all(void);   /* generated: gen_hle_nids.py sys_spu_printf */
int ps3_hle_has(uint32_t nid);
void ps3_hle_call(uint32_t nid, ppu_context* ctx);

/* Dispatch `nid` exactly as a title's import stub does and return the whole
 * 64-bit guest r3. The argument registers past r3 carry noise: the callback
 * must see only the id. */
static uint64_t hle_r3(u32 nid, u32 id)
{
    static uint8_t arena[0x10000];
    vm_base = arena;
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[1] = 0x8000;
    ctx.gpr[2] = 0x12345678;
    ctx.gpr[3] = 0xBADBADBA00000000ull | id;   /* a u32 parameter: only the low word is the argument */
    for (int r = 4; r <= 10; r++) ctx.gpr[r] = 0xDEAD000000000000ull | (u64)r;
    ps3_hle_call(nid, &ctx);
    assert(ctx.gpr[2] == 0x12345678);
    return ctx.gpr[3];
}

static void hle_nid_path(void)
{
    static const struct { u32 nid; u32 cb; } nids[4] = {
        { 0xDD0C1E09u, OPD_AG },   /* _sys_spu_printf_attach_group */
        { 0x5FDFB2FEu, OPD_DG },   /* _sys_spu_printf_detach_group */
        { 0x1AE10B92u, OPD_AT },   /* _sys_spu_printf_attach_thread */
        { 0xB3BBCF2Au, OPD_DT },   /* _sys_spu_printf_detach_thread */
    };
    ppu_hle_register_all();
    for (int i = 0; i < 4; i++) assert(ps3_hle_has(nids[i].nid));
    assert(ps3_hle_has(0x45FE2FCEu) && ps3_hle_has(0xDD3B27ACu));   /* initialize, finalize */

    assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
    for (int i = 0; i < 4; i++) {
        /* an s32 error (CELL_EINVAL-style) is sign-extended: the guest tests r3 as a 64-bit value */
        reset_rec(0x80010005u);
        assert(hle_r3(nids[i].nid, 0x77u + (u32)i) == 0xFFFFFFFF80010005ull);
        assert(g_calls == 1 && g_last_opd == nids[i].cb && g_last_args[0] == 0x77u + (u32)i);
        for (int r = 1; r < 8; r++) assert(g_last_args[r] == 0);
        /* the callback's own r3 upper half is not part of an s32 result */
        reset_rec(0xDEADBEEF80010005ull);
        assert(hle_r3(nids[i].nid, 1) == 0xFFFFFFFF80010005ull);
        reset_rec(0xDEADBEEF00000003ull);
        assert(hle_r3(nids[i].nid, 1) == 3);
        reset_rec(0xFFFFFFFFull);
        assert(hle_r3(nids[i].nid, 1) == 0xFFFFFFFFFFFFFFFFull);
        reset_rec(0x7FFFFFFFull);
        assert(hle_r3(nids[i].nid, 1) == 0x7FFFFFFFull);
        reset_rec(0);
        assert(hle_r3(nids[i].nid, 1) == 0);
    }

    /* No callback registered: CELL_ESTAT, sign-extended, nothing called. */
    assert(_sys_spu_printf_finalize() == CELL_OK);
    for (int i = 0; i < 4; i++) {
        reset_rec(0);
        assert(hle_r3(nids[i].nid, 0x1234) == 0xFFFFFFFF8001000Full);
        assert(g_calls == 0);
    }
    assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
}

int main(void)
{
    s32 (*const fns[4])(u32) = {
        _sys_spu_printf_attach_group, _sys_spu_printf_detach_group,
        _sys_spu_printf_attach_thread, _sys_spu_printf_detach_thread };
    const u32 opds[4] = { OPD_AG, OPD_DG, OPD_AT, OPD_DT };

    /* Before initialize: documented error, nothing invoked. */
    assert(CELL_ESTAT == 0x8001000Fu);
    expect_unregistered();

    /* initialize stores and returns CELL_OK without calling any callback. */
    reset_rec(0);
    assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
    assert(g_calls == 0);

    /* Each function reaches exactly its own OPD, with the id, result unchanged. */
    for (int i = 0; i < 4; i++) {
        expect_dispatch(fns[i], 0x00000042u + (u32)i, opds[i], 0, CELL_OK);
        expect_dispatch(fns[i], 0xFFFFFFFFu, opds[i], 0x80010005u, (s32)0x80010005u);
        expect_dispatch(fns[i], 7, opds[i], 12345, 12345);
        /* r3 above the low word is not part of an s32 result */
        expect_dispatch(fns[i], 9, opds[i], 0xDEADBEEF00000000ull | 3u, 3);
    }

    /* A callback registered NULL leaves only that entry unregistered. */
    assert(_sys_spu_printf_initialize(OPD_AG, 0, OPD_AT, 0) == CELL_OK);
    expect_dispatch(_sys_spu_printf_attach_group, 1, OPD_AG, 0, CELL_OK);
    expect_dispatch(_sys_spu_printf_attach_thread, 2, OPD_AT, 0, CELL_OK);
    reset_rec(0);
    assert(_sys_spu_printf_detach_group(3) == (s32)CELL_ESTAT);
    assert(_sys_spu_printf_detach_thread(4) == (s32)CELL_ESTAT);
    assert(g_calls == 0);

    /* A second initialize replaces the whole set. */
    assert(_sys_spu_printf_initialize(OPD_DT, OPD_AT, OPD_DG, OPD_AG) == CELL_OK);
    expect_dispatch(_sys_spu_printf_attach_group, 5, OPD_DT, 0, CELL_OK);
    expect_dispatch(_sys_spu_printf_detach_group, 5, OPD_AT, 0, CELL_OK);
    expect_dispatch(_sys_spu_printf_attach_thread, 5, OPD_DG, 0, CELL_OK);
    expect_dispatch(_sys_spu_printf_detach_thread, 5, OPD_AG, 0, CELL_OK);

    /* finalize clears state: back to the documented error, nothing invoked. */
    assert(_sys_spu_printf_finalize() == CELL_OK);
    expect_unregistered();
    assert(_sys_spu_printf_finalize() == CELL_OK);   /* idempotent */
    expect_unregistered();

    /* Re-initialisable after finalize. */
    assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
    expect_dispatch(_sys_spu_printf_attach_group, 8, OPD_AG, 0, CELL_OK);

    /* Concurrency: process-global state under guest threads. Every call must
     * resolve to the registered OPD and reach it exactly once. */
    reset_rec(7);
    atomic_int stop;
    atomic_init(&stop, 0);
    pthread_t workers[CONC_THREADS], churner;
    assert(pthread_create(&churner, NULL, churn, (void*)&stop) == 0);
    for (int i = 0; i < CONC_THREADS; i++)
        assert(pthread_create(&workers[i], NULL, hammer, NULL) == 0);
    for (int i = 0; i < CONC_THREADS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    atomic_store_explicit(&stop, 1, memory_order_release);
    assert(pthread_join(churner, NULL) == 0);
    assert(g_calls == 2u * CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[0] == (unsigned)CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[3] == (unsigned)CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[1] == 0 && g_per_opd[2] == 0);

    hle_nid_path();

    puts("PASS: _sys_spu_printf_* store, dispatch, ESTAT-when-unregistered, finalize, threads, "
         "64-bit sign-extended r3 through the NID table");
    return 0;
}
