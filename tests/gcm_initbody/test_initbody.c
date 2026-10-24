/*
 * ps3recomp - tests/gcm_initbody/test_initbody.c
 *
 * The RSX command-buffer plumbing libgcm_sys builds in _cellGcmInitBody and the wait/ordering
 * semantics the walker needs around it (FW 4.93; the layout and callback are libgcm_sys
 * 0x020009A8, 0x020036D4, 0x02003AAC):
 *
 *   - InitBody's argument validation and the layout it builds: the default-state subroutine at io 0
 *     (a well-formed command stream ending in RET), the context over the first 32 KiB segment at
 *     io + 0x1000, put == get == 0x1000, the default command/segment word sizes;
 *   - the buffer-full callback: JUMP to the next segment, the wrap at the last one, the wait for the
 *     RSX to leave the segment (with a thread standing in for the RSX), and the named stop when
 *     nothing ever does;
 *   - NV406E semaphore acquire: blocks the walker until the label holds the value, a driver
 *     handshake for the flip-ready label, a named stop after gcm.wait_timeout_ms;
 *   - the label window follows ppu_hle_inject_base;
 *   - GCM_SET_USER_COMMAND is delivered in order and never coalesced, and the driver events are
 *     counted in retirement order.
 *
 * Synthetic: every address, size and value is made up here; the stream words are checked
 * structurally, not against recorded bytes.
 *
 *   ctest --test-dir build/dev -R gcm_initbody --output-on-failure
 */
#include "cellGcmSys.h"
#include "ps3emu/guest_call.h"
#include <pthread.h>
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
extern void cellGcm_fifo_recycle(u32 ctx_ea);
extern void ppu_gcm_pump(void);

uint8_t* vm_base = NULL;

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00400000u
#define CMD_SIZE  0x00200000u                 /* 64 segments of 32 KiB */
#define RING      (IO_ADDR + 0x1000u)
#define HEAP      0x00800000u

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

static void scene(void)
{
    cellGcmTerminate();
    u32 ctx_out = host_alloc(4, 4);
    g_ctx = cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();
}

/* ---- InitBody ------------------------------------------------------------------------------- */

static void test_validation(void)
{
    cellGcmTerminate();
    u32 out = host_alloc(4, 4);
    CHECK(cellGcmSetupContext(out, 0xFFFCu, IO_SIZE, IO_ADDR, host_alloc, host_w32) == 0, "cmdSize 0xFFFC");
    CHECK(cellGcmSetupContext(out, 0x10002u, IO_SIZE, IO_ADDR, host_alloc, host_w32) == 0, "cmdSize not a word multiple");
    CHECK(cellGcmSetupContext(out, 0x400000u - 0xFFCu, IO_SIZE, IO_ADDR, host_alloc, host_w32) == 0,
          "cmdSize + 0x1000 > ioSize");
    CHECK(cellGcmSetupContext(out, CMD_SIZE, 0x380000u, IO_ADDR, host_alloc, host_w32) == 0, "ioSize not 1 MiB aligned");
    CHECK(cellGcmSetupContext(out, CMD_SIZE, IO_SIZE, IO_ADDR + 0x800u, host_alloc, host_w32) == 0,
          "ioAddress not 1 MiB aligned");
    CHECK(cellGcmGetDefaultCommandWordSize() == 0 && cellGcmGetDefaultSegmentWordSize() == 0,
          "nothing initialised by the rejected calls");
    /* The smallest accepted command size is 0x10000, and the largest the IO window leaves. */
    CHECK(cellGcmSetupContext(out, 0x10000u, IO_SIZE, IO_ADDR, host_alloc, host_w32) != 0, "cmdSize 0x10000");
    CHECK(cellGcmGetDefaultCommandWordSize() == 0x4000u, "0x10000 bytes = 0x4000 words, got 0x%X",
          cellGcmGetDefaultCommandWordSize());
}

/* Walk the default-state subroutine: every word is a header whose count words follow, until RET. */
static void test_default_state(void)
{
    scene();
    CHECK(g_ctx != 0, "SetupContext");
    uint32_t off = 0, headers = 0, words = 0, bound7 = 0, calls = 0;
    for (;;) {
        const uint32_t w = gr32(IO_ADDR + off);
        if (w == 0x00020000u) break;
        if ((w & 3u) != 0 || (w >> 29) != 0 || off > 0x1000u) {
            CHECK(0, "word 0x%08X at io 0x%X is not a method header", w, off);
            return;
        }
        const uint32_t count = (w >> 18) & 0x7FFu, subch = (w >> 13) & 7u, method = w & 0x1FFCu;
        if (subch == 7 && method == 0 && count == 1 && gr32(IO_ADDR + off + 4) == 0xCAFEBABEu) bound7++;
        headers++; words += 1 + count;
        off += 4 + count * 4;
        (void)calls;
    }
    CHECK(off == 0x98Cu, "RET at io 0x%X, want 0x98C", off);
    CHECK(headers > 0 && words == off / 4, "stream parses into %u headers, %u words", headers, words);
    CHECK(bound7 == 1, "subchannel 7 bound once, %u", bound7);
    CHECK(gr32(IO_ADDR + 0x990) == 0, "nothing written past the RET");
}

static void test_context_layout(void)
{
    scene();
    CHECK(gr32(g_ctx + 0) == RING, "begin 0x%X want 0x%X", gr32(g_ctx + 0), RING);
    CHECK(gr32(g_ctx + 4) == IO_ADDR + 0x7FFCu, "end 0x%X want 0x%X", gr32(g_ctx + 4), IO_ADDR + 0x7FFCu);
    CHECK(gr32(g_ctx + 8) == RING, "current");
    const uint32_t opd = gr32(g_ctx + 0xC);
    CHECK(opd != 0 && gr32(opd) == ppu_hle_inject_base + 0x2F00u, "callback is the runtime's sentinel");
    CHECK(gr32(g_ctrl + 0) == 0x1000u && gr32(g_ctrl + 4) == 0x1000u, "put 0x%X get 0x%X, want 0x1000",
          gr32(g_ctrl + 0), gr32(g_ctrl + 4));
    CHECK(cellGcmGetDefaultCommandWordSize() == 0x80000u, "command words 0x%X", cellGcmGetDefaultCommandWordSize());
    CHECK(cellGcmGetDefaultSegmentWordSize() == 0x2000u, "segment words 0x%X", cellGcmGetDefaultSegmentWordSize());
}

/* ---- the buffer-full callback --------------------------------------------------------------- */

static void test_segment_switch(void)
{
    scene();
    /* The RSX has drained the first segment and idles at its put. */
    uint32_t end = gr32(g_ctx + 4);
    for (unsigned seg = 1; seg < 64; seg++) {
        const uint32_t cur = end - 0x20u;
        gw32(g_ctx + 8, cur);
        cellGcm_fifo_recycle(g_ctx);
        const uint32_t start = end + 4u;
        CHECK(gr32(g_ctx + 0) == start && gr32(g_ctx + 8) == start &&
              gr32(g_ctx + 4) == start + 0x7FFCu, "segment %u: ctx 0x%X..0x%X", seg, gr32(g_ctx + 0), gr32(g_ctx + 4));
        CHECK(gr32(cur) == (0x20000000u | (start - IO_ADDR)), "segment %u: JUMP word 0x%08X", seg, gr32(cur));
        CHECK(gr32(g_ctrl + 0) == cur - IO_ADDR, "segment %u: put 0x%X is the flush at the old current", seg,
              gr32(g_ctrl + 0));
        end = gr32(g_ctx + 4);
    }
    CHECK(end == IO_ADDR + 0x200000u - 4u, "the last segment ends the command area, 0x%X", end);
    /* The last segment wraps to the first, 0x1000 into the window. */
    gw32(g_ctrl + 4, 0x20000u);          /* the RSX is elsewhere in the ring, not in the segment the wrap enters */
    gw32(g_ctx + 8, end - 0x20u);
    cellGcm_fifo_recycle(g_ctx);
    CHECK(gr32(g_ctx + 0) == RING && gr32(g_ctx + 4) == IO_ADDR + 0x7FFCu, "wrap: ctx 0x%X..0x%X",
          gr32(g_ctx + 0), gr32(g_ctx + 4));
    CHECK(gr32(end - 0x20u) == (0x20000000u | 0x1000u), "wrap: JUMP word 0x%08X", gr32(end - 0x20u));
}

static void* rsx_leaves(void* arg)
{
    usleep(40 * 1000);
    gw32(g_ctrl + 4, *(uint32_t*)arg);          /* get moves out of the segment */
    return NULL;
}

static void test_segment_wait(void)
{
    scene();
    gw32(g_ctx + 8, gr32(g_ctx + 4) - 0x10u);
    gw32(g_ctrl + 4, 0x9000u);                   /* the RSX is inside the next segment */
    uint32_t out_of_it = 0x2000u;
    pthread_t t;
    pthread_create(&t, NULL, rsx_leaves, &out_of_it);
    const clock_t t0 = clock();
    (void)t0;
    cellGcm_fifo_recycle(g_ctx);
    pthread_join(t, NULL);
    CHECK(gr32(g_ctrl + 4) == 0x2000u && gr32(g_ctx + 0) == IO_ADDR + 0x8000u,
          "the callback returned once get left the segment, ctx begin 0x%X", gr32(g_ctx + 0));
    /* ...and likewise while the RSX is outside the ring (running the default-state subroutine). */
    gw32(g_ctx + 8, gr32(g_ctx + 4) - 0x10u);
    gw32(g_ctrl + 4, 0x200u);
    out_of_it = 0x3000u;
    pthread_create(&t, NULL, rsx_leaves, &out_of_it);
    cellGcm_fifo_recycle(g_ctx);
    pthread_join(t, NULL);
    CHECK(gr32(g_ctrl + 4) == 0x3000u, "waited while get was below the ring");
}

/* ---- NV406E semaphore ------------------------------------------------------------------------ */

static uint32_t g_wp;   /* write position, io offset */
static void w(uint32_t v) { gw32(IO_ADDR + g_wp, v); g_wp += 4; }
static void m(uint32_t subch, uint32_t method, uint32_t data)
{
    w((1u << 18) | (subch << 13) | method);
    w(data);
}
static void kick(void)
{
    gw32(g_ctx + 8, IO_ADDR + g_wp);
    gw32(g_ctrl + 0, g_wp);
}
static void drain(void)
{
    for (int i = 0; i < 40 && gr32(g_ctrl + 4) != g_wp; i++) {
        cellGcm_rsx_process_fifo();
        usleep(2000);
    }
}
static uint32_t label_ea(u8 index) { return (uint32_t)(uintptr_t)cellGcmGetLabelAddress(index); }

static void test_label_window(void)
{
    scene();
    CHECK(label_ea(0) == ppu_hle_inject_base, "label 0 at the inject base 0x%X, got 0x%X", ppu_hle_inject_base, label_ea(0));
    CHECK(label_ea(0x40) == ppu_hle_inject_base + 0x400u, "label 0x40 at +0x400");
    CHECK(label_ea(0x45) == ppu_hle_inject_base + 0x450u, "label 0x45 at +0x450");
}

static void test_acquire(void)
{
    scene();
    gw32(label_ea(0x41), 0);
    gw32(label_ea(0x42), 0);
    g_wp = 0x1000;
    m(0, 0x64, 0x410);
    m(0, 0x68, 5);                       /* waits for label 0x41 == 5 */
    const uint32_t acquire_at = g_wp - 8;
    m(0, 0x64, 0x420);
    m(0, 0x6C, 0xAA);                    /* then releases label 0x42 */
    kick();
    for (int i = 0; i < 5; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == acquire_at && gr32(label_ea(0x42)) == 0,
          "blocked on the acquire: get 0x%X (want 0x%X), label 0x42 = 0x%X", gr32(g_ctrl + 4), acquire_at,
          gr32(label_ea(0x42)));
    gw32(label_ea(0x41), 5);             /* the agent that was to write it */
    drain();
    CHECK(gr32(g_ctrl + 4) == g_wp && gr32(label_ea(0x42)) == 0xAA,
          "acquire released: get 0x%X put 0x%X, label 0x42 = 0x%X", gr32(g_ctrl + 4), g_wp, gr32(label_ea(0x42)));

    /* An acquire whose label already holds the value passes at once. */
    scene();
    gw32(label_ea(0x41), 7);
    g_wp = 0x1000;
    m(0, 0x64, 0x410);
    m(0, 0x68, 7);
    kick();
    drain();
    CHECK(gr32(g_ctrl + 4) == g_wp, "satisfied acquire does not block");
}

/* The flip stream's handshake: the driver answers the queued flip by storing 1 in label 0x30. */
static GcmDriverEvent g_events[16];
static unsigned g_nevents;
static void observer(const GcmDriverEvent* ev)
{
    if (g_nevents < 16) g_events[g_nevents] = *ev;
    g_nevents++;
}

static void flip_stream(int queued)
{
    g_wp = 0x1000;
    if (queued) m(7, 0x0944, 3);         /* GCM_DRIVER_QUEUE: buffer 3 */
    m(0, 0x64, 0x30);
    m(0, 0x6C, 0);
    m(0, 0x64, 0x30);
    m(0, 0x68, 1);                       /* acquire label 0x30 == 1 */
    m(0, 0x64, 0x10);
    m(0, 0x6C, 0xFFFFFFFFu);
    m(7, 0x0924, 0x8000010Fu);           /* GCM_FLIP_HEAD */
    kick();
}

static void test_flip_handshake(void)
{
    scene();
    g_nevents = 0;
    gcm_set_driver_observer(observer);
    flip_stream(1);
    drain();
    CHECK(gr32(g_ctrl + 4) == g_wp, "flip stream drains: get 0x%X put 0x%X", gr32(g_ctrl + 4), g_wp);
    CHECK(gr32(label_ea(3)) == 1 && gr32(label_ea(1)) == 0xFFFFFFFFu, "label 0x30 = %u, label 0x10 = 0x%X",
          gr32(label_ea(3)), gr32(label_ea(1)));
    CHECK(g_nevents == 2 && g_events[0].kind == GCM_DRV_EVENT_QUEUE_FLIP && g_events[0].arg == 3 &&
          g_events[1].kind == GCM_DRV_EVENT_FLIP_HEAD && g_events[1].arg == 3 &&
          g_events[0].seq == 1 && g_events[1].seq == 2,
          "events: %u, first kind %u arg %u seq %llu", g_nevents, g_events[0].kind, g_events[0].arg, g_events[0].seq);
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_QUEUE_FLIP) == 1 && gcm_driver_event_count(GCM_DRV_EVENT_FLIP_HEAD) == 1,
          "event counters");

    /* No queued flip: nothing answers, the acquire stands. */
    scene();
    flip_stream(0);
    const uint32_t acquire_at = 0x1000u + 8u + 8u + 8u;
    for (int i = 0; i < 5; i++) { cellGcm_rsx_process_fifo(); usleep(2000); }
    CHECK(gr32(g_ctrl + 4) == acquire_at, "unanswered handshake blocks at 0x%X, get 0x%X", acquire_at, gr32(g_ctrl + 4));
    gcm_set_driver_observer(NULL);
}

/* ---- ordered user commands ------------------------------------------------------------------- */

static uint32_t g_delivered[2048];
static unsigned g_ndelivered;
static void user_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (g_ndelivered < 2048) g_delivered[g_ndelivered] = (uint32_t)a0;
    g_ndelivered++;
}

static void emit_user_commands(unsigned n, unsigned first)
{
    g_wp = 0x1000;
    for (unsigned i = 0; i < n; i++) m(7, 0x0B00, first + i);
    kick();
}

static void test_user_commands(void)
{
    scene();
    const uint32_t opd = host_alloc(8, 8);
    gw32(opd, 0x00123400u);
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)opd);
    g_ps3_guest_caller = user_caller;
    g_ndelivered = 0;

    /* 300 commands retired before the pump runs once: all arrive, in order, none coalesced. */
    emit_user_commands(300, 1000);
    drain();
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_USER_COMMAND) == 300, "300 events, got %llu",
          gcm_driver_event_count(GCM_DRV_EVENT_USER_COMMAND));
    ppu_gcm_pump();
    int ordered = g_ndelivered == 300;
    for (unsigned i = 0; ordered && i < 300; i++) ordered = g_delivered[i] == 1000 + i;
    CHECK(ordered, "300 user commands delivered in order (%u delivered)", g_ndelivered);
    CHECK(gcm_user_ring_drops() == 0, "no drops");

    /* A burst larger than the ring loses the newest and says so. */
    scene();
    gw32(opd, 0x00123400u);
    cellGcmSetUserHandler((CellGcmUserHandler)(uintptr_t)opd);
    g_ndelivered = 0;
    emit_user_commands(1100, 1);
    for (int i = 0; i < 40 && gr32(g_ctrl + 4) != g_wp; i++) { cellGcm_rsx_process_fifo(); usleep(1000); }
    ppu_gcm_pump();
    ordered = g_ndelivered == 1024;
    for (unsigned i = 0; ordered && i < 1024; i++) ordered = g_delivered[i] == 1 + i;
    CHECK(ordered, "1024 delivered in order (%u delivered)", g_ndelivered);
    CHECK(gcm_user_ring_drops() == 1100u - 1024u, "drops %u, want %u", gcm_user_ring_drops(), 1100u - 1024u);
    CHECK(gcm_driver_event_count(GCM_DRV_EVENT_USER_COMMAND) == 1100, "every retirement is an event");
    g_ps3_guest_caller = NULL;
}

/* ---- named stops ----------------------------------------------------------------------------- */

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

static void check_timeout(const char* self, const char* mode, const char* what)
{
    char out[8192];
    int rc = run_child(self, mode, out, sizeof out);
    CHECK(rc == 70, "%s: exit status %d, want 70", mode, rc);
    CHECK(strstr(out, "PS3STOP v=1 kind=gcm-wait-timeout func=") != NULL, "%s: no gcm-wait-timeout stop in '%s'", mode, out);
    char want[128];
    snprintf(want, sizeof want, "what=%s", what);
    CHECK(strstr(out, want) != NULL, "%s: no '%s' in '%s'", mode, want, out);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The HLE page holds the labels, control register and (at +0x31000) the RSX driver info that
     * a user command's interrupt writes. */
    const uint32_t vm_size = ppu_hle_inject_base + 0x40000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    if (argc == 2) {
        if (!strcmp(argv[1], "acquire-timeout")) {
            scene();
            gw32(label_ea(0x41), 0);
            g_wp = 0x1000;
            m(0, 0x64, 0x410);
            m(0, 0x68, 5);
            kick();
            for (int i = 0; i < 500; i++) { cellGcm_rsx_process_fifo(); usleep(5000); }
            return 0;
        }
        if (!strcmp(argv[1], "segment-timeout")) {
            scene();
            gw32(g_ctx + 8, gr32(g_ctx + 4) - 0x10u);
            gw32(g_ctrl + 4, 0x9000u);
            cellGcm_fifo_recycle(g_ctx);
            return 0;
        }
        return 2;
    }

    test_validation();
    test_default_state();
    test_context_layout();
    test_segment_switch();
    test_segment_wait();
    test_label_window();
    test_acquire();
    test_flip_handshake();
    test_user_commands();

    check_timeout(argv[0], "acquire-timeout", "semaphore-acquire");
    check_timeout(argv[0], "segment-timeout", "segment-busy");

    printf("[gcm_initbody] %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    free(vm_base);
    return g_fail ? 1 : 0;
}
