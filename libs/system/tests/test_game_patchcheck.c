/* cellGamePatchCheck (AM-11): NOTPATCH unless booted as a patch, which this
 * runtime never is; PARAM for a reserved argument or a NULL size. CELL_OK would
 * tell the title it is a patch.
 *
 * Built and run by tools/test_game_patchcheck.py. */
#include "../cellGame.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
const char* ppu_vfs_root;

int main(void)
{
    CellGameContentSize size;
    memset(&size, 0xAB, sizeof size);
    assert(cellGamePatchCheck(&size, NULL) == CELL_GAME_ERROR_NOTPATCH);
    assert(cellGamePatchCheck(&size, NULL) != CELL_OK);
    assert(cellGamePatchCheck(&size, &size) == CELL_GAME_ERROR_PARAM);
    assert(cellGamePatchCheck(NULL, NULL) == CELL_GAME_ERROR_PARAM);
    /* untouched: the size is written only on a patch boot */
    assert(((unsigned char*)&size)[0] == 0xAB);
    puts("PASS: cellGamePatchCheck answers NOTPATCH, PARAM on bad arguments");
    return 0;
}
