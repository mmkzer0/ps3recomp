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
#include "../cellGcmSys.c"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint8_t* vm_base;
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
        vm_write32(g_ctrl + 0, g_end2);
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

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* mode = argc > 1 ? argv[1] : "ordered";
    if (argc > 2) g_timeout_ms = (unsigned)atoi(argv[2]);
    const int no_walker = !strcmp(mode, "no-walker");
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
          (ppu_store_window >> 32) == CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE,
          "the first pass registered the store hook for the 256-label window");

    pthread_t ppu;
    pthread_create(&ppu, NULL, ppu_thread, NULL);
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

    printf("[gcm_label_store ordered] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
