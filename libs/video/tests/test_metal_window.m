/* #62: the coordinate-convention pair, through the real draw engine and the
 * Metal backend on the GPU. No title data.
 *
 * SET_WINDOW_OFFSET (0x02B8): x in bits 0-15, y in 16-31. It moves the
 * raster origin in memory: a 32x32 clip drawn with offset (16, 8) lands at
 * pitch * 8 + 16 * 4 bytes past COLOR_A_OFFSET. Observed through
 * rsx_draw_engine_guest_read, which stores the surface holding an address
 * into guest memory: one probe inside the shifted rectangle only, one inside
 * the unshifted one only. Offset 0 (what RESC and the default state send)
 * must leave the unshifted placement.
 *
 * SET_SHADER_WINDOW (0x1D88): height in bits 0-11, origin (1 = bottom) in
 * 12-15, pixel centre (1 = integer) in 16-19. It reaches WPOS only; the
 * program MOV r0, WPOS into an RGBA32F target reads back the convention at
 * one pixel for a top origin, a bottom origin and integer centres.
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include <math.h>

u8* vm_base; u32 ppu_vm_size;
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
float cellAvconfExt_display_gamma(void) { return 1.0f; }
u32 cellGcmResolveOffset(u32 offset) { return offset; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }

static unsigned checks, failures;
#define CHECK(x, ...) do { checks++; if (x) { printf("[PASS] " __VA_ARGS__); printf("\n"); } \
                           else { failures++; printf("[FAIL] " __VA_ARGS__); printf("\n"); } } while (0)

static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void be_float(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void draw(void) { method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0); }

#define BASE  0x40000u
#define PITCH 256u                              /* 64 pixels of 4 bytes */

/* A 32x32 clip of colour format `cfmt` at BASE with `pitch`, the given
 * fragment program at 0x60000, and one full-screen triangle of colour
 * (1, .5, .25, 1). */
static int setup(u32 cfmt, u32 pitch, void (*fp)(u8*))
{
    if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return 0; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0210, BASE);
    method(0x020C, pitch);
    method(0x0200, 32u << 16);
    method(0x0204, 32u << 16);
    method(0x0208, cfmt | (1u << 8));           /* no zeta, linear */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[36] = { -1, -1, 0, 1,  1, .5f, .25f, 1,  0, 0, 0, 1,
                           3, -1, 0, 1,  1, .5f, .25f, 1,  0, 0, 0, 1,
                          -1,  3, 0, 1,  1, .5f, .25f, 1,  0, 0, 0, 1 };
    for (u32 i = 0; i < 36; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    fp(vm_base + 0x60000);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x0A74, 0);                          /* DEPTH_TEST_ENABLE off */
    return 1;
}

static void fp_col0(u8* p) { rsx_test_fp_mov_r0_col0(p, RSX_TEST_FP_SWZ_IDENT); }

/* MOV r0, WPOS ; END */
static void fp_wpos(u8* p)
{
    rsx_test_fp_instr(p, RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV) | RSX_TEST_FP_MASK_XYZW |
                         RSX_TEST_FP_INSRC(0) | RSX_TEST_FP_END,
                         RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
}

/* The guest A8R8G8B8 word at pixel (x, y) of the BASE/PITCH grid after the
 * engine stored whatever surface holds it. */
static u32 guest_pixel(u32 location, u32 x, u32 y)
{
    const u32 off = BASE + y * PITCH + x * 4u;
    rsx_draw_engine_guest_read(location, off, PITCH, 1, 1);
    const u8* p = vm_base + off;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* Draws with WINDOW_OFFSET `win` and returns the guest words at the two
 * probes: `in_shifted` (40, 20), inside the (16, 8)-shifted rectangle only,
 * and `in_base` (4, 2), inside the unshifted one only. */
static void offset_run(u32 win, u32* in_shifted, u32* in_base)
{
    *in_shifted = *in_base = 0xDEADBEEFu;
    memset(vm_base + BASE, 0, 64u * PITCH);
    if (!setup(8u, PITCH, fp_col0)) return;
    method(0x02B8, win);
    draw();
    s_engine_backend.submit_and_wait(NULL, 0);
    const u32 location = g.n_surfaces ? g.surfaces[0].location : 0;
    *in_shifted = guest_pixel(location, 40, 20);
    *in_base = guest_pixel(location, 4, 2);
    CHECK(s_eng_dropped == 0, "no dropped records");
    rsx_draw_engine_shutdown();
}

/* WPOS.xy at pixel (3, 5) of a 32x32 RGBA32F target under SHADER_WINDOW `win`. */
static void wpos_run(u32 win, float out[2])
{
    out[0] = out[1] = -1.0f;
    if (!setup(0x0Cu, 32u * 16u, fp_wpos)) return;
    method(0x1D88, win);
    draw();
    s_engine_backend.submit_and_wait(NULL, 0);
    float px[4];
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 3, 5, 1, 1, px, sizeof px);
    out[0] = px[0]; out[1] = px[1];
    CHECK(s_eng_dropped == 0, "no dropped records");
    rsx_draw_engine_shutdown();
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 77; }
        s_queue = [s_dev newCommandQueue];
        s_headless = 1; s_guest_shaders = 1;
        vm_base = calloc(1, 1u << 20); ppu_vm_size = 1u << 20;
        rsx_draw_engine_set_backend(&s_engine_backend);

        const u32 drawn = 0xFFFF8040u;          /* (1, .5, .25, 1) as A8R8G8B8 */
        u32 shifted, base;
        offset_run(0, &shifted, &base);
        CHECK(base == drawn && shifted == 0,
              "WINDOW_OFFSET 0: the draw lands at COLOR_A_OFFSET (base %08X, shifted %08X)", base, shifted);
        offset_run((8u << 16) | 16u, &shifted, &base);
        CHECK(shifted == drawn && base == 0,
              "WINDOW_OFFSET (16, 8): the draw lands 8 rows and 16 pixels further (shifted %08X, base %08X)",
              shifted, base);

        float w[2];
        wpos_run(0x00000020u, w);               /* height 32, origin top, half centres */
        CHECK(w[0] == 3.5f && w[1] == 5.5f, "SHADER_WINDOW top/half: WPOS (%g, %g), want (3.5, 5.5)", w[0], w[1]);
        wpos_run(0x00001020u, w);               /* origin bottom */
        CHECK(w[0] == 3.5f && w[1] == 26.5f, "SHADER_WINDOW bottom/half: WPOS (%g, %g), want (3.5, 26.5)", w[0], w[1]);
        wpos_run(0x00010020u, w);               /* integer centres */
        CHECK(w[0] == 3.0f && w[1] == 5.0f, "SHADER_WINDOW top/integer: WPOS (%g, %g), want (3, 5)", w[0], w[1]);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (window offset and shader window, #62)\n", checks, failures);
    return failures ? 1 : 0;
}
