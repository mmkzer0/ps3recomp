/* gcm.handler_thread delivers the guest's vblank/flip/user handlers on a dedicated interrupt thread. They are guest
 * code, so that thread needs the host stack a guest PPU thread gets (ppu.host_stack_mb, sys_ppu_thread.c): it was
 * started with 1 MB, and a handler whose call chain went deeper than that overflowed it.
 *
 * Includes the real implementation, registers a vblank handler, marks a vblank pending and pumps from the main thread
 * (which only starts the interrupt thread). The handler checks it is not on the main thread, reads its own stack
 * size, and then runs a 16 MB deep call chain, which a 1 MB stack cannot hold. The size is compared with
 * ps3_ppu_host_stack_bytes(), the configured value, not with a constant: the fixture sets ppu.host_stack_mb to 64,
 * neither the 1 MB the thread once had nor the 256 MB desktop default, so a stack that ignores the option fails.
 *
 * tools/test_gcm_handler_thread.py builds and runs it. */
#include "../libs/video/cellGcmSys.c"
#include "../libs/video/tests/gcm_fixture_stubs.h"
#include "../libs/video/rsx_tiling.c"        /* cellGcmSys.c publishes the bound tiles here (v0.19.0 #569) */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

/* GCM_REPORT_SPU_HOLD_MS (gcm.report_spu_hold_ms) asks the SPU task layer whether a task round is
 * running; there are no SPU tasks here. */
int spu_taskset_round_busy(uint32_t* ts, uint32_t* tid) { (void)ts; (void)tid; return 0; }

uint8_t* vm_base;
uint32_t ppu_vm_size;
uint64_t ps3_system_time_us(void) { return 0; }   /* sys_timer.c */
uint32_t g_ww_lo, g_ww_hi;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
int spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
/* The rest of the walker's world: no graphics backend, no lifted program (as libs/video/tests/test_gcm_handler_mask.c). */
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; (void)method; (void)data; return 0; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
int  rsx_draw_engine_enabled(void) { return 0; }
rsx_backend* rsx_get_backend(void) { return NULL; }
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

#define FIXTURE_HOST_STACK_MB 64u

static unsigned long s_main_tid;
static _Atomic unsigned long s_handler_tid;
static _Atomic size_t s_handler_stack;
static atomic_int s_handler_done;

static size_t my_stack_size(void)
{
#if defined(__APPLE__)
    return pthread_get_stacksize_np(pthread_self());
#else
    pthread_attr_t a;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &a) != 0) return 0;
    pthread_attr_getstacksize(&a, &sz);
    pthread_attr_destroy(&a);
    return sz;
#endif
}

/* 64 KB of frame per level, 256 levels: a 16 MB chain, as a deep guest handler makes on the host stack. */
static __attribute__((noinline)) unsigned deep(unsigned n)
{
    volatile unsigned char frame[64 * 1024];
    frame[0] = (unsigned char)n;
    frame[sizeof frame - 1] = (unsigned char)n;
    return n ? deep(n - 1) + frame[0] + frame[sizeof frame - 1] : 0;
}

static void vblank_handler(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    (void)opd; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (atomic_load_explicit(&s_handler_done, memory_order_acquire)) return;
    atomic_store_explicit(&s_handler_tid, (unsigned long)GetCurrentThreadId(), memory_order_relaxed);
    atomic_store_explicit(&s_handler_stack, my_stack_size(), memory_order_relaxed);
    (void)deep(256);
    atomic_store_explicit(&s_handler_done, 1, memory_order_release);
}
ps3_guest_caller_fn g_ps3_guest_caller = vblank_handler;

int main(void)
{
    /* gcm.handler_thread on, and a host stack that is neither the 1 MB of the old thread nor the default. */
    ps3_fixture_config()->gcm_handler_thread = true;
    ps3_fixture_config()->ppu_host_stack_mb = FIXTURE_HOST_STACK_MB;
    const size_t want = ps3_ppu_host_stack_bytes();
    if (want != (size_t)FIXTURE_HOST_STACK_MB * 1024u * 1024u) {
        fprintf(stderr, "FAIL: ps3_ppu_host_stack_bytes() is %zu, not the %u MB the fixture configured\n", want,
                FIXTURE_HOST_STACK_MB);
        return 1;
    }

    s_main_tid = (unsigned long)GetCurrentThreadId();
    s_vblank_handler_opd = 0x1000;
    cellGcm_vblank_signal();   /* counts the vblank and marks it pending: the pump calls the handler once per vblank */
    ppu_gcm_pump();   /* not the interrupt thread: starts it and returns */
    for (int i = 0; i < 5000 && !atomic_load_explicit(&s_handler_done, memory_order_acquire); i++) usleep(1000);

    int fails = 0;
    if (!atomic_load_explicit(&s_handler_done, memory_order_acquire)) {
        fprintf(stderr, "FAIL: the vblank handler never ran\n");
        return 1;
    }
    if (atomic_load_explicit(&s_handler_tid, memory_order_relaxed) == s_main_tid) {
        fprintf(stderr, "FAIL: the handler ran on the main thread\n");
        fails++;
    }
    const size_t got = atomic_load_explicit(&s_handler_stack, memory_order_relaxed);
    if (got < want) {
        fprintf(stderr, "FAIL: the interrupt thread has a %zu KB stack, want the %zu KB (ppu.host_stack_mb) a PPU "
                "thread gets\n", got / 1024, want / 1024);
        fails++;
    }
    if (fails) return 1;
    printf("PASS: GCM handlers run on an interrupt thread with a guest PPU thread's host stack\n");
    return 0;
}
