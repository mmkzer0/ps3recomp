/*
 * ps3recomp - the spu_job_replay executable (docs/SPU_XDIFF.md section 13).
 *
 * spu_job_replay_main is a library entry: lifted bodies are a title's code, so a replay of a lifted subject needs
 * the executable to be linked with the registry of the images it can replay. This one has none. It runs the
 * interpreter (`--tier interp`) and answers not-found / no-body for `--tier registered`; a title links its own
 * main with its lifted registry and calls spu_job_replay_main the same way.
 *
 * The definitions below are what the channel and SPURS runtime sources reference and a replay never reaches: a
 * served context touches no guest memory (vm_base stays NULL), no clock and no peer.
 */
#include "spu_xdiff_replay.h"

#include "../spu_context.h"
#include "ps3emu/lifter_abi.h"

uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = 0;
const uint32_t ppu_lifter_abi = PPU_RUNTIME_LIFTER_ABI;   /* this program links no PPU lift */

uint32_t g_barrier_sync_watch = 0;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

int main(int argc, char** argv)
{
    return spu_job_replay_main(argc, argv);
}
