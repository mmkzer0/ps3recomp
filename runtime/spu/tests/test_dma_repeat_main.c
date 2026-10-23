/* The identical-DMA guard is timing and an opt-in named stop, never a silent
 * halt (docs/STOPS.md, kind spu-dma-repeat).
 *
 * Drives the real mfc_submit() with the same GET over and over:
 *
 *   test_dma_repeat_main           300 repeats: no stop, no halt (default
 *                                  spu.dma_repeat_stop_secs = 0)
 *   test_dma_repeat_main stop      repeats until spu.dma_repeat_stop_secs (set
 *                                  by tools/test_spu_dma_repeat.py through the
 *                                  environment) ends the process with exit 70
 *                                  and one PS3STOP kind=spu-dma-repeat line.
 *
 * The bounded loop matters: if the stop regresses into a halt or into "keep
 * going forever", this fixture exits non-zero instead of hanging.
 */
#include "spu_dma.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Backing "main memory" for the GET. One MB is plenty: every transfer targets
 * EA 0x200 and the guard keys on (ea, lsa), not on the data. */
static uint8_t g_mem[1u << 20];
uint8_t* vm_base = g_mem;

/* spu_dma.h's PUT/SDCRZ paths and its diagnostics reference these in the full
 * runtime. A GET never reaches the coherence hooks, but the linker needs them. */
int g_cri_video_dma = 0;
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

/* More state the runtime keeps for the diagnostics around mfc_submit. A GET of
 * 16 bytes never inspects any of it; every switch that would is off (the test
 * strips the SPU_ and YDKJ_ environment). */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_taskset_ea;
SPU_THREAD_LOCAL volatile unsigned g_wws_batch_gets;
void spu_overlay_note_get(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size)
{
    (void)ctx; (void)ea; (void)ls; (void)size;
}
unsigned* spu_ls_watch_list(int* out_n)
{
    if (out_n) *out_n = 0;
    return NULL;
}

/* The real clock: the guard measures wall time, so a fake clock would not
 * exercise the thing under test. */
unsigned long long ps3_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

int main(int argc, char** argv)
{
    const int stop_expected = argc > 1 && strcmp(argv[1], "stop") == 0;
    spu_context ctx;
    spu_context_init(&ctx, 1);
    ctx.image_id = 7;
    ctx.pc = 0x1234;
    ctx.mfc_lsa = 0x100;
    ctx.mfc_eah = 0;
    ctx.mfc_eal = 0x200;
    ctx.mfc_size = 16;
    ctx.mfc_tag = 0;

    mfc_engine mfc;
    mfc_engine_init(&mfc);
    memset(g_mem, 0, sizeof g_mem);

    /* 3000 sleeps of 1 ms is a 3 s bound; the stop must arrive after ~1 s. */
    const unsigned long max = stop_expected ? 3000ul : 300ul;
    unsigned long n;
    for (n = 0; n < max; ++n) {
        const int rc = mfc_submit(&mfc, &ctx, MFC_GET_CMD);
        if (rc != 0) {
            fprintf(stderr, "FAIL: mfc_submit returned %d at repeat %lu\n", rc, n);
            return 1;
        }
    }
    if (stop_expected) {
        fprintf(stderr, "FAIL: no spu-dma-repeat stop after %lu identical transfers\n", n);
        return 3;
    }
    printf("PASS: %lu identical transfers, no stop, no halt\n", n);
    return 0;
}
