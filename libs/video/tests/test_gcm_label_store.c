/* A guest store to a label is ordered after the walker (cellGcmSys.c, gcm_guest_label_store).
 *
 * The title queues NV406E SEMAPHORE_OFFSET + ACQUIRE(N) for a label, kicks `put`, and overwrites the label
 * with N+1 as soon as it builds its next frame. ACQUIRE compares for equality, so the RSX has to read the
 * kicked ACQUIRE before that store. The console's RSX does, within microseconds of the `put` store; here the
 * walker is a polling host thread that a loaded host leaves off the CPU. The "RSX" in this test is the main
 * thread and is deliberately late (LATE_MS before its first look at the kick); the "PPU" is a second thread.
 *
 *   control     the PPU's store is a plain store (what the runtime did before the ordering): it lands before
 *               the walker looks, the label holds N+1, and the walker is held at ACQUIRE(N) for ever. This is
 *               the failure, reproduced without a loaded host.
 *   ordered     the PPU's store goes through the hook the runtime registered (ppu_set_store_hook, which the
 *               loader's vm_write32 reaches through ps3_label_window_store before the store lands): it waits
 *               for the late walker, which
 *               passes ACQUIRE(N) against the old value; then the label becomes N+1, the next segment's
 *               ACQUIRE(N+1) passes too, and nothing stopped. Also: with `get` at `put` the store does not
 *               wait (no walker needed), and a walker thread's own store never waits.
 *               The doorbell (cellGcm_set_walker_wake_hook): the guest's `put` store rings STORE; a store that
 *               waits for a pass rings PASS before it waits (the pass it waits for) and STORE after (the store
 *               lands after the wait); a store with nothing to wait for rings nothing. cellGcm_rsx_walker_pending
 *               is true for a `put` that moved, false parked at an ACQUIRE, true once the label holds the value.
 *   unrung      the ordered flow with NO doorbell hook installed: a ring that is never delivered (here, a host
 *               that only polls) costs the wait for the pass its own loop runs, and #17 still holds. Every
 *               invariant of `ordered` is asserted, and every ring count must be zero.
 *   no-walker   nobody walks: the store stops with gcm-wait-timeout what=rsx-walker-pass.
 *
 * Run as tools/test_gcm_label_store.py does: `test <mode> <timeout ms>`; control and ordered must exit 0,
 * no-walker must exit 70 with the stop line.
 *
 * cc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_label_store.c
 *   runtime/memory/label_window.c libs/video/gcm_default_state.c -ffunction-sections
 *   -fdata-sections -Wl,--gc-sections -pthread -o /tmp/test_gcm_label_store
 *   (clang on macOS: -Wl,-dead_strip)   (tools/test_gcm_label_store.py)
 */
#define RSX_STUB_OWN_CONFIG   /* ps3_config_get() below */
#include "../cellGcmSys.c"
#include "../rsx_tiling.c"        /* cellGcmSys.c publishes the bound tiles here and the 2D engines read them (v0.19.0 #569) */
/* GCM_REPORT_SPU_HOLD_MS (gcm.report_spu_hold_ms) asks the SPU task layer whether a task round is
 * running; there are no SPU tasks here. */
int spu_taskset_round_busy(uint32_t* ts, uint32_t* tid) { (void)ts; (void)tid; return 0; }
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint8_t* vm_base;
uint64_t ps3_system_time_us(void) { return 0; }   /* sys_timer.c */
uint32_t ppu_vm_size;
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
/* No microcode or constant runs here, and no vtable backend registered (#466, #554): the walker hands every
 * word to rsx_process_method one by one. */
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
int  rsx_draw_engine_enabled(void) { return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { (void)method; (void)arg; }
u32  rsx_dsp_method_class(u32 method) { (void)method; return RSX_DSP_CLASS_TODO; }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return 0; }
void rsx_live_draw_method(u32 method, u32 arg) { (void)method; (void)arg; }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }

/* The typed configuration cellGcmSys.c reads: the wait timeout (argv[2], milliseconds), short here so that a
 * wait that is meant to expire does. And the named stop it ends in: the process status of a stop (70) and
 * the fields. */
static unsigned g_timeout_ms = 10000;
const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    cfg.gcm_wait_timeout_ms = g_timeout_ms;
    cfg.eng_depth_clamp = true;
    cfg.eng_depth_bounds = true;
    return &cfg;
}
void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset, uint32_t value,
                               uint32_t current, uint32_t waited_ms)
{
    printf("[STOP] gcm-wait-timeout func=%s what=%s offset=0x%08X value=0x%08X current=0x%08X waited_ms=%u\n", func,
           what, offset, value, current, waited_ms);
    fflush(stdout);
    exit(70);
}
/* The NV0039 copy's stops: no fixture here copies, so reaching one is a bug. */
/* S1's live stop: a fixture that reaches it feeds the walker a pair nothing classifies. */
PS3_NORETURN void ps3_stop_rsx_unknown_method(const char* cls, uint32_t method, uint32_t subchannel,
                                              uint32_t fifo, uint32_t word, uint32_t data)
{
    printf("[FAIL] rsx-unknown-method class %s method 0x%04X subchannel %u fifo 0x%08X word 0x%08X data 0x%08X\n",
           cls ? cls : "invalid", method, subchannel, fifo, word, data);
    exit(70);
}
#include "rsx_driver_stop_stub.h"   /* the walker's rsx-driver-method stop; no flip word here */

PS3_NORETURN void ps3_stop_gcm_unmodelled(const char* func, const char* feature, const char* reason)
{
    printf("[FAIL] gcm-unmodelled in %s: %s: %s\n", func, feature, reason);
    exit(70);
}
PS3_NORETURN void ps3_stop_guest_range(const char* func, const char* arg, uint64_t ea, uint64_t size,
                                       uint64_t limit)
{
    printf("[FAIL] hle-guest-range in %s: %s ea 0x%llX size 0x%llX limit 0x%llX\n", func, arg,
           (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    exit(70);
}

/* What ppu_loader.cpp's vm_write32 does for a guest store: the registered hook first, then the store. The
 * window and hook are the runtime library's (runtime/memory/label_window.c), registered by the first walker
 * pass through ppu_set_store_hook; ps3_label_window_store is the same out-of-line call the loader makes. */
static void guest_store32(u32 ea, u32 v)
{
    ps3_label_window_store(ea, 4, v);
    vm_write32(ea, v);
}

#define LABEL_OFF   0x450u                       /* label 0x45 */
#define LABEL_EA    (GCM_LABEL_GUEST_BASE + LABEL_OFF)
#define OTHER_EA    (GCM_LABEL_GUEST_BASE + 0x460u)
#define N           0x20u
#define LATE_MS     120u                         /* how late the walker is */

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u
#define CMD0      0x00001000u

static uint32_t g_heap = 0x00400000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }

static int fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static u32 put_cmd(u32 io, u32 subch, u32 method, u32 count, const u32* args)
{
    gw32(IO_ADDR + io, (count << 18) | (subch << 13) | method);
    for (u32 i = 0; i < count; i++) gw32(IO_ADDR + io + 4 + i * 4, args[i]);
    return io + 4 + count * 4;
}
/* A segment as the title queues it: OFFSET, ACQUIRE(value), then a probe the walker only reaches past the
 * ACQUIRE (GCM_SET_USER_COMMAND, which calls rsx_raise_user_cmd from inside the pass). */
static u32 put_segment(u32 io, u32 value, u32 probe)
{
    const u32 off = LABEL_OFF;
    io = put_cmd(io, 0, 0x64u, 1, &off);
    io = put_cmd(io, 0, 0x68u, 1, &value);
    return put_cmd(io, 7, 0x0B00u, 1, &probe);
}

/* The label the walker saw when it reached each probe; read by the PPU thread after the hook returned. */
static atomic_uint seen_label[4], seen_n;
void rsx_raise_user_cmd(uint32_t arg)
{
    if (arg < 4) { atomic_store(&seen_label[arg], vm_read32(LABEL_EA)); atomic_fetch_add(&seen_n, 1); }
}

static uint32_t g_ctrl, g_end1, g_end2;
static atomic_int g_ppu_done;
static unsigned g_hook_wait_ms, g_early_ms;
static int g_mode_ordered;

/* The doorbell's ringer log: one counter per reason. */
static atomic_uint g_rings[3];
static void test_wake(unsigned reasons)
{
    for (unsigned i = 0; i < 3; i++)
        if (reasons & (1u << i)) atomic_fetch_add(&g_rings[i], 1u);
}

/* A guest thread storing while the main thread walks: a label the walker is not parked on, then the one it is. */
static void* store_thread(void* arg)
{
    (void)arg;
    guest_store32(OTHER_EA, 5u);
    guest_store32(LABEL_EA, N + 2u);
    atomic_store(&g_ppu_done, 1);
    return NULL;
}

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/* The PPU: published N and kicked the segment holding ACQUIRE(N), and now overwrites the label. */
static void* ppu_thread(void* arg)
{
    (void)arg;
    vm_write32(g_ctrl + 0, g_end1);                                   /* the kick */
    const unsigned long long t0 = now_ms();
    if (g_mode_ordered) guest_store32(LABEL_EA, N + 1u);
    else                vm_write32(LABEL_EA, N + 1u);                 /* control: no ordering */
    g_hook_wait_ms = (unsigned)(now_ms() - t0);
    if (g_mode_ordered) {
        /* The next frame's segment, behind the first one, waits for N+1. */
        g_end2 = put_segment(g_end1, N + 1u, 2);
        guest_store32(g_ctrl + 0, g_end2);                              /* the guest's kick, through the hook */
        for (unsigned long long d = now_ms() + 3000; atomic_load(&seen_n) < 2 && now_ms() < d;) usleep(500);
        /* Caught up (get == put): a store has nothing to wait for and needs no walker. */
        for (unsigned long long d = now_ms() + 3000; vm_read32(g_ctrl + 4) != g_end2 && now_ms() < d;) usleep(500);
        const unsigned long long t1 = now_ms();
        guest_store32(OTHER_EA, 7u);
        g_early_ms = (unsigned)(now_ms() - t1);
    }
    atomic_store(&g_ppu_done, 1);
    return NULL;
}

/* The host's active clock (cellGcm_set_host_active_clock), as the Metal backend on iOS supplies it while the app is
 * inactive: until released it stands still at the start, so the walker's wait clock does not run; once released it
 * runs again, less the time it stood still. Every read is counted: the store's wait loop is the only reader, one
 * read per poll, and its polls are what the test paces on instead of a sleep. */
static _Atomic unsigned g_pause_polls;
static _Atomic int g_pause_released;
static unsigned long long g_pause_t0, g_pause_frozen_ms;
static unsigned long long fixture_active_ms(void)
{
    atomic_fetch_add(&g_pause_polls, 1u);
    if (atomic_load(&g_pause_released)) return now_ms() - g_pause_frozen_ms;
    return g_pause_t0;
}
#define PAUSE_POLLS 300u   /* well past the 100 ms timeout's worth of polls (64 yields, then one per millisecond) */

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* mode = argc > 1 ? argv[1] : "ordered";
    if (argc > 2) g_timeout_ms = (unsigned)atoi(argv[2]);
    const int paused = !strcmp(mode, "paused");
    const int no_walker = !strcmp(mode, "no-walker") || paused;
    /* unrung: the ordered flow with no doorbell hook installed at all -- a host that polls and never rings. The
     * store's pass still comes from the host's own loop (here, one LATE_MS after the kick), which is what a missed
     * ring degrades to. Ordering is carried by the pass counters, never by the ring or by a sleep. */
    const int unrung = !strcmp(mode, "unrung");
    const unsigned want_store = unrung ? 0u : 1u, want_pass = unrung ? 0u : 1u;
    const unsigned want_store_end = unrung ? 0u : 2u, want_pass_end = unrung ? 0u : 3u;
    g_mode_ordered = strcmp(mode, "control") != 0;
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;

    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    g_ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* Segment 1, as queued by the title: ACQUIRE(N) and a probe behind it. The label already holds N. */
    g_end1 = put_segment(CMD0, N, 1);
    vm_write32(LABEL_EA, N);
    s_fifo_getoff = CMD0;
    vm_write32(g_ctrl + 4, CMD0);
    vm_write32(g_ctrl + 0, CMD0);
    cellGcm_rsx_process_fifo();                     /* the first pass registers the store hook; put == get */
    check((uint32_t)ppu_store_window == GCM_LABEL_GUEST_BASE &&
          (ppu_store_window >> 32) == GCM_CONTROL_GUEST_ADDR + GCM_CONTROL_REG_BYTES - GCM_LABEL_GUEST_BASE,
          "the first pass registered the store hook for the label area and the control register");
    if (!unrung) cellGcm_set_walker_wake_hook(test_wake);

    if (paused) {
        g_pause_t0 = now_ms();
        cellGcm_set_host_active_clock(fixture_active_ms);
    }
    pthread_t ppu;
    pthread_create(&ppu, NULL, ppu_thread, NULL);
    if (paused) {
        /* The store has waited PAUSE_POLLS polls, far longer than the timeout in wall time, and has not stopped. */
        while (atomic_load(&g_pause_polls) < PAUSE_POLLS) sched_yield();
        printf("[gcm_label_store paused] still waiting after %u polls\n", atomic_load(&g_pause_polls));
        g_pause_frozen_ms = now_ms() - g_pause_t0;
        atomic_store(&g_pause_released, 1);          /* the pause is over: the clock runs, and the wait expires */
    }
    if (no_walker) {
        sleep(30);                                  /* the hook stops the process long before */
        puts("[FAIL] no stop");
        return 1;
    }
    usleep(LATE_MS * 1000u);                        /* the walker is off the CPU: the kick is not read yet */
    const unsigned long long deadline = now_ms() + (g_mode_ordered ? 5000 : 300);
    while (now_ms() < deadline && !(g_mode_ordered && atomic_load(&g_ppu_done))) {
        cellGcm_rsx_process_fifo();
        usleep(500);
    }
    pthread_join(ppu, NULL);

    if (!g_mode_ordered) {
        check(vm_read32(LABEL_EA) == N + 1u, "control: the label overtook the ACQUIRE");
        check(atomic_load(&seen_n) == 0 && s_fifo_getoff == CMD0 + 8u,
              "control: the walker is held at ACQUIRE(N), the probe behind it never ran");
        printf("[gcm_label_store control] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
        return fails ? 1 : 0;
    }

    check(g_hook_wait_ms >= LATE_MS / 2u, "ordered: the PPU's store waited for the late walker");
    check(atomic_load(&seen_n) == 2, "ordered: both probes ran");
    check(atomic_load(&seen_label[1]) == N, "ordered: ACQUIRE(N) was evaluated against the old value");
    check(atomic_load(&seen_label[2]) == N + 1u, "ordered: ACQUIRE(N+1) passed against the new value");
    check(vm_read32(LABEL_EA) == N + 1u, "ordered: the store landed");
    check(vm_read32(g_ctrl + 4) == g_end2, "ordered: the walker is caught up with put");
    check(g_early_ms < LATE_MS / 2u, "ordered: with get at put a store does not wait");

    /* A walker thread (this one ran passes) never waits for itself, whatever is pending. */
    const u32 end3 = put_segment(g_end2, N + 2u, 3);
    vm_write32(g_ctrl + 0, end3);
    const unsigned long long t2 = now_ms();
    guest_store32(OTHER_EA, 9u);
    check(now_ms() - t2 < LATE_MS / 2u, "ordered: a walker thread's own store does not wait");

    /* The doorbell, counted over the whole run (the walker's own stores above ring nothing). The label store that
     * waited found the walker caught up, not parked on its label: it needs the pass (PASS), and nothing after.
     * unrung installs no hook, so every count is zero and the invariants above had to hold without one. */
    check(atomic_load(&g_rings[0]) == want_store,
          unrung ? "doorbell: with no hook installed nothing rang STORE"
                 : "doorbell: STORE rang for the guest's put store only");
    check(atomic_load(&g_rings[1]) == want_pass,
          unrung ? "doorbell: with no hook installed nothing rang PASS"
                 : "doorbell: PASS rang once, for the label store that waited (the one with get at put rang nothing)");
    check(atomic_load(&g_rings[2]) == 0u, "doorbell: nothing rang WORK");

    /* cellGcm_rsx_walker_pending: what the host may sleep through. `put` is at end3, the last pass ended caught up
     * with an older put. */
    check(cellGcm_rsx_walker_pending() == 1, "pending: put moved since the pass");
    cellGcm_rsx_process_fifo();
    check(cellGcm_rsx_walker_pending() == 0, "pending: parked at an ACQUIRE the label does not satisfy");

    /* Parked on LABEL_EA: a store to another label needs a pass and nothing more; the store to the label the walker
     * is parked on rings STORE as well (it lands after the wait, and satisfies the ACQUIRE). The thread stores while
     * this one walks. */
    pthread_t st;
    atomic_store(&g_ppu_done, 0);
    pthread_create(&st, NULL, store_thread, NULL);
    for (unsigned long long d = now_ms() + 5000; !atomic_load(&g_ppu_done) && now_ms() < d;) {
        cellGcm_rsx_process_fifo();
        usleep(500);
    }
    pthread_join(st, NULL);
    check(atomic_load(&g_rings[1]) == want_pass_end,
          "doorbell: PASS rang for each of the two stores that waited");
    check(atomic_load(&g_rings[0]) == want_store_end,
          "doorbell: STORE rang once more, for the store to the label the walker is parked on only");
    check(vm_read32(LABEL_EA) == N + 2u, "pending: the store to the parked label landed");
    cellGcm_rsx_process_fifo();
    check(vm_read32(g_ctrl + 4) == end3 && cellGcm_rsx_walker_pending() == 0, "pending: caught up, nothing to do");

    /* ---- the pending-state table: every way a pass can leave work, each in the one case a pass would act ---- */
    {
        /* A non-empty fence queue: a pass publishes exactly ONE queued fence, so two leave one behind for the next
         * pass, and that pass empties it. get == put and no flip, so the queue is the only reason. */
        u32 io = end3, v = 0x51u;
        io = put_cmd(io, 0, 0x50u, 1, &v);          /* NV406E SET_REFERENCE */
        v = 0x52u;
        io = put_cmd(io, 0, 0x50u, 1, &v);
        vm_write32(g_ctrl + 0, io);
        cellGcm_rsx_process_fifo();
        check(cellGcm_rsx_walker_pending() == 1, "pending: a fence is still queued for publication");
        cellGcm_rsx_process_fifo();
        check(cellGcm_rsx_walker_pending() == 0, "pending: the fence queue is empty and nothing else is due");

        /* A flip head with commands behind it: the pass stops at the head (MORE) and the flip is pending, so the
         * host comes back to present and to drain the rest. */
        (void)cellGcmSetDisplayBuffer(0, 0x100000u, 1280u * 4u, 1280u, 720u);   /* the head resolves a buffer */
        u32 qbuf = 0u;
        io = put_cmd(io, 7, 0xE944u, 1, &qbuf);      /* GCM_DRIVER_QUEUE: the buffer the head flips to */
        u32 arg = 0x8000010Fu;                       /* GCM_FLIP_HEAD, buffer 0 */
        io = put_cmd(io, 7, 0xE924u, 1, &arg);
        const u32 flip_end = put_cmd(io, 7, 0x0B00u, 1, &arg);   /* a probe behind the head: work left */
        vm_write32(g_ctrl + 0, flip_end);
        cellGcm_rsx_process_fifo();
        check(cellGcm_rsx_walker_pending() == 1, "pending: a pass stopped behind a flip head with commands unread");
        /* Drain the rest: now get == put and only the pending flip keeps the walker awake (the present). */
        cellGcm_rsx_process_fifo();
        check(vm_read32(g_ctrl + 4) == flip_end && cellGcm_rsx_walker_pending() == 1,
              "pending: a flip to present is due with the commands drained");
        check(cellGcm_take_flip_pending_synced() == 1, "pending: the due flip was taken for its present");
        check(cellGcm_rsx_walker_pending() == 0, "pending: presented, drained, idle");

        /* A self-JUMP park: the walker stops on it every pass and a guest patches it in ring memory, which no hook
         * can see, so the host must keep polling at its cadence. */
        const u32 park_io = flip_end, park_ea = IO_ADDR + park_io;
        vm_write32(park_ea, 0x20000000u | park_io);  /* JUMP to itself */
        s_fifo_getoff = park_io;
        vm_write32(g_ctrl + 4, park_io);
        vm_write32(g_ctrl + 0, park_io + 8u);        /* put beyond the park */
        cellGcm_rsx_process_fifo();
        check(s_fifo_getoff == park_io, "pending: the pass stopped at the self-JUMP");
        check(cellGcm_rsx_walker_pending() == 1, "pending: a self-JUMP park is polled for its patch");
        vm_write32(g_ctrl + 0, park_io);             /* put back at get: nothing left to do */
        check(cellGcm_rsx_walker_pending() == 1,
              "pending: the park is still the record until a pass runs again (poll, do not sleep)");
        cellGcm_rsx_process_fifo();                  /* get == put: the pass ends caught up */
        check(cellGcm_rsx_walker_pending() == 0, "pending: idle again, nothing to do");
    }



    printf("[gcm_label_store %s] %s (%d failed); rings store=%u pass=%u work=%u\n", mode,
           fails ? "FAIL" : "PASS", fails, atomic_load(&g_rings[0]), atomic_load(&g_rings[1]),
           atomic_load(&g_rings[2]));
    free(vm_base);
    return fails ? 1 : 0;
}
