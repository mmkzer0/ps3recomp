/*
 * ps3recomp - tests/resc/test_flipstatus.c
 *
 * cellRescResetFlipStatus / cellRescGetFlipStatus / cellRescSetFlipHandler / cellRescSetVBlankHandler /
 * cellRescGetLastFlipTime (libresc FW 4.93: 02004534, 020044D0, 02003704, 02003794, 02004590; exports
 * 02000438, 0200040C, 020002E8, 02000314, 0200045C).
 *
 * Every configuration but 720x576 with a PAL temporal mode (stored mode +0xD0 == 2, +0x10 not 0 or 5) is a
 * plain call to the libgcm export: ResetFlipStatus is cellGcmResetFlipStatus, GetFlipStatus is
 * cellGcmGetFlipStatus, the handlers go to cellGcmSetFlipHandler / cellGcmSetVBlankHandler. So the status
 * is WAITING from the reset until the driver completes a flip the RSX has executed, and neither a poll
 * nor cellRescSetConvertAndFlip (tests/resc/test_convert.c) completes it. (The library used to complete it on both, which let a title
 * run frames ahead of the RSX it queues work for.) The PAL configuration keeps a flag in the library
 * object that is not modelled: a named stop.
 *
 * Synthetic configuration, nothing recorded.
 *
 *   ctest --test-dir build/dev -R resc_flipstatus --output-on-failure
 */
#include "cellResc.h"
#include "cellGcmSys.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);
extern void cellGcm_rsx_process_fifo(void);
extern void cellGcmTickVBlank(void);
extern void cellGcmTickFlip(void);
extern void ppu_gcm_pump(void);

uint8_t* vm_base = NULL;

#define CFG_EA     0x2000u
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00400000u
#define HEAP       0x00800000u

static int g_fail;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            g_fail++;                                                          \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static uint32_t gr32(uint32_t ea) { return vm_read32(ea); }
static void gw32(uint32_t ea, uint32_t v) { vm_write32(ea, v); }

static uint32_t g_heap = HEAP;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static uint32_t g_ctx, g_ctrl;

static void scene_gcm(void)
{
    cellGcmTerminate();
    u32 ctx_out = host_alloc(4, 4);
    g_ctx = cellGcmSetupContext(ctx_out, 0x10000u, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
}

static s32 scene_resc(u32 support, u32 pal, u32 flip, u32 mode)
{
    cellRescExit();
    gw32(CFG_EA + 0, 28);   gw32(CFG_EA + 4, 2);   gw32(CFG_EA + 8, support);
    gw32(CFG_EA + 12, 0);   gw32(CFG_EA + 16, pal); gw32(CFG_EA + 20, 0);
    gw32(CFG_EA + 24, flip);
    CHECK(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "Init");
    return cellRescSetDisplayMode(mode);
}

static int g_flip_calls, g_vblank_calls;
static uint64_t g_flip_arg, g_vblank_args[16];
static uint32_t g_flip_opd, g_vblank_opd;
static void fake_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                        uint64_t a6, uint64_t a7)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (opd == g_flip_opd) { g_flip_calls++; g_flip_arg = a0; }
    else if (opd == g_vblank_opd) { if (g_vblank_calls < 16) g_vblank_args[g_vblank_calls] = a0; g_vblank_calls++; }
}

/* A flip command in the FIFO, the way a frame queues one. */
static void queue_flip(void)
{
    CHECK(cellGcmSetDisplayBuffer(0, 0x100000, 0x1400, 1280, 720) == 0, "SetDisplayBuffer");
    CHECK(cellGcm_emit_flip_command(g_ctx, 0) == 0, "flip command");
    gw32(g_ctrl + 0, gr32(g_ctx + 8) - IO_ADDR);
}

static void test_720p_is_libgcm(void)
{
    CHECK(scene_resc(CELL_RESC_1280x720, 0, 0, CELL_RESC_1280x720) == 0, "720p");
    scene_gcm();
    g_flip_opd = host_alloc(8, 8);
    g_vblank_opd = host_alloc(8, 8);
    gw32(g_flip_opd, 0x00123400u);
    gw32(g_vblank_opd, 0x00123500u);
    CHECK(cellRescSetFlipHandler((void (*)(u32))(uintptr_t)g_flip_opd) == 0, "SetFlipHandler");
    CHECK(cellRescSetVBlankHandler((void (*)(u32))(uintptr_t)g_vblank_opd) == 0, "SetVBlankHandler");
    g_ps3_guest_caller = fake_caller;

    cellRescResetFlipStatus();
    CHECK(cellGcmGetFlipStatus() == 1u, "ResetFlipStatus is cellGcmResetFlipStatus: libgcm says %u, want WAITING (1)",
          cellGcmGetFlipStatus());
    for (int i = 0; i < 5; i++)
        CHECK(cellRescGetFlipStatus() == 1, "poll %d completed the flip (status %d)", i, cellRescGetFlipStatus());
    CHECK(g_flip_calls == 0, "a poll called the flip handler");

    queue_flip();
    /* cellGcm's flip command also calls the handler when the flip is requested (head 0); the count that
     * matters is what the completion adds. */
    const int at_request = g_flip_calls;
    for (int i = 0; i < 4; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD) >= 1, "the flip head retired");
    CHECK(cellRescGetFlipStatus() == 1, "the RSX executed the flip but the driver has not completed it: status %d",
          cellRescGetFlipStatus());
    ppu_gcm_pump();
    CHECK(cellRescGetFlipStatus() == 1 && g_flip_calls == at_request, "delivery without a vblank tick: status %d, handler x%d, was x%d",
          cellRescGetFlipStatus(), g_flip_calls, at_request);

    cellGcmTickVBlank();
    cellGcmTickFlip();
    ppu_gcm_pump();
    CHECK(cellRescGetFlipStatus() == 0, "the flip completed at the vblank: status %d, want DONE (0)", cellRescGetFlipStatus());
    CHECK(g_flip_calls == at_request + 1 && g_flip_arg == 1u, "flip handler x%d(%llu), want x%d(1) (forwarded to libgcm)",
          g_flip_calls, (unsigned long long)g_flip_arg, at_request + 1);
    CHECK(g_vblank_calls == 1, "vblank handler x%d, want x1", g_vblank_calls);

    const uint32_t tea = host_alloc(8, 8);
    gw32(tea, 0xFFFFFFFFu); gw32(tea + 4, 0xFFFFFFFFu);
    CHECK(cellRescGetLastFlipTime((u64*)(uintptr_t)tea) == 0, "GetLastFlipTime");
    const u64 t = ((u64)gr32(tea) << 32) | gr32(tea + 4);
    CHECK(t == cellGcmGetLastFlipTime(), "GetLastFlipTime is cellGcmGetLastFlipTime: 0x%llX vs 0x%llX",
          (unsigned long long)t, (unsigned long long)cellGcmGetLastFlipTime());
    g_ps3_guest_caller = NULL;
}

/* The vblank handler runs once per vblank the driver raised, not once for however many are pending: a vblank
 * thread that ran late fires its missed vblanks back to back (RPCS3's does, each one counted and each handler call
 * queued), and the guest's counter advances by the missed count. */
static void test_a_late_pump_runs_the_handler_once_per_vblank(void)
{
    CHECK(scene_resc(CELL_RESC_1280x720, 0, 0, CELL_RESC_1280x720) == 0, "720p");
    scene_gcm();
    g_vblank_opd = host_alloc(8, 8);
    gw32(g_vblank_opd, 0x00123500u);
    cellRescSetVBlankHandler((void (*)(u32))(uintptr_t)g_vblank_opd);
    g_ps3_guest_caller = fake_caller;
    ppu_gcm_pump();                       /* whatever was pending */
    g_vblank_calls = 0;

    cellGcmTickVBlank();
    ppu_gcm_pump();
    CHECK(g_vblank_calls == 1, "one vblank, one pump: handler x%d, want x1", g_vblank_calls);
    const uint64_t first = g_vblank_args[0];

    g_vblank_calls = 0;
    for (int i = 0; i < 3; i++) cellGcmTickVBlank();   /* the thread ran late: three missed vblanks back to back */
    ppu_gcm_pump();
    CHECK(g_vblank_calls == 3, "three vblanks pending at one pump: handler x%d, want x3", g_vblank_calls);
    CHECK(g_vblank_args[0] == first + 1 && g_vblank_args[1] == first + 2 && g_vblank_args[2] == first + 3,
          "the three calls carry the vblank numbers %llu %llu %llu, want %llu..%llu", (unsigned long long)g_vblank_args[0],
          (unsigned long long)g_vblank_args[1], (unsigned long long)g_vblank_args[2], (unsigned long long)first + 1,
          (unsigned long long)first + 3);

    g_vblank_calls = 0;
    ppu_gcm_pump();
    CHECK(g_vblank_calls == 0, "a second pump with nothing pending called the handler x%d", g_vblank_calls);
    g_ps3_guest_caller = NULL;
}

/* ---- the PAL configuration: a named stop ------------------------------------------------------------------ */

static int run_child(const char* self, const char* mode, char* out, size_t cap)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' %s 2>&1", self, mode);
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x40000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    if (argc == 2) {
        if (scene_resc(CELL_RESC_720x576, 1, 0, CELL_RESC_720x576) != 0) return 3;
        if (!strcmp(argv[1], "pal-get")) { (void)cellRescGetFlipStatus(); return 0; }
        if (!strcmp(argv[1], "pal-reset")) { cellRescResetFlipStatus(); return 0; }
        if (!strcmp(argv[1], "pal-flip-handler")) { (void)cellRescSetFlipHandler(NULL); return 0; }
        if (!strcmp(argv[1], "pal-vblank-handler")) { (void)cellRescSetVBlankHandler(NULL); return 0; }
        if (!strcmp(argv[1], "pal-last-flip-time")) { (void)cellRescGetLastFlipTime((u64*)(uintptr_t)host_alloc(8, 8)); return 0; }
        return 2;
    }

    test_720p_is_libgcm();
    test_a_late_pump_runs_the_handler_once_per_vblank();

    static const char* const modes[] = { "pal-get", "pal-reset", "pal-flip-handler", "pal-vblank-handler",
                                         "pal-last-flip-time" };
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        char out[8192];
        int rc = run_child(argv[0], modes[i], out, sizeof out);
        CHECK(rc == 70, "%s: exit status %d, want 70", modes[i], rc);
        CHECK(strstr(out, "PS3STOP v=1 kind=gcm-unmodelled") != NULL && strstr(out, "pal-flip-flag") != NULL,
              "%s: no gcm-unmodelled stop naming pal-flip-flag in '%s'", modes[i], out);
    }

    /* A PAL temporal mode of 5 (and 0) is not the library-flag configuration. */
    CHECK(scene_resc(CELL_RESC_720x576, 5, 1, CELL_RESC_720x576) == 0, "576 pal 5");
    scene_gcm();
    cellRescResetFlipStatus();
    CHECK(cellRescGetFlipStatus() == 1, "576 with PAL temporal mode 5 is libgcm's status");
    CHECK(scene_resc(CELL_RESC_720x576, 0, 0, CELL_RESC_720x576) == 0, "576 pal 0");
    CHECK(cellRescGetFlipStatus() == 1, "576 with PAL temporal mode 0 is libgcm's status");

    printf("[resc_flipstatus] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
