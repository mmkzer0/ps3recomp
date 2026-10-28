/* #64: what NV4097_INVALIDATE_L2 (0x1FD8) means for the engine's texture
 * cache, through the real draw engine and the Metal backend on the GPU. No
 * title data.
 *
 * A texture at a fixed local offset is sampled by a draw into the left half
 * of a 64x64 target. Its bytes are then rewritten in place (same offset,
 * format and size, as a PPU write between two FIFO segments would), and a
 * second draw samples it into the right half. Run once with INVALIDATE_L2
 * between the rewrite and the second draw, once without.
 *
 * The contract the method row claims (INERT: "the engine keeps no such
 * cache"): with the invalidate, the second draw shows the new bytes; without
 * it, the result is the same.
 */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"

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

#define TEX_OFFSET 0x20000u
#define TEX_SIZE   8u

/* An 8x8 A8R8G8B8 texture of one colour, in guest (big-endian ARGB) order. */
static void fill_texture(u32 argb)
{
    for (u32 i = 0; i < TEX_SIZE * TEX_SIZE; i++) {
        u8* p = vm_base + TEX_OFFSET + i * 4;
        p[0] = argb >> 24; p[1] = argb >> 16; p[2] = argb >> 8; p[3] = argb;
    }
}

/* The readback is RGBA. */
static u32 argb_at(const u8* px, u32 x, u32 y)
{
    const u8* p = px + (y * 64 + x) * 4;
    return (u32)p[3] << 24 | (u32)p[0] << 16 | (u32)p[1] << 8 | p[2];
}

/* Left half sampled before the rewrite, right half after; returns the right
 * half's centre colour and stores the left one. */
static u32 run(int invalidate, u32 before, u32 after, u32* left)
{
    *left = 0;
    if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return 0; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0210, 0x40000);                    /* COLOR_A_OFFSET */
    method(0x020C, 64 * 4);                     /* COLOR_A_PITCH */
    method(0x0200, 64u << 16);
    method(0x0204, 64u << 16);
    method(0x0208, 0x148);                      /* A8R8G8B8, Z24S8, linear */
    /* position, colour, texcoord: float32 x4, stride 48, in main memory */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[36] = { -1, -1, 0, 1,  1, 1, 1, 1,  .5f, .5f, 0, 1,
                           3, -1, 0, 1,  1, 1, 1, 1,  .5f, .5f, 0, 1,
                          -1,  3, 0, 1,  1, 1, 1, 1,  .5f, .5f, 0, 1 };
    for (u32 i = 0; i < 36; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_tex_r0_tc0(vm_base + 0x60000, 0);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x0A74, 0);                          /* DEPTH_TEST_ENABLE off */
    method(0x1D90, 0);
    method(0x1D94, 0xF0);                       /* clear colour */

    fill_texture(before);
    method(0x1A00, TEX_OFFSET);                 /* TEXTURE_OFFSET[0] */
    method(0x1A04, 1u | (2u << 4) | (0xA5u << 8) | (1u << 16));   /* local, 2D, A8R8G8B8 linear, 1 level */
    method(0x1A0C, 0x80000000);                 /* CONTROL0: enable */
    method(0x1A10, 0xAAE4);                     /* identity remap */
    method(0x1A18, (TEX_SIZE << 16) | TEX_SIZE);
    method(0x1840, TEX_SIZE * 4);               /* pitch */
    method(0x08C0, 32u << 16); method(0x08C4, 64u << 16);
    draw();

    fill_texture(after);
    if (invalidate) method(0x1FD8, 1);          /* NV4097_INVALIDATE_L2 */
    method(0x08C0, (32u << 16) | 32u);
    draw();

    s_engine_backend.submit_and_wait(NULL, 0);
    static u8 px[64 * 64 * 4];
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, 64, 64, px, 64 * 4);
    *left = argb_at(px, 16, 32);
    const u32 right = argb_at(px, 48, 32);
    CHECK(s_eng_dropped == 0, "no dropped records");
    rsx_draw_engine_shutdown();
    return right;
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

        const u32 old_c = 0xFF204060u, new_c = 0xFFA0B0C0u;
        u32 left_inv, left_plain;
        const u32 with = run(1, old_c, new_c, &left_inv);
        const u32 without = run(0, old_c, new_c, &left_plain);
        CHECK(left_inv == old_c && left_plain == old_c,
              "first draw samples the original bytes (%08X, %08X)", left_inv, left_plain);
        CHECK(with == new_c,
              "after the rewrite and INVALIDATE_L2 the second draw samples the new bytes: got %08X, want %08X",
              with, new_c);
        CHECK(with == without,
              "the result does not depend on INVALIDATE_L2: with %08X, without %08X", with, without);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (INVALIDATE_L2 contract, #64)\n", checks, failures);
    return failures ? 1 : 0;
}
