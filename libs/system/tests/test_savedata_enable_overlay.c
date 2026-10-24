/*
 * cellSaveDataEnableOverlay(s32) (cellSysutil export 0xE7FA820B): returns CELL_OK for every value, as
 * the reference does. Built and run by tools/test_savedata_enable_overlay.py.
 */
#include "../cellSaveData.c"
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;

static int s_failed;
static void check(int ok, const char* what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_failed = 1;
}

int main(void)
{
    check(cellSaveDataEnableOverlay(1) == CELL_OK, "enable = 1 returns CELL_OK");
    check(cellSaveDataEnableOverlay(0) == CELL_OK, "enable = 0 returns CELL_OK");
    check(cellSaveDataEnableOverlay(-1) == CELL_OK, "enable = -1 returns CELL_OK (no validation)");
    check(cellSaveDataEnableOverlay(2) == CELL_OK, "enable = 2 returns CELL_OK (no validation)");
    return s_failed;
}
