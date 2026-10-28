/*
 * ps3recomp - tests/gcm_initbody/test_setwaitflip.c
 *
 * cellGcmSetWaitFlip(ctx) / cellGcmSetWaitFlipUnsafe(ctx) (libgcm_sys FW 4.93: bodies 0x02001180 /
 * 0x02001228, NIDs 0x983FB9AA / 0xDF6476BD) and the flip-done label they wait on.
 *
 * The firmware never waits on the CPU. It appends two method pairs to the caller's command context:
 *     0x00040064 0x10     NV406E SEMAPHORE_OFFSET (label 0x10, the one the flip command releases)
 *     0x00040068 0        NV406E SEMAPHORE_ACQUIRE
 * and advances `current` by 16. SetWaitFlip reserves them first (`current + 16 > end` calls
 * callback(ctx, 4)); the Unsafe export writes at `current` without looking at `end`.
 *
 * The wait is then the walker's: the RSX holds at the ACQUIRE until the driver completes the flip
 * (the vblank after the flip head writes 0 to label 0x10), and a flip nobody completes is the named
 * stop gcm-wait-timeout, not a silent hang.
 *
 * Synthetic configuration, nothing recorded.
 *
 *   ctest --test-dir build/dev -R gcm_setwaitflip --output-on-failure
 */
#include "cellGcmSys.h"
#include "ps3emu/guest_call.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
extern void vm_write32(uint64_t addr, uint32_t v);
extern uint32_t vm_read32(uint64_t addr);
extern void cellGcm_rsx_process_fifo(void);
extern int cellGcm_take_flip_pending_synced(void);
extern void cellGcmTickVBlank(void);

uint8_t* vm_base = NULL;

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

static void wait_flip(uint32_t ctx) { cellGcmSetWaitFlip((void*)(uintptr_t)ctx); }
static void wait_flip_unsafe(uint32_t ctx) { cellGcmSetWaitFlipUnsafe((void*)(uintptr_t)ctx); }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- the words, and no CPU wait --------------------------------------------------------------- */

static void test_pairs_no_sleep(void)
{
    scene_gcm();
    /* A flip is outstanding: the old HLE slept up to 64 ms per call in exactly this state. */
    cellGcmResetFlipStatus();
    CHECK(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING, "flip status is WAITING");

    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 8; i++) {
        const uint32_t at = gr32(g_ctx + 8);
        const uint64_t t0 = now_ns();
        wait_flip(g_ctx);
        const uint64_t dt = now_ns() - t0;
        if (dt < best) best = dt;
        CHECK(gr32(g_ctx + 8) == at + 16u, "call %d: current advanced %u bytes, want 16", i, gr32(g_ctx + 8) - at);
        CHECK(gr32(at + 0) == 0x00040064u && gr32(at + 4) == 0x10u && gr32(at + 8) == 0x00040068u &&
              gr32(at + 12) == 0u,
              "call %d: words %08X %08X %08X %08X, want 00040064 00000010 00040068 00000000", i, gr32(at),
              gr32(at + 4), gr32(at + 8), gr32(at + 12));
    }
    CHECK(best < 1000000ull, "the fastest of 8 calls took %llu ns with a flip outstanding; the firmware does not wait",
          (unsigned long long)best);
    CHECK(cellGcmGetFlipStatus() == CELL_GCM_FLIP_STATUS_WAITING,
          "SetWaitFlip does not touch the flip status: only the vblank completes it");
}

/* ---- the buffer-full callback ------------------------------------------------------------------ */

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

static uint32_t install_callback(uint32_t room)
{
    const uint32_t opd = host_alloc(8, 8);
    gw32(opd, 0x00123400u);      /* a title's own callback, not the runtime's */
    gw32(opd + 4, 0);
    gw32(g_ctx + 0xC, opd);
    gw32(g_ctx + 4, gr32(g_ctx + 8) + room);
    g_seg2 = IO_ADDR + 0x40000u;
    return opd;
}

static void test_callback(void)
{
    scene_gcm();
    install_callback(12u);       /* 12 bytes left: the 16-byte group does not fit */
    const uint32_t full_at = gr32(g_ctx + 8);
    gw32(full_at, 0);            /* earlier scenes wrote their groups here: SetupContext does not clear the ring */
    g_ps3_guest_caller = fake_caller;
    g_cb_calls = 0;
    wait_flip(g_ctx);
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 1 && g_cb_ctx == g_ctx && g_cb_words == 4u,
          "callback(ctx 0x%X, words %u) x%d, want (0x%X, 4) x1", g_cb_ctx, g_cb_words, g_cb_calls, g_ctx);
    CHECK(gr32(g_seg2) == 0x00040064u && gr32(g_seg2 + 4) == 0x10u && gr32(g_seg2 + 8) == 0x00040068u &&
          gr32(g_seg2 + 12) == 0u && gr32(g_ctx + 8) == g_seg2 + 16u,
          "the pairs land in the segment the callback chose");
    CHECK(gr32(full_at) != 0x00040064u, "nothing was written at the full segment's current");

    /* Exactly 16 bytes left is room: no callback. */
    scene_gcm();
    install_callback(16u);
    const uint32_t at = gr32(g_ctx + 8);
    g_ps3_guest_caller = fake_caller;
    g_cb_calls = 0;
    wait_flip(g_ctx);
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 0 && gr32(g_ctx + 8) == at + 16u && gr32(at) == 0x00040064u,
          "a group that exactly fits does not call the callback");
}

/* cellGcmSetWaitFlipUnsafe (0x02001228) is the same two pairs without the space check. */
static void test_unsafe(void)
{
    scene_gcm();
    install_callback(0u);        /* no room at all: the Unsafe body does not look */
    const uint32_t at = gr32(g_ctx + 8);
    cellGcmResetFlipStatus();
    g_ps3_guest_caller = fake_caller;
    g_cb_calls = 0;
    const uint64_t t0 = now_ns();
    wait_flip_unsafe(g_ctx);
    const uint64_t dt = now_ns() - t0;
    g_ps3_guest_caller = NULL;
    CHECK(g_cb_calls == 0, "Unsafe never calls the buffer-full callback (x%d)", g_cb_calls);
    CHECK(gr32(g_ctx + 8) == at + 16u, "Unsafe advanced current by %u, want 16", gr32(g_ctx + 8) - at);
    CHECK(gr32(at + 0) == 0x00040064u && gr32(at + 4) == 0x10u && gr32(at + 8) == 0x00040068u &&
          gr32(at + 12) == 0u, "Unsafe words %08X %08X %08X %08X", gr32(at), gr32(at + 4), gr32(at + 8), gr32(at + 12));
    CHECK(dt < 20000000ull, "Unsafe took %llu ns with a flip outstanding", (unsigned long long)dt);
}

/* ---- through the walker ------------------------------------------------------------------------- */

static uint32_t label_ea(u8 i) { return (uint32_t)(uintptr_t)cellGcmGetLabelAddress(i); }

/* Frame N's flip, SetWaitFlip, then frame N+1's first command: release label 0x41. */
static void flip_then_wait(void)
{
    scene_gcm();
    CHECK(cellGcmSetDisplayBuffer(0, 0x100000, 0x1400, 1280, 720) == 0, "SetDisplayBuffer");
    gw32(label_ea(0x41), 0);
    CHECK(cellGcm_emit_flip_command(g_ctx, 0) == 0, "flip command");
    wait_flip(g_ctx);
    const uint32_t w[4] = { 0x00040064u, 0x410u, 0x0004006Cu, 0xAAu };
    cellGcm_ctx_emit(g_ctx, w, 4);
    gw32(g_ctrl + 0, gr32(g_ctx + 8) - IO_ADDR);
}

static void test_walker(void)
{
    flip_then_wait();
    const uint32_t put = gr32(g_ctrl + 0);
    for (int i = 0; i < 6; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(label_ea(1)) == 0xFFFFFFFFu, "the flip stream released label 0x10: 0x%X", gr32(label_ea(1)));
    CHECK(gr32(label_ea(0x41)) == 0 && gr32(g_ctrl + 4) != put,
          "the RSX is held at SetWaitFlip's ACQUIRE: next frame's release 0x%X, get 0x%X put 0x%X",
          gr32(label_ea(0x41)), gr32(g_ctrl + 4), put);
    CHECK(cellGcm_take_flip_pending_synced() == 1,
          "the frame behind the stalled ACQUIRE is presentable (get 0x%X != put 0x%X)", gr32(g_ctrl + 4), put);

    cellGcmTickVBlank();   /* the driver completes the flip: label 0x10 goes back to 0 */
    CHECK(gr32(label_ea(1)) == 0, "the vblank zeroed label 0x10: 0x%X", gr32(label_ea(1)));
    for (int i = 0; i < 6 && gr32(g_ctrl + 4) != put; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == put && gr32(label_ea(0x41)) == 0xAAu,
          "the completion released the ACQUIRE: get 0x%X put 0x%X, label 0x41 = 0x%X", gr32(g_ctrl + 4), put,
          gr32(label_ea(0x41)));

    /* A SetWaitFlip with no flip outstanding passes at once (label 0x10 already 0). */
    scene_gcm();
    wait_flip(g_ctx);
    const uint32_t tail[4] = { 0x00040064u, 0x410u, 0x0004006Cu, 0xBBu };
    cellGcm_ctx_emit(g_ctx, tail, 4);
    gw32(label_ea(0x41), 0);
    gw32(g_ctrl + 0, gr32(g_ctx + 8) - IO_ADDR);
    const uint32_t put2 = gr32(g_ctrl + 0);
    for (int i = 0; i < 6 && gr32(g_ctrl + 4) != put2; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == put2 && gr32(label_ea(0x41)) == 0xBBu,
          "no flip outstanding: the ACQUIRE of 0 is satisfied: get 0x%X put 0x%X", gr32(g_ctrl + 4), put2);
}

/* ---- named stops --------------------------------------------------------------------------------- */

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
            /* No vblank ever completes the flip: SetWaitFlip's ACQUIRE has no producer. */
            flip_then_wait();
            for (int i = 0; i < 500; i++) { cellGcm_rsx_process_fifo(); usleep(5000); }
            return 0;
        }
        if (!strcmp(argv[1], "ctx-outside-guest")) {
            scene_gcm();
            wait_flip(vm_size - 8u);          /* the 16-byte context straddles the end of guest memory */
            return 0;
        }
        if (!strcmp(argv[1], "unsafe-ctx-outside-guest")) {
            scene_gcm();
            wait_flip_unsafe(vm_size - 8u);
            return 0;
        }
        if (!strcmp(argv[1], "current-outside-guest")) {
            scene_gcm();
            gw32(g_ctx + 4, vm_size);         /* end past current, so no callback */
            gw32(g_ctx + 8, vm_size - 8u);    /* current: the 16-byte group straddles the end */
            wait_flip(g_ctx);
            return 0;
        }
        return 2;
    }

    test_pairs_no_sleep();
    test_callback();
    test_unsafe();
    test_walker();

    char out[8192];
    int rc = run_child(argv[0], "never-completes", out, sizeof out);
    CHECK(rc == 70, "never-completes: exit status %d, want 70", rc);
    CHECK(strstr(out, "PS3STOP v=1 kind=gcm-wait-timeout func=") != NULL, "never-completes: no gcm-wait-timeout in '%s'", out);
    CHECK(strstr(out, "what=semaphore-acquire") != NULL && strstr(out, "offset=0x00000010") != NULL &&
          strstr(out, "value=0x00000000") != NULL && strstr(out, "current=0xFFFFFFFF") != NULL,
          "never-completes: the stop does not name the flip-done acquire: '%s'", out);

    static const char* const range_modes[] = { "ctx-outside-guest", "unsafe-ctx-outside-guest", "current-outside-guest" };
    static const char* const range_args[] = { "ctx", "ctx", "current" };
    for (int i = 0; i < 3; i++) {
        rc = run_child(argv[0], range_modes[i], out, sizeof out);
        char arg[32];
        snprintf(arg, sizeof arg, "arg=%s", range_args[i]);
        CHECK(rc == 70, "%s: exit status %d, want 70", range_modes[i], rc);
        CHECK(strstr(out, "kind=hle-guest-range") != NULL && strstr(out, arg) != NULL,
              "%s: no hle-guest-range stop naming %s in '%s'", range_modes[i], range_args[i], out);
    }

    printf("[gcm_setwaitflip] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
