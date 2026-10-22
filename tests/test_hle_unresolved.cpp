/* Calls ps3_hle_call with a NID nothing implements and reports whether it came
 * back (tools/test_hle_unresolved.py). It must not: the unresolved path used to
 * set r3 = CELL_OK and return, which this treats as the failure it is.
 *
 *   test_hle_unresolved <nid>
 */
#include "ppu_context.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x10000;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
int g_resv_store_active = 0;
void ps3_hle_call(uint32_t, ppu_context*);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) { std::abort(); }
void ppu_prof_stamp(void*, unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*, ...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ppu_resv_break_store(uint64_t) {}
void ps3_ww_report_inline(uint32_t, uint64_t, int) {}
uint32_t vm_read32(uint64_t) { return 0; }
void vm_write32(uint64_t, uint32_t) {}
uint64_t vm_read64(uint64_t) { return 0; }
void ppu_dump_guest_stack(ppu_context*, const char*) {}
void ppu_dump_bctrl_ring(uint32_t, const char*) {}
void ppu_guest_callstack(const char*) {}
}
void vm_write64(uint64_t, uint64_t) {}
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    ppu_context ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = 1;
    ctx.lr = 0x10100;
    ctx.gpr[1] = 0x8000;
    ctx.gpr[3] = 0x1234;   // an argument; an unimplemented import must not touch it by "succeeding"
    ps3_hle_call((uint32_t)std::strtoul(argv[1], nullptr, 0), &ctx);
    std::printf("RETURNED r3=0x%llX\n", (unsigned long long)ctx.gpr[3]);
    return 0;
}
