/* An MFC command/alignment error is a named stop, never a silently dropped
 * transfer: the hardware raises the error and the SPU does not run on with the
 * DMA lost. One case per process, because a stop is terminal; the Python
 * driver checks kind=spu-mfc-error and the staged command's fields.
 *
 * Two states are deliberately NOT errors and exit 0 here:
 *   zero          a zero-byte transfer moves nothing and completes its tag
 *                 (RPCS3's process_mfc_cmd runs a command only when size != 0)
 *   ack-unparked  MFC_WrListStallAck for a tag with no parked list is ignored
 *                 (RPCS3 acts only when the tag's ch_stall_mask bit is set)
 */
#include "spu_dma.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t g_mem[1u << 20];
uint8_t* vm_base = g_mem;

int g_cri_video_dma = 0;
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_coh_notify_write_except(uint32_t ea, const void* self) { (void)ea; (void)self; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockguard_check(spu_context* ctx, uint32_t ea, const uint8_t* ls, uint32_t size,
                         const char* what)
{
    (void)ctx; (void)ea; (void)ls; (void)size; (void)what;
}
void (*g_spu_put_hook)(uint32_t ea, uint32_t size) = 0;

/* State the runtime's diagnostics around mfc_submit inspect. Every diagnostic
 * that would use them is off (the driver strips the SPU_/YDKJ_ environment). */
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

/* A parked list raises the stall event and wakes the SPU; this fixture drives
 * the engine directly, so both hooks are inert. */
void* volatile g_sn_defer_ctx;
volatile unsigned g_sn_defer;
unsigned spu_sn_defer_ticks(void) { return 0; }
void spu_ch_wake(spu_context* ctx) { (void)ctx; }

unsigned long long ps3_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull
         + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static spu_context g_ctx;
static mfc_engine g_mfc;

static void begin(void)
{
    spu_context_init(&g_ctx, 2);
    g_ctx.image_id = 7;
    g_ctx.pc = 0x1234;
    g_ctx.mfc_tag = 7;
    mfc_engine_init(&g_mfc);
    memset(g_mem, 0, sizeof g_mem);
}

static int fail(const char* what, int rc)
{
    fprintf(stderr, "FAIL: %s (rc=%d)\n", what, rc);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    const char* c = argv[1];
    begin();

    if (!strcmp(c, "zero")) {
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eah = 0;
        g_ctx.mfc_eal = 0x2000;
        g_ctx.mfc_size = 0;
        int rc = mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
        if (rc || !(g_mfc.tag_completed & (1u << 7)))
            return fail("zero-byte GET must complete without moving data", rc);
        printf("PASS: zero-byte GET is a completed no-op\n");
        return 0;
    }
    if (!strcmp(c, "ack-unparked")) {
        int rc = mfc_list_stall_ack(&g_mfc, &g_ctx, 7);
        if (rc)
            return fail("an ack with no parked list must return 0", rc);
        printf("PASS: stall ack with no parked list has no side effects\n");
        return 0;
    }

    if (!strcmp(c, "size")) {
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eal = 0x2000;
        g_ctx.mfc_size = 0x8000;
        mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
    } else if (!strcmp(c, "alignment")) {
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eal = 0x2000;
        g_ctx.mfc_size = 24;
        mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
    } else if (!strcmp(c, "alignment-addr")) {
        /* Under 16 bytes the size need not be a multiple of 16, but LSA and
         * EA must have the same alignment. */
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eal = 0x2001;
        g_ctx.mfc_size = 8;
        mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
    } else if (!strcmp(c, "ls-overrun")) {
        g_ctx.mfc_lsa = 0x3FFF8;
        g_ctx.mfc_eal = 0x2008;
        g_ctx.mfc_size = 16;
        mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
    } else if (!strcmp(c, "ea-range")) {
        /* A range that wraps the 32-bit guest space: not committed memory. */
        g_ctx.mfc_lsa = 0x1008;
        g_ctx.mfc_eal = 0xFFFFFFF8u;
        g_ctx.mfc_size = 16;
        mfc_submit(&g_mfc, &g_ctx, MFC_GET_CMD);
    } else if (!strcmp(c, "null-ea")) {
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eal = 0x100;
        g_ctx.mfc_size = 16;
        mfc_submit(&g_mfc, &g_ctx, MFC_PUT_CMD);
    } else if (!strcmp(c, "list-submit")) {
        /* A list whose element is over 16 KiB: the element, not the list
         * descriptor, is the malformed transfer. */
        spu_ls_write32(&g_ctx, 0x100, 0x00007FF0u);
        spu_ls_write32(&g_ctx, 0x104, 0x2000);
        g_ctx.mfc_lsa = 0x1000;
        g_ctx.mfc_eal = 0x100;
        g_ctx.mfc_size = 8;
        mfc_submit(&g_mfc, &g_ctx, MFC_GETL_CMD);
    } else if (!strcmp(c, "list-resume")) {
        /* Park at element 0, then acknowledge: resuming element 1 must stop
         * just like an initial transfer would. */
        spu_ls_write32(&g_ctx, 0x100, 0x80000000u);
        spu_ls_write32(&g_ctx, 0x104, 0x2000);
        spu_ls_write32(&g_ctx, 0x108, 0x00007FF0u);
        spu_ls_write32(&g_ctx, 0x10C, 0x2010);
        int rc = mfc_run_list(&g_ctx, 0x100, 2, 0x1000, 0, MFC_GET_CMD, 7);
        if (rc || !(g_ctx.list_stall_mask & (1u << 7)))
            return fail("the initial list must park at its stall element", rc);
        mfc_list_stall_ack(&g_mfc, &g_ctx, 7);
    } else {
        fprintf(stderr, "unknown case %s\n", c);
        return 2;
    }

    fprintf(stderr, "FAIL: %s returned without a stop\n", c);
    return 1;
}
