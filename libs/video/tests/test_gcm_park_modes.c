/* One synthetic parked FIFO, run in both park modes (cellGcmSys.c; docs/CONFIG.md gcm.park_resync).
 *
 * A self-JUMP park (an Edge geometry hole, or a segment link a title patches last) with `put` past it, and
 * a command between the park and put that the guest "has not finished linking in":
 *
 *   io 0x00  probe 1      executed before the park
 *   io 0x08  JUMP 0x08    the park
 *   io 0x0C  probe 2      the command between the park and put: executed only if the park is patched
 *   io 0x14  JUMP 0x14    a second park (a second round, Compatibility only)
 *   io 0x18  NOP          put of the second round is 0x1C
 *
 * argv[1] = reference | compat, argv[2] = gcm.wait_timeout_ms, argv[3] = gcm.park_grace_ms.
 *
 *   reference  GET never moves to put, however long the park stands (the grace, a third of the timeout
 *              here, passes many times over); the walker is polled until the named stop arrives, exit 70,
 *              which tools/test_gcm_park_modes.py checks (what=jump-park, waited at least the timeout).
 *   compat     after the grace, GET moves to put; probe 2 is bypassed; the intervention is logged with the
 *              mode and the effective grace (the driver reads stderr: one line per intervention, two
 *              rounds here). Exit 0.
 *
 * The clock is the only thing that ends a park here, so the walker is polled against a bounded deadline;
 * no pass is ordered by a sleep.
 *
 * cc -std=gnu17 -I include -I libs/video libs/video/tests/test_gcm_park_modes.c runtime/memory/label_window.c
 *   libs/video/gcm_default_state.c -ffunction-sections -fdata-sections -Wl,--gc-sections -pthread
 *   (clang on macOS: -Wl,-dead_strip)   (tools/test_gcm_park_modes.py)
 */
#include "../cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
/* The probe methods the walker executed, in order. */
#define PROBE_METHOD 0x1EE0u
static u32 t_probe[8], t_probes;
int  rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    (void)state;
    if (method == PROBE_METHOD && t_probes < 8) t_probe[t_probes++] = data;
    return 0;
}
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
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

/* The registry, as gcm_fixture_stubs.h models it, and the stop the Reference mode ends in. A fixture stop
 * says what it was and ends the process with the runtime's stop status. */
#include "gcm_fixture_stubs.h"
static int t_reference;
#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00100000u
#define CMD_SIZE  0x00010000u

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

static int t_fails;
static void check(int ok, const char* what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    t_fails += !ok;
    if (!ok && t_reference) { fflush(stdout); exit(1); }   /* the poll loop below would repeat it */
}

static u32 put_probe(u32 io, u32 arg)
{
    gw32(IO_ADDR + io, (1u << 18) | PROBE_METHOD);
    gw32(IO_ADDR + io + 4, arg);
    return io + 8;
}

static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
/* Let the clock run between two walker passes; the pass itself is what is asserted, not the delay. */
static void tick(void)
{
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4 || (strcmp(argv[1], "reference") && strcmp(argv[1], "compat"))) {
        fprintf(stderr, "usage: %s reference|compat <wait_timeout_ms> <park_grace_ms>\n", argv[0]);
        return 2;
    }
    t_reference = !strcmp(argv[1], "reference");
    ps3_config* cfg = ps3_fixture_config();
    cfg->gcm_park_resync = !t_reference;
    cfg->gcm_wait_timeout_ms = (uint32_t)atoi(argv[2]);
    cfg->gcm_park_grace_ms = (uint32_t)atoi(argv[3]);

    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    u32 io = put_probe(0, 1);
    const u32 park = io;
    gw32(IO_ADDR + io, 0x20000000u | io); io += 4;
    io = put_probe(io, 2);
    const u32 put = io;
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, put);

    cellGcm_rsx_process_fifo();
    check(t_probes == 1 && t_probe[0] == 1u && s_fifo_getoff == park, "the walker parks at the self-JUMP");

    const unsigned long long t0 = now_ms();
    const unsigned long long deadline = t0 + (unsigned long long)cfg->gcm_wait_timeout_ms + 5000ull;

    if (t_reference) {
        /* Poll past the grace many times over: GET must stay on the park every time. The only way out is the
         * named stop (exit 70, from the stub in gcm_fixture_stubs.h) after gcm.wait_timeout_ms. */
        while (now_ms() < deadline) {
            cellGcm_rsx_process_fifo();
            check(s_fifo_getoff == park && vm_read32(ctrl + 4) != put && t_probes == 1,
                  "Reference: GET stays on the park and the command after it is not executed");
            tick();
        }
        puts("[FAIL] Reference: the park stood past the timeout and no stop arrived");
        return 1;
    }

    /* Compatibility: the park stands past the grace, then GET follows put and probe 2 is bypassed. */
    while (s_fifo_getoff != put && now_ms() < deadline) {
        cellGcm_rsx_process_fifo();
        if (s_fifo_getoff != put) check(s_fifo_getoff == park, "Compatibility: GET waits on the park within the grace");
        tick();
    }
    check(s_fifo_getoff == put, "Compatibility: after the grace GET moved to put");
    check(now_ms() - t0 >= cfg->gcm_park_grace_ms, "...not before the grace elapsed");
    check(t_probes == 1, "...and the command between the park and put was bypassed");

    /* A second round: a new park at put, another put past it. Every intervention is logged. */
    const u32 park2 = put;
    gw32(IO_ADDR + park2, 0x20000000u | park2);
    const u32 put2 = park2 + 8;
    gw32(IO_ADDR + park2 + 4, 0);
    vm_write32(ctrl + 0, put2);
    cellGcm_rsx_process_fifo();
    check(s_fifo_getoff == park2, "a second park is waited on again");
    const unsigned long long t1 = now_ms();
    while (s_fifo_getoff != put2 && now_ms() < t1 + (unsigned long long)cfg->gcm_wait_timeout_ms) {
        cellGcm_rsx_process_fifo();
        tick();
    }
    check(s_fifo_getoff == put2, "...and left by a second intervention");

    printf("[gcm_park_modes compat] %s (%d failed)\n", t_fails ? "FAIL" : "PASS", t_fails);
    free(vm_base);
    return t_fails ? 1 : 0;
}
