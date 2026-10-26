/* clang -std=gnu17 -I include libs/system/tests/test_game_paths.c
 * -Wl,-dead_strip -o /tmp/test_game_paths && /tmp/test_game_paths */
#include "../cellGame.c"
#include <assert.h>
#include <unistd.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
/* No stop is on this fixture's path (test_game_host_paths.c covers them). */
void ps3_stop(ps3_stop_kind k, const char* w, const char* r, const ps3_stop_field* f,
              size_t n, const ps3_stop_site* s)
{ (void)k; (void)w; (void)r; (void)f; (void)n; (void)s; abort(); }
void ps3_stop_guest_range(const char* f, const char* a, uint64_t ea, uint64_t size,
                          uint64_t limit)
{ (void)f; (void)a; (void)ea; (void)size; (void)limit; abort(); }

ps3_guest_caller_fn g_ps3_guest_caller;
const char* ppu_vfs_root = ".";

int main(void)
{
    char root[] = "/tmp/ps3recomp-game-paths-XXXXXX";
    assert(mkdtemp(root));
    vm_base = calloc(1, 65536); assert(vm_base);
    cellGame_set_content_path(root);
    memcpy(vm_base + 0x100, "TEST00000", 10);
    assert(cellGameDataCheck(3, (void*)0x100, (void*)0x200) == CELL_GAME_RET_NONE);
    assert(cellGameCreateGameData((void*)0x300, (void*)0x400, (void*)0x500) == CELL_OK);
    assert(strcmp((char*)vm_base + 0x400, "/dev_hdd0/game/TEST00000") == 0);
    assert(strcmp((char*)vm_base + 0x500, "/dev_hdd0/game/TEST00000/USRDIR") == 0);
    assert(cellGameDataCheck(3, (void*)0x100, NULL) == CELL_OK);
    char host[256];
    snprintf(host, sizeof(host), "%s/TEST00000/USRDIR", root);
    assert(dir_exists(host)); assert(rmdir(host) == 0);
    snprintf(host, sizeof(host), "%s/TEST00000", root);
    assert(rmdir(host) == 0); assert(rmdir(root) == 0);
    /* RPCS3 cellGame.cpp cellGameGetParamString: !buf || bufsize == 0 ->
     * CELL_GAME_ERROR_PARAM. */
    assert(cellGameGetParamString(CELL_GAME_PARAMID_TITLE_ID, NULL, 10) ==
           CELL_GAME_ERROR_PARAM);
    assert(cellGameGetParamString(CELL_GAME_PARAMID_TITLE_ID, (void*)0x600, 0) ==
           CELL_GAME_ERROR_PARAM);
    free(vm_base);
    puts("Game creation returns guest paths and creates host directories");
}
