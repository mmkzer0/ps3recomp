/*
 * ps3recomp - tests/resc/test_waitflip.c
 *
 * cellRescSetWaitFlip(ctx) (libresc FW 4.93, body 0x020045E8, export 0x02000488, NID 0x0D3C22CE) and
 * the flip-done label it waits on.
 *
 * The firmware appends one 4-word group to the command context:
 *     0x00040064 <label offset>   NV406E SEMAPHORE_OFFSET
 *     0x00040068 0                NV406E SEMAPHORE_ACQUIRE
 * The label is 0x10 -- the flip-done label _cellGcmSetFlipCommand releases as 0xFFFFFFFF before its flip
 * head -- except for 720x576 (stored display mode 2) with a non-zero PAL temporal mode other than 5,
 * where it is 18 << 4. The group is reserved like every group (buffer-full callback with 4 words), and
 * the function does not need cellRescInit.
 *
 * The walker side (cellGcmSys.c): the RSX stalls on the ACQUIRE until the label is 0, the driver sets it
 * to 0 when it completes the flip (the vblank after the flip head retired), the frame behind the stalled
 * ACQUIRE is still presentable (the flip head executed), and a flip nobody completes is the named stop
 * gcm-wait-timeout, not a silent hang.
 *
 * Synthetic configuration, nothing recorded.
 *
 *   ctest --test-dir build/dev -R resc_waitflip --output-on-failure
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
extern int cellGcm_take_flip_pending_synced(void);
extern void cellGcmTickVBlank(void);
extern void cellGcm_vblank_signal(void);
extern void cellGcm_vblank_flip_label(void);

uint8_t* vm_base = NULL;

#define CFG_EA     0x2000u
#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00400000u
#define RING       (IO_ADDR + 0x1000u)
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

/* cellRescInit with {size 28, policy 2, support, ratio 0, pal, interlace 0, flip}, then SetDisplayMode. */
static s32 scene_resc(u32 support, u32 pal, u32 flip, u32 mode)
{
    cellRescExit();
    gw32(CFG_EA + 0, 28);   gw32(CFG_EA + 4, 2);   gw32(CFG_EA + 8, support);
    gw32(CFG_EA + 12, 0);   gw32(CFG_EA + 16, pal); gw32(CFG_EA + 20, 0);
    gw32(CFG_EA + 24, flip);
    CHECK(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "Init");
    return cellRescSetDisplayMode(mode);
}

/* One SetWaitFlip: the four words it appended, and what it returned. */
static void wait_flip(uint32_t want_label, const char* what)
{
    scene_gcm();
    const uint32_t at = gr32(g_ctx + 8);
    const s32 r = cellRescSetWaitFlip((void*)(uintptr_t)g_ctx);
    CHECK(gr32(g_ctx + 8) == at + 16u, "%s: appended %u bytes", what, gr32(g_ctx + 8) - at);
    CHECK(gr32(at + 0) == 0x00040064u && gr32(at + 4) == want_label && gr32(at + 8) == 0x00040068u &&
          gr32(at + 12) == 0u,
          "%s: words %08X %08X %08X %08X, want 00040064 %08X 00040068 00000000", what, gr32(at), gr32(at + 4),
          gr32(at + 8), gr32(at + 12), want_label);
    CHECK((uint32_t)r == g_ctx, "%s: returned 0x%X, want the context 0x%X", what, (uint32_t)r, g_ctx);
}

static void test_label_by_mode(void)
{
    CHECK(scene_resc(CELL_RESC_1280x720, 0, 0, CELL_RESC_1280x720) == 0, "720p");
    wait_flip(0x10, "720p");
    CHECK(scene_resc(CELL_RESC_1920x1080, 0, 0, CELL_RESC_1920x1080) == 0, "1080p");
    wait_flip(0x10, "1080p");
    CHECK(scene_resc(CELL_RESC_720x480, 0, 0, CELL_RESC_720x480) == 0, "480");
    wait_flip(0x10, "480");
    /* 720x576: the PAL temporal mode picks the label. */
    CHECK(scene_resc(CELL_RESC_720x576, 0, 0, CELL_RESC_720x576) == 0, "576 pal 0");
    wait_flip(0x10, "576, PAL mode 0");
    CHECK(scene_resc(CELL_RESC_720x576, 1, 0, CELL_RESC_720x576) == 0, "576 pal 1");
    wait_flip(0x120, "576, PAL mode 1");
    CHECK(scene_resc(CELL_RESC_720x576, 4, 0, CELL_RESC_720x576) == 0, "576 pal 4");
    wait_flip(0x120, "576, PAL mode 4");
    CHECK(scene_resc(CELL_RESC_720x576, 5, 1, CELL_RESC_720x576) == 0, "576 pal 5 flip 1");
    wait_flip(0x10, "576, PAL mode 5");
    /* A PAL mode with another display mode does not reach the 576 path. */
    CHECK(scene_resc(CELL_RESC_1280x720 | CELL_RESC_720x576, 1, 0, CELL_RESC_1280x720) == 0, "720p pal 1");
    wait_flip(0x10, "720p with PAL mode 1");
}

static void test_without_init(void)
{
    cellRescExit();
    wait_flip(0x10, "no cellRescInit");
}

/* The buffer-full callback: current + 16 > end calls callback(ctx, 4); the group goes where it leaves current. */
static int g_cb_calls;
static uint32_t g_cb_ctx, g_cb_words, g_seg2;
static void fake_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                        uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    g_cb_calls++;
    g_cb_ctx = (uint32_t)a0;
    g_cb_words = (uint32_t)a1;
    gw32(g_cb_ctx + 0, g_seg2);
    gw32(g_cb_ctx + 4, g_seg2 + 0x1000u);
    gw32(g_cb_ctx + 8, g_seg2);
}

static void test_callback(void)
{
    CHECK(scene_resc(CELL_RESC_1280x720, 0, 0, CELL_RESC_1280x720) == 0, "720p");
    scene_gcm();
    const uint32_t opd = host_alloc(8, 8);
    gw32(opd, 0x00123400u);      /* a title's own callback, not the runtime's */
    gw32(opd + 4, 0);
    gw32(g_ctx + 0xC, opd);
    gw32(g_ctx + 4, RING + 12u);  /* 12 bytes left: the 16-byte group does not fit */
    g_seg2 = IO_ADDR + 0x40000u;
    gw32(RING, 0);               /* earlier scenes wrote their groups here: SetupContext does not clear the ring */
    g_ps3_guest_caller = fake_caller;
    g_cb_calls = 0;
    const s32 r = cellRescSetWaitFlip((void*)(uintptr_t)g_ctx);
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 1 && g_cb_ctx == g_ctx && g_cb_words == 4u, "callback(ctx 0x%X, words %u) x%d, want (0x%X, 4) x1",
          g_cb_ctx, g_cb_words, g_cb_calls, g_ctx);
    CHECK(gr32(g_seg2) == 0x00040064u && gr32(g_seg2 + 4) == 0x10u && gr32(g_seg2 + 8) == 0x00040068u &&
          gr32(g_seg2 + 12) == 0u && gr32(g_ctx + 8) == g_seg2 + 16u, "the group lands in the segment the callback chose");
    CHECK(gr32(RING) == 0, "nothing was written in the full segment");
    CHECK(r == 0, "returned %d after a callback, want 0", r);
    /* Exactly 16 bytes left is room: no callback. */
    scene_gcm();
    gw32(g_ctx + 0xC, opd);
    gw32(g_ctx + 4, RING + 16u);
    g_ps3_guest_caller = fake_caller;
    g_cb_calls = 0;
    cellRescSetWaitFlip((void*)(uintptr_t)g_ctx);
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 0 && gr32(g_ctx + 8) == RING + 16u, "a group that exactly fits does not call the callback");
}

/* ---- through the walker ------------------------------------------------------------------------------- */

static uint32_t label_ea(u8 i) { return (uint32_t)(uintptr_t)cellGcmGetLabelAddress(i); }

static void flip_then_wait(void)
{
    CHECK(scene_resc(CELL_RESC_1280x720, 0, 0, CELL_RESC_1280x720) == 0, "720p");
    scene_gcm();
    CHECK(cellGcmSetDisplayBuffer(0, 0x100000, 0x1400, 1280, 720) == 0, "SetDisplayBuffer");
    gw32(label_ea(0x41), 0);
    CHECK(cellGcm_emit_flip_command(g_ctx, 0) == 0, "flip command");
    cellRescSetWaitFlip((void*)(uintptr_t)g_ctx);
    /* the next frame's first command: release label 0x41 */
    const uint32_t w[4] = { 0x00040064u, 0x410u, 0x0004006Cu, 0xAAu };
    cellGcm_ctx_emit(g_ctx, w, 4);
    gw32(g_ctrl + 0, gr32(g_ctx + 8) - IO_ADDR);
}

static void test_walker(void)
{
    flip_then_wait();
    const uint32_t put = gr32(g_ctrl + 0);
    for (int i = 0; i < 6; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(label_ea(1)) == 0xFFFFFFFFu, "the flip stream released the flip-done label: 0x%X", gr32(label_ea(1)));
    CHECK(gr32(label_ea(0x41)) == 0 && gr32(g_ctrl + 4) != put,
          "the RSX is held at the ACQUIRE: next frame's release 0x%X, get 0x%X put 0x%X", gr32(label_ea(0x41)),
          gr32(g_ctrl + 4), put);
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD) == 1, "the flip head retired");
    CHECK(cellGcm_take_flip_pending_synced() == 1,
          "the frame behind the stalled ACQUIRE is presentable (get 0x%X != put 0x%X)", gr32(g_ctrl + 4), put);
    CHECK(gr32(label_ea(1)) == 0xFFFFFFFFu, "no vblank yet: the flip is not complete");

    cellGcmTickVBlank();   /* the driver completes the flip */
    CHECK(gr32(label_ea(1)) == 0, "the vblank zeroed the flip-done label: 0x%X", gr32(label_ea(1)));
    for (int i = 0; i < 6 && gr32(g_ctrl + 4) != put; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == put && gr32(label_ea(0x41)) == 0xAAu, "released: get 0x%X put 0x%X, label 0x41 = 0x%X",
          gr32(g_ctrl + 4), put, gr32(label_ea(0x41)));
    cellGcmTickVBlank();
    CHECK(gr32(label_ea(1)) == 0, "a vblank with no flip outstanding leaves the label alone");

    /* The two halves of the vblank: the vertical blank itself leaves the flip outstanding; the driver's answer
     * completes it. */
    flip_then_wait();
    const uint32_t put2 = gr32(g_ctrl + 0);
    for (int i = 0; i < 6; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    /* (the scene's reset counts the heads from zero again) */
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD) == 1 && gr32(label_ea(1)) == 0xFFFFFFFFu,
          "the second flip head retired and released the label: heads %u, label 0x%X",
          (unsigned)gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD), gr32(label_ea(1)));
    CHECK(cellGcm_take_flip_pending_synced() == 1, "the second frame is presentable (put 0x%X)", put2);
    cellGcm_vblank_signal();
    CHECK(gr32(label_ea(1)) == 0xFFFFFFFFu, "the vertical blank alone does not complete the flip: 0x%X", gr32(label_ea(1)));
    cellGcm_vblank_flip_label();
    CHECK(gr32(label_ea(1)) == 0, "the driver's answer zeroed the flip-done label: 0x%X", gr32(label_ea(1)));
    for (int i = 0; i < 6 && gr32(g_ctrl + 4) != put2; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == put2, "released: get 0x%X put 0x%X", gr32(g_ctrl + 4), put2);
}

/* ---- the named stop --------------------------------------------------------------------------------- */

static int run_child(const char* self, const char* mode, char* out, size_t cap)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "PS3RECOMP_GCM_WAIT_TIMEOUT_MS=60 '%s' %s 2>&1", self, mode);
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
        if (!strcmp(argv[1], "never-completes")) {
            /* No vblank ever completes the flip: the ACQUIRE has no producer. */
            flip_then_wait();
            for (int i = 0; i < 500; i++) { cellGcm_rsx_process_fifo(); usleep(5000); }
            return 0;
        }
        return 2;
    }

    test_label_by_mode();
    test_without_init();
    test_callback();
    test_walker();

    char out[8192];
    int rc = run_child(argv[0], "never-completes", out, sizeof out);
    CHECK(rc == 70, "never-completes: exit status %d, want 70", rc);
    CHECK(strstr(out, "PS3STOP v=1 kind=gcm-wait-timeout func=") != NULL, "never-completes: no gcm-wait-timeout in '%s'", out);
    CHECK(strstr(out, "what=semaphore-acquire") != NULL && strstr(out, "offset=0x00000010") != NULL &&
          strstr(out, "value=0x00000000") != NULL && strstr(out, "current=0xFFFFFFFF") != NULL,
          "never-completes: the stop does not name the flip-done acquire: '%s'", out);

    printf("[resc_waitflip] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
