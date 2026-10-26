/*
 * The guest depth range on a real Metal device: SET_CLIP_MIN/MAX must reach MTLViewport.znear/zfar.
 *
 * Two synthetic triangles cover a 256x256 target, left and right, with clip-space z = 0 and z = 1. The
 * guest sets the depth range to [0.25, 0.75] (the pair libgcm writes with every cellGcmSetViewport),
 * depth test ALWAYS with depth writes on, and the depth buffer is read back:
 *
 *   left  pixel depth = near + 0 * (far - near) = 0.25
 *   right pixel depth = near + 1 * (far - near) = 0.75
 *
 * Before S1 the engine hard-coded [0, 1], so the same frame read 0.0 and 1.0; that is the fail-before.
 * Synthetic, no title data. Skips with 77 without a Metal device or the guest-shader translator.
 *
 * Build: the CMake target test_depth_range_metal (labels gpu;t0, Apple only).
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <math.h>
#include <stdlib.h>

#define GUEST_SIZE 0x01000000u
#define W 256u
#define H 256u
#define PITCH 1024u
#define COLOR_OFF 0x00400000u
#define ZETA_OFF  0x00500000u
#define VTX_EA    0x00C00000u
#define FRAG_EA   0x00C10000u

u8* vm_base;
u32 ppu_vm_size;
u32 g_ww_lo = 1, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}

static void die(const char* f, const char* a, const char* r)
{
    fprintf(stderr, "STOP %s %s: %s\n", f, a, r);
    abort();
}
void ps3_stop_guest_range(const char* f, const char* a, uint64_t ea, uint64_t size, uint64_t limit)
{ (void)f; (void)a; (void)ea; (void)size; (void)limit; die("guest-range", "span", "reached"); }
void ps3_stop_hle_uninitialized(const char* f, const char* m, const char* r) { die(f, m, r); }
void ps3_stop_gcm_unmodelled(const char* f, const char* feature, const char* r) { die(f, feature, r); }
void ps3_stop_rsx_capture(const char* f, const char* r, const char* d) { (void)d; die(f, r, "reached"); }
void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
              const ps3_stop_field* fields, size_t nfields, const ps3_stop_site* site)
{ (void)fields; (void)nfields; (void)site; die("stop", what, remedy); (void)kind; }
u32 ps3_process_sdk_version(void) { return 0x00400000u; }
/* The backend's other references: this test never takes those paths, so they end the test if reached. */
float cellAvconfExt_display_gamma(void) { return 1.0f; }
void cellGcm_set_capture_sink(const rsx_capture_sink* sink) { (void)sink; }
const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    cfg.gcm_wait_timeout_ms = 10000;
    return &cfg;
}
/* The backend's legacy vtable registration: this test drives the engine backend only. */
void rsx_set_backend(rsx_backend* backend) { (void)backend; }

/* Guest-memory and timing seams of the shared sources; the test's flat guest is its own memory. */
uint32_t ppu_hle_inject_base = 0x40000000u;
u32 gcm_io_offset_to_ea(u32 io) { return io < ppu_vm_size ? io : 0; }
unsigned long long ps3_ms_now(void) { return 0; }
void ps3_ms(const char* why) { (void)why; }
u32 cellGcmResolveOffset(u32 offset) { (void)offset; die("cellGcmResolveOffset", "legacy", "reached"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; die("cellGcmOffsetIsDisplay", "legacy", "reached"); return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; (void)out; die("rsx_fetch_attrib", "legacy", "reached"); }

static unsigned checks, failures;
#define T(x, label) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s\n", label); } } while (0)

static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void bf(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Left half (x in [-1, 0]) at depth z, or right half (x in [0, 1]) at depth z. Two triangles each:
 * a quad needs two, and the second one of the pair is degenerate at the screen edge. */
static void quad(u32 vtx_ea, int right, float z)
{
    const float x0 = right ? 0.0f : -1.0f, x1 = right ? 1.0f : 0.0f;
    const float v[6][8] = {
        { x0, -1.0f, z, 1.0f,   1, 0, 0, 1 },
        { x1, -1.0f, z, 1.0f,   0, 1, 0, 1 },
        { x0,  1.0f, z, 1.0f,   0, 0, 1, 1 },
        { x1, -1.0f, z, 1.0f,   0, 1, 0, 1 },
        { x1,  1.0f, z, 1.0f,   0, 0, 1, 1 },
        { x0,  1.0f, z, 1.0f,   1, 0, 0, 1 },
    };
    for (u32 i = 0; i < 6; i++)
        for (u32 k = 0; k < 8; k++) bf(vm_base + vtx_ea + (i * 8 + k) * 4, v[i][k]);
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("SKIP: no Metal device"); return 77; }
        s_queue = [s_dev newCommandQueue];
        s_headless = 1;
        if (!rsx_hlsl_to_msl_available()) { puts("SKIP: guest shader translation unavailable"); return 77; }
        s_guest_shaders = 1;
        vm_base = calloc(1, GUEST_SIZE); ppu_vm_size = GUEST_SIZE;
        rsx_draw_engine_set_backend(&s_engine_backend);
        if (rsx_draw_engine_init(W, H) != 0) { puts("FAIL: engine init"); return 1; }

        /* Target: A8R8G8B8 colour, Z24S8 depth, linear, clip = the whole surface. */
        method(0x194, 0xFEED0000);                      /* DMA_COLOR_A: local */
        method(0x198, 0xFEED0000);                      /* DMA_ZETA: local */
        method(0x210, COLOR_OFF); method(0x20C, PITCH);
        method(0x214, ZETA_OFF);  method(0x22C, PITCH);
        method(0x200, W << 16); method(0x204, H << 16);
        method(0x208, 0x1A8u);                          /* A8R8G8B8, Z24S8, linear  */
        method(0x220, 1);                               /* colour target A */
        method(0x8C0, W << 16); method(0x8C4, H << 16);
        method(0x1D90, 0xFF000000); method(0x1D94, 0x10);
        method(0xA74, 1); method(0xA70, 1); method(0xA6C, 0x207);   /* depth test ALWAYS, write on */

        /* The vertex layout: attr0 = position (4 f32), attr1 = colour (4 f32), stride 32, main memory. */
        method(0x1740, 2u | (4u << 4) | (32u << 8));
        method(0x1680, 0x80000000u | VTX_EA);
        method(0x1744, 2u | (4u << 4) | (32u << 8));
        method(0x1684, 0x80000000u | (VTX_EA + 16));
        u8 vp[32];
        rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
        rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
        method(0x1E9C, 0);
        for (u32 i = 0; i < 8; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0xB80 + i * 4, w); }
        method(0x1EA0, 0);
        rsx_test_fp_mov_r0_col0(vm_base + FRAG_EA, RSX_TEST_FP_SWZ_IDENT);
        method(0x8E4, FRAG_EA | 2u); method(0x1D60, 64);

        /* The depth range libgcm's cellGcmSetViewport writes: [0.25, 0.75]. */
        method(0x394, 0x3E800000u);
        method(0x398, 0x3F400000u);

        quad(VTX_EA, 0, 0.0f);                          /* left,  z = 0 */
        method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 5u << 24); method(0x1808, 0);
        quad(VTX_EA, 1, 1.0f);                          /* right, z = 1 */
        method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 5u << 24); method(0x1808, 0);
        rsx_draw_engine_present_buffer(0);
        s_engine_backend.submit_and_wait(NULL, 0);

        T(s_eng_dropped == 0, "no dropped records");
        const u32 zs = eng_zdepth_get(RSX_LOCATION_LOCAL, ZETA_OFF, W, H);
        T(zs != ENG_INVALID && g.zdepths[zs].had_write, "the draw wrote the depth target");
        if (zs != ENG_INVALID && s_engine_backend.depth_readback) {
            float d[4];
            const int ok = s_engine_backend.depth_readback(NULL, g.zdepths[zs].handle,
                                                           8, H / 2, W - 16, 1, d, (W - 16) * 4);
            T(ok == 0, "the depth buffer reads back");
            if (ok == 0) {
                const float left = d[W / 4 - 8], right = d[3 * W / 4 - 8];
                fprintf(stderr, "depth range [0.25,0.75]: left=%.4f right=%.4f\n", left, right);
                T(fabsf(left - 0.25f) <= 1.0f / 255.0f,
                  "the left half (clip z 0) sits at the near end of the guest range (0.25)");
                T(fabsf(right - 0.75f) <= 1.0f / 255.0f,
                  "the right half (clip z 1) sits at the far end (0.75)");
            }
        } else {
            T(0, "the backend can read depth back");
        }
        printf("depth range metal checks=%u failures=%u\n", checks, failures);
        fflush(stdout);
        /* The process is the fixture's unit: the engine's teardown releases host objects the Metal
         * validation layer still references when a depth readback was the last GPU work, and a crash
         * there would only hide the result. The OS reclaims everything. */
        _exit(failures ? 1 : 0);
    }
}
