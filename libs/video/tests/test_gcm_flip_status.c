/* clang -std=gnu17 -I include -I libs/video
 *   libs/video/tests/test_gcm_flip_status.c -Wl,-dead_strip -pthread
 *   -o /tmp/test_gcm_flip_status && /tmp/test_gcm_flip_status
 *
 * The flip status against a flip completion that is delivered late: the
 * ticker marks a completion pending, but the flip handler only runs when a
 * guest thread next pumps, and the title may have requested another flip by
 * then. That newer request must stay WAITING until a tick of its own
 * completes it.
 * A beat completes only a flip the RSX has executed (upstream b00e0e2a): each flip
 * here is executed before the tick that is to complete it. (Upstream's last block,
 * cellGcmSetWaitFlip giving up after a CPU-side bound, is not carried: here SetWaitFlip
 * enqueues the NV406E acquire and never waits on the CPU, #77.)
 */
#include "../cellGcmSys.c"
#include "gcm_fixture_stubs.h"   /* ps3_config_get and the named stops cellGcmSys.c ends in */
#include "../rsx_tiling.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
/* What the drain does when it reaches a flip queued in the command stream. */
static void flip_executed(void) { s_fifo_flips_reached++; }
static unsigned flips;
static void callback(uint32_t opd, uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t h, uint64_t i)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)h; (void)i;
    if (opd == 0x400) flips++;
}
int main(void)
{
    g_ps3_guest_caller = callback;
    s_flip_handler_opd = 0x400;

    /* The plain case: request, execute, tick, deliver. */
    cellGcmResetFlipStatus();
    assert(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 0 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    flip_executed();
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);
    /* No flip outstanding: a tick completes nothing and calls no handler. */
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 1);

    /* A newer request between the tick and the delivery: the late completion
     * runs the handler for the old flip but leaves the new one WAITING. */
    cellGcmResetFlipStatus();
    flip_executed();
    cellGcmTickFlip();
    cellGcmResetFlipStatus();
    ppu_gcm_pump();
    assert(flips == 2 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING);
    flip_executed();
    cellGcmTickFlip();
    ppu_gcm_pump();
    assert(flips == 3 && cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_DONE);

    puts("GCM flip status checks passed");
}
