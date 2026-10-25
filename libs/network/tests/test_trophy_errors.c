/* clang -std=gnu17 -I include libs/network/tests/test_trophy_errors.c
 * libs/system/cellSysutil.c -Wl,-dead_strip -pthread -o /tmp/test_trophy_errors
 *
 * The codes a title sees from sceNpTrophy are the firmware's. FW 4.93
 * libsysutil_np_trophy confirms 0x80022901 (already initialized), 0x02 (not
 * initialized), 0x03 (not supported), 0x06 (invalid argument), 0x0A (unknown
 * context), 0x0F (processing aborted) and 0x11 (unknown handle); RPCS3 (pinned
 * e53ac18f) gives the others. The literals below are the guest-visible
 * contract: a title compares against them (or against its SDK header, which
 * holds the same numbers), so the test checks them through the API.
 */
#undef NDEBUG
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../sceNpTrophy.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void cellMsgDialog_tick(void) {}
void np_psnr_pump(void) {}
void ps3_stop(ps3_stop_kind k, const char* w, const char* r,
              const ps3_stop_field* f, size_t n, const ps3_stop_site* s)
{ (void)k; (void)w; (void)r; (void)f; (void)n; (void)s; abort(); }
const ps3_config* ps3_config_get(void) { static ps3_config cfg; return &cfg; }

#define E(n) ((s32)(0x800229u << 8 | (n)))

u32 cellGcmGetVBlankCount(void) { return 0; }
void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset,
                               uint32_t value, uint32_t current, uint32_t waited_ms)
{ (void)func; (void)what; (void)offset; (void)value; (void)current; (void)waited_ms; abort(); }
u64 ppu_guest_call(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{ (void)opd; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; abort(); }

int main(void)
{
    char root[] = "/tmp/ps3recomp-trophy-err-XXXXXX";
    assert(mkdtemp(root));
    vm_base = calloc(1, 65536); assert(vm_base);
    strcpy((char*)vm_base + 0x400, root);
    sceNpTrophySetStoragePath((void*)0x400);
    memcpy(vm_base + 0x100, "NPWR00001", 10);

    /* Before init. */
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x108, 0) == E(0x02));
    assert(sceNpTrophyTerm() == E(0x02));
    assert(sceNpTrophyCreateHandle((void*)0x204) == E(0x02));
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0, 0) == E(0x02));

    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == E(0x01));

    /* Arguments. */
    assert(sceNpTrophyCreateContext(NULL, (void*)0x100, (void*)0x108, 0) == E(0x06));
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == E(0x06));
    assert(sceNpTrophyCreateHandle(NULL) == E(0x06));
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, (void*)0x108, 0) == CELL_OK);
    assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);

    /* A slot in range that names nothing is unknown; an id out of range is an
     * invalid argument. */
    assert(sceNpTrophyDestroyContext(3) == E(0x0A));
    assert(sceNpTrophyDestroyHandle(3) == E(0x11));
    assert(sceNpTrophyDestroyContext(0) == E(0x06));
    assert(sceNpTrophyDestroyContext(5) == E(0x06));
    assert(sceNpTrophyDestroyHandle(0) == E(0x06));
    assert(sceNpTrophyDestroyHandle(5) == E(0x06));
    assert(sceNpTrophyRegisterContext(3, 1, 0x300, 0, 0) == E(0x0A));
    assert(sceNpTrophyRegisterContext(1, 3, 0x300, 0, 0) == E(0x11));
    assert(sceNpTrophyRegisterContext(0, 1, 0x300, 0, 0) == E(0x06));
    assert(sceNpTrophyRegisterContext(1, 0, 0x300, 0, 0) == E(0x06));
    assert(sceNpTrophyRegisterContext(1, 1, 0, 0, 0) == E(0x06));
    /* Only options 0 and 1 exist. */
    assert(sceNpTrophyRegisterContext(1, 1, 0x300, 0, 2) == E(0x03));
    assert(sceNpTrophyGetRequiredDiskSpace(3, 1, (u64*)(uintptr_t)0x300, 0) == E(0x0A));
    assert(sceNpTrophyGetRequiredDiskSpace(0, 1, (u64*)(uintptr_t)0x300, 0) == E(0x06));
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, NULL, 0) == E(0x06));

    /* A context that was never registered. */
    assert(sceNpTrophyGetGameInfo(1, 1, NULL, NULL) == E(0x04));
    assert(sceNpTrophyGetTrophyInfo(1, 1, 0, NULL, NULL) == E(0x04));
    assert(sceNpTrophyUnlockTrophy(1, 1, 0, NULL) == E(0x04));
    assert(sceNpTrophyGetTrophyUnlockState(1, 1, (void*)(uintptr_t)0x300, (u32*)(uintptr_t)0x304) == E(0x04));
    assert(sceNpTrophyGetGameProgress(1, 1, (s32*)(uintptr_t)0x308) == E(0x04));
    assert(sceNpTrophyGetGameInfo(3, 1, NULL, NULL) == E(0x0A));

    /* A trophy unlocked twice. */
    s_contexts[1].registered = 1;
    assert(sceNpTrophyUnlockTrophy(1, 1, 0, NULL) == CELL_OK);
    assert(sceNpTrophyUnlockTrophy(1, 1, 0, NULL) == E(0x15));
    assert(sceNpTrophyUnlockTrophy(1, 1, SCE_NP_TROPHY_MAX_NUM_TROPHIES, NULL) == E(0x06));

    /* The inline callback's abort (the firmware's 0x8002290F) is
     * SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED. */
    assert((s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED == E(0x0F));

    assert(sceNpTrophyDestroyContext(1) == CELL_OK);
    assert(sceNpTrophyDestroyContext(1) == E(0x0A));
    assert(sceNpTrophyTerm() == CELL_OK);
    assert(sceNpTrophyTerm() == E(0x02));

    char file[256]; snprintf(file, sizeof(file), "%s/NPWR00001.json", root);
    assert(unlink(file) == 0); assert(rmdir(root) == 0); free(vm_base);
    puts("Trophy error codes are the firmware's 0x800229xx values");
    return 0;
}
