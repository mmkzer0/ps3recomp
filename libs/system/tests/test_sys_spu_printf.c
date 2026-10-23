/* _sys_spu_printf_* (sysPrxForUser, F6): initialize stores four guest OPDs;
 * attach/detach_group/thread dispatch to exactly the matching one through
 * ppu_guest_call and return its result; with no callback registered (never
 * initialised, or after finalize) they return CELL_ESTAT and call nothing.
 *
 * Built and run by tools/test_sys_spu_printf.py. ppu_guest_call is faked: it
 * records (opd, args) and returns a value the test chooses. */
#include "../sys_spu_printf.c"
#include <assert.h>
#include <stdio.h>
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
    volatile int* stop = arg;
    while (!*stop)   /* replacing the same OPDs must never expose a torn/zero set */
        assert(_sys_spu_printf_initialize(OPD_AG, OPD_DG, OPD_AT, OPD_DT) == CELL_OK);
    return NULL;
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
    volatile int stop = 0;
    pthread_t workers[CONC_THREADS], churner;
    assert(pthread_create(&churner, NULL, churn, (void*)&stop) == 0);
    for (int i = 0; i < CONC_THREADS; i++)
        assert(pthread_create(&workers[i], NULL, hammer, NULL) == 0);
    for (int i = 0; i < CONC_THREADS; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    stop = 1;
    assert(pthread_join(churner, NULL) == 0);
    assert(g_calls == 2u * CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[0] == (unsigned)CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[3] == (unsigned)CONC_THREADS * CONC_ITERS);
    assert(g_per_opd[1] == 0 && g_per_opd[2] == 0);

    puts("PASS: _sys_spu_printf_* store, dispatch, ESTAT-when-unregistered, finalize, threads");
    return 0;
}
