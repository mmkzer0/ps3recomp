/* Inert host-side symbols for the legacy integration fixtures (run_tests.sh).
 *
 * The fixtures stub the SPU channel and dispatch entry points themselves and
 * link this file plus the real runtime once. These definitions are the symbols
 * the runtime's inline DMA header and the SPU modules reference but that belong
 * to another layer: the PPU loader (ppu_vm_size, the diagnostic globals), the
 * game-specific SPURS HLE modules (spurs_*), and the host clock. None of them
 * decides anything here -- each test asserts on the values it produced itself.
 */
#include <stdint.h>
#include <time.h>

#include "spu_context.h"

/* runtime/ppu/ppu_loader.cpp and runtime/host/host_stubs.c */
uint32_t ppu_vm_size = 0;
uint32_t g_barrier_sync_watch = 0;
uint32_t g_spu_image_src_ea = 0, g_spu_image_ls_start = 0, g_spu_image_span = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width)
{
    (void)addr; (void)val; (void)width;
}

/* libs/spurs (only reached by the SPURS paths these fixtures do not drive) */
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t task_id,
                                uint32_t spu_num, uint32_t dma_tag_id)
{
    (void)ls; (void)taskset_ea; (void)task_id; (void)spu_num; (void)dma_tag_id;
    return 0;
}

/* libs/video/cellGcmSys.c -- the DMA-repeat guard times its back-off */
unsigned long long ps3_ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
