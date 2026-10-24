/* cellNetCtl argument/state checks, compared with RPCS3 cellNetCtl.cpp.
 *
 *   cc -std=gnu17 -I include -o /tmp/test_netctl_checks \
 *      libs/network/tests/test_netctl_checks.c
 */
#include "../cellNetCtl.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
const char* np_psnr_identity(void) { return 0; }
void cellSysutilQueueEvent(int slot, u32 status, u32 param)
{ (void)slot; (void)status; (void)param; }

int main(void)
{
#ifdef _WIN32
    _putenv_s("PS3_NET_ONLINE", "");
#else
    unsetenv("PS3_NET_ONLINE");
#endif
    vm_base = calloc(1, 65536);
    assert(vm_base);

    /* RPCS3 cellNetCtl.cpp: not initialized -> NOT_INITIALIZED. */
    assert(cellNetCtlGetState((s32*)(uintptr_t)0x100) ==
           CELL_NET_CTL_ERROR_NOT_INITIALIZED);
    assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_DEVICE,
                             (CellNetCtlInfo*)(uintptr_t)0x100) ==
           CELL_NET_CTL_ERROR_NOT_INITIALIZED);

    /* RPCS3 cellNetCtlInit: re-init -> NOT_TERMINATED. */
    assert(cellNetCtlInit() == CELL_OK);
    assert(cellNetCtlInit() == CELL_NET_CTL_ERROR_NOT_TERMINATED);

    /* RPCS3 checks !state / !info -> INVALID_ADDR. */
    assert(cellNetCtlGetState(NULL) == CELL_NET_CTL_ERROR_INVALID_ADDR);
    assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_DEVICE, NULL) ==
           CELL_NET_CTL_ERROR_INVALID_ADDR);

    /* Offline: RPCS3 writes Disconnected and returns CELL_OK, not an error. */
    assert(cellNetCtlGetState((s32*)(uintptr_t)0x100) == CELL_OK);
    assert(vm_read32(0x100) == CELL_NET_CTL_STATE_Disconnected);
    assert(cellNetCtlGetInfo(CELL_NET_CTL_INFO_DEVICE,
                             (CellNetCtlInfo*)(uintptr_t)0x100) == CELL_OK);

    cellNetCtlTerm();
    assert(cellNetCtlGetState((s32*)(uintptr_t)0x100) ==
           CELL_NET_CTL_ERROR_NOT_INITIALIZED);

    free(vm_base);
    puts("test_netctl_checks: OK");
    return 0;
}
