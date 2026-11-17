/* Backend init behind the host GPU gate: an app started in the background (iOS refuses every GPU allocation to an
 * inactive app) must not fail init for good. With the gate suspended, rsx_metal_backend_init waits before it
 * creates the command queue, the depth texture and the placeholders; once the gate resumes it completes.
 *
 * No window: PS3RECOMP_METAL_HEADLESS=1. The draw engine is stubbed out to the backend's own eng_init, as in
 * test_metal_host_surface.m. Skips (77) without a Metal device. The gate's parked-thread count (the test's only
 * ordering signal: no sleeps) is read from the included backend's own gate.
 *
 *   clang -fobjc-arc -I include -I libs/video libs/video/tests/test_metal_init_gate.m ... (as the other fixtures)
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <sched.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { (void)b; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b) { (void)b; }
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return 1; }
int  rsx_draw_engine_init(u32 w, u32 h) { return eng_init(NULL, w, h); }
void rsx_draw_engine_shutdown(void) { eng_shutdown(NULL); }
int  rsx_draw_engine_present(void) { return 1; }   /* the stub engine always presents */
void rsx_draw_engine_require_represent(void) {}
u32  rsx_draw_engine_readback_center(void) { return 0; }
u32  rsx_draw_engine_guest_draws(void) { return 0; }
int  cellGcmOffsetIsDisplay(u32 off) { (void)off; return 1; }
u32  cellGcmResolveLocated(int location, u32 offset) { (void)location; return offset; }
u32  cellGcmResolveOffset(u32 offset) { return offset; }
void ps3_host_quit(const char* why) { (void)why; }
u8*  vm_base;
/* Reached from the included backend, not from init: stubs so the link finds them. */
int  rsx_draw_engine_frame_identity(rsx_frame_identity* out) { (void)out; return 0; }
u32  rsx_draw_engine_present_diag(void) { return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }
int  rsx_capture_install_from_config(const rsx_capture_environment* env) { (void)env; return 0; }
void cellGcm_set_host_pause_clock(unsigned long long (*paused_ms)(void)) { (void)paused_ms; }

static atomic_int g_done;
static int g_rc = -1;

static void* init_thread(void* arg)
{
    (void)arg;
    g_rc = rsx_metal_backend_init(64, 64, "init gate");
    atomic_store_explicit(&g_done, 1, memory_order_release);
    return NULL;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> probe = MTLCreateSystemDefaultDevice();
        if (!probe) { puts("no Metal device: skipped"); return 77; }
        setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);

        /* The app started in the background: the gate is closed before init begins. */
        rsx_metal_backend_gpu_suspend();
        pthread_t t;
        assert(pthread_create(&t, NULL, init_thread, NULL) == 0);

        /* Init is parked at the gate (the only ordering signal), not failed and not finished. */
        for (unsigned spin = 0; spin < 200000000u; spin++) {
            if (host_gpu_gate_waiters(&s_gpu_gate) == 1 || atomic_load_explicit(&g_done, memory_order_acquire)) break;
            sched_yield();
        }
        assert(!atomic_load_explicit(&g_done, memory_order_acquire));
        assert(host_gpu_gate_waiters(&s_gpu_gate) == 1);
        assert(s_queue == nil && s_depth == nil && s_null_tex == nil);   /* nothing was created while closed */
        puts("[PASS] init waits at the gate while the app is inactive");

        rsx_metal_backend_gpu_resume();
        assert(pthread_join(t, NULL) == 0);
        assert(g_rc == 0);
        assert(s_queue != nil && s_depth != nil && s_null_tex != nil);
        puts("[PASS] init completes once the gate resumes");

        rsx_metal_backend_shutdown();
    }
    puts("[metal_init_gate] PASS");
    return 0;
}
