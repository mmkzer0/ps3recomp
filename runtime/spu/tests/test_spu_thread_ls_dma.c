/* MFC transfers to the SPU thread local-store window (0xF0000000 +
 * spu_num * 0x100000): a GET or PUT there moves data between local stores,
 * not guest memory. Resistance 3's collision job gathers records out of a
 * local store this way, and reading guest memory instead gave it zeros. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#endif

static uint8_t guest_memory[1u << 20];
uint8_t* vm_base = guest_memory;
uint32_t ppu_vm_size = 0;

/* The unit under test. */
#include "../spu_channels.c"
#include "../spu_workload.h"

static int g_pass, g_fail;

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

#define CHECK(cond)  check((cond), #cond)

#define SPURS_EA    0x00080000u
#define PEER_LSA    0x7400u
#define LOCAL_LSA   0x22340u
#define PUT_LSA     0x8000u

static const uint8_t k_pm[16];          /* a policy module only needs bytes to load */
static const uint8_t k_spu0[16] = { 0xA0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t k_spu1[16] = { 0xB1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static spu_context* g_spu0;
static spu_context* g_other;
static int g_rc_peer_get, g_rc_self_get, g_rc_put;
static uint8_t g_got_peer[16], g_got_self[16];

/* SPU 0: leaves a record in its own local store. */
static void spu0_entry(spu_context* ctx)
{
    g_spu0 = ctx;
    memcpy(&ctx->ls[PEER_LSA], k_spu0, sizeof k_spu0);
}

/* SPU 0 of a second workload of the same instance. */
static void other_entry(spu_context* ctx) { g_other = ctx; }

/* SPU 1: reads SPU 0's record and its own through the window, writes one into SPU 0. */
static void spu1_entry(spu_context* ctx)
{
    memcpy(&ctx->ls[PEER_LSA], k_spu1, sizeof k_spu1);
    g_rc_peer_get = mfc_do_transfer(ctx, LOCAL_LSA, 0xF0000000u + PEER_LSA, 16, MFC_GET_CMD, 0);
    memcpy(g_got_peer, &ctx->ls[LOCAL_LSA], 16);
    g_rc_self_get = mfc_do_transfer(ctx, LOCAL_LSA, 0xF0100000u + PEER_LSA, 16, MFC_GET_CMD, 0);
    memcpy(g_got_self, &ctx->ls[LOCAL_LSA], 16);
    g_rc_put = mfc_do_transfer(ctx, PEER_LSA, 0xF0000000u + PUT_LSA, 16, MFC_PUT_CMD, 0);
}

#ifndef _WIN32
static int g_race_stop;
static unsigned long g_race_copies, g_race_missed, g_race_bad;

/* SPU 1 of workload 0, entered again after every removal or reload. It writes nothing: running[] is published before
 * the entry runs, so a stamp written here would be unordered with the reader's copy, and the sanitizer would report
 * the test's own stamp instead of the lifetime bugs these blocks are for. The reader accepts only the zeroes of a
 * fresh context. */
static void race_entry(spu_context* ctx) { (void)ctx; }

static void* race_reader(void* arg)
{
    static const uint8_t zero[16];
    uint8_t buf[16];
    (void)arg;
    while (!__atomic_load_n(&g_race_stop, __ATOMIC_ACQUIRE)) {
        if (spu_thread_ls_copy(g_other, 1, PEER_LSA, buf, 16, 0)) {
            __atomic_add_fetch(&g_race_copies, 1, __ATOMIC_RELEASE);
            if (memcmp(buf, zero, 16) != 0)
                __atomic_add_fetch(&g_race_bad, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_add_fetch(&g_race_missed, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}
#endif

/* ---------------------------------------------------------------------------
 * Link stubs: PPU memory hooks, diagnostics and title-specific SPURS hooks.
 * None of them is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

int main(void)
{
    printf("SPU thread local-store DMA window\n");

    spu_run_policy_module(spu0_entry, 1, k_pm, sizeof k_pm, 0, 0, SPURS_EA, 0);
    spu_run_policy_module(spu1_entry, 1, k_pm, sizeof k_pm, 0, 0, SPURS_EA, 1);

    CHECK(g_spu0 != NULL);
    CHECK(g_rc_peer_get == 0);
    CHECK(memcmp(g_got_peer, k_spu0, 16) == 0);        /* SPU 0's local store, not guest memory */
    CHECK(g_rc_self_get == 0);
    CHECK(memcmp(g_got_self, k_spu1, 16) == 0);        /* its own SPU number reaches itself */
    CHECK(g_rc_put == 0);
    CHECK(g_spu0 && memcmp(&g_spu0->ls[PUT_LSA], k_spu1, 16) == 0);
    {   /* guest memory under the window is untouched */
        static const uint8_t zero[16];
        CHECK(memcmp(guest_memory + PEER_LSA, zero, 16) == 0);
    }

    {   /* Removing a workload frees its contexts: SPU 1's "entered last" context was one of them. */
        uint8_t buf[16];
        spu_run_policy_module(other_entry, 1, k_pm, sizeof k_pm, 0, 1, SPURS_EA, 0);
        CHECK(g_other != NULL && spu_thread_ls_copy(g_other, 1, PEER_LSA, buf, 16, 0) == 1);
        CHECK(spurs_policy_release_workload(SPURS_EA, 0) == 0);
        CHECK(spu_thread_ls_copy(g_other, 1, PEER_LSA, buf, 16, 0) == 0);   /* not the freed context */
        CHECK(spu_thread_ls_copy(g_other, 0, PEER_LSA, buf, 16, 0) == 1);   /* its own SPU still reaches itself */
        CHECK(spu_thread_ls_copy(g_other, 6, PEER_LSA, buf, 16, 0) == 0);   /* no such SPU */
        CHECK(spu_thread_ls_copy(g_other, 0, SPU_LS_SIZE - 8, buf, 16, 0) == 0);   /* past the local store */
    }

#ifndef _WIN32
    {   /* The reader copies out of SPU 1's local store while the workload that owns it is removed and re-created:
         * every copy either finds no context or reads a live one (the zeroes of a fresh context). The race is only
         * visible to ThreadSanitizer (the tsan preset sets SANITIZERS=thread for this test): the copy against the
         * free in spurs_policy_release_workload. */
        pthread_t reader;
        g_race_stop = 0;
        spu_run_policy_module(race_entry, 1, k_pm, sizeof k_pm, 0, 0, SPURS_EA, 1);
        CHECK(pthread_create(&reader, NULL, race_reader, NULL) == 0);
        while (!__atomic_load_n(&g_race_copies, __ATOMIC_ACQUIRE)) sched_yield();   /* the reader is live */
        for (int i = 0; i < 400; i++) {
            CHECK(spurs_policy_release_workload(SPURS_EA, 0) == 0);
            spu_run_policy_module(race_entry, 1, k_pm, sizeof k_pm, 0, 0, SPURS_EA, 1);
        }
        __atomic_store_n(&g_race_stop, 1, __ATOMIC_RELEASE);
        pthread_join(reader, NULL);
        CHECK(g_race_bad == 0);
        CHECK(spurs_policy_release_workload(SPURS_EA, 0) == 0);
    }
    {   /* A policy-module reload (the workload's data changed) keeps SPU 1's local store in the window: the reader
         * always finds a context (withdrawing it made the copy miss, and the transfer fell through to guest memory),
         * and never one that is being zeroed (TSan: the copy against the reload's memset). */
        pthread_t reader;
        g_race_stop = 0;
        g_race_copies = g_race_missed = g_race_bad = 0;   /* the first reader is joined */
        spu_run_policy_module(race_entry, 1, k_pm, sizeof k_pm, 0, 0, SPURS_EA, 1);
        CHECK(pthread_create(&reader, NULL, race_reader, NULL) == 0);
        while (!__atomic_load_n(&g_race_copies, __ATOMIC_ACQUIRE)) sched_yield();
        for (uint64_t data = 1; data <= 400; data++)   /* new workload data on every run: each one reloads */
            spu_run_policy_module(race_entry, 1, k_pm, sizeof k_pm, data, 0, SPURS_EA, 1);
        __atomic_store_n(&g_race_stop, 1, __ATOMIC_RELEASE);
        pthread_join(reader, NULL);
        CHECK(g_race_missed == 0);
        CHECK(g_race_bad == 0);
        CHECK(spurs_policy_release_workload(SPURS_EA, 0) == 0);
    }
#endif

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
