/* #62: two-sided stencil's back-face write mask (0x034C), reference (0x0354)
 * and compare mask (0x0358), through the real draw engine and the Metal
 * backend on the GPU. No title data.
 *
 * One full-screen triangle writes the stencil with distinct front and back
 * state; the same geometry is drawn once with FRONT_FACE CW and once with
 * CCW, so it is front-facing in one run and back-facing in the other
 * whatever the engine's Y convention. A probe pass (two-sided off, EQUAL,
 * colour on) then lights the left half if the stencil holds the value the
 * back state produces and the right half if it holds the front state's.
 * Over the two windings both must appear: before the fix the back faces
 * reused the front mask and reference, so both runs gave the front value.
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

enum { EQUAL = 0x202, ALWAYS = 0x207, KEEP = 0x1E00, REPLACE = 0x1E01 };

/* One face's stencil block: write mask, func, ref, func mask, fail/zfail/zpass. */
typedef struct { u32 write_mask, func, ref, func_mask, fail, zpass; } face;

static void put_face(u32 mask_reg, u32 block, const face* f)
{
    method(mask_reg, f->write_mask);
    method(block, f->func); method(block + 4, f->ref); method(block + 8, f->func_mask);
    method(block + 12, f->fail); method(block + 16, f->fail); method(block + 20, f->zpass);
}

/* Returns 'B' if only the back value is in the stencil, 'F' if only the
 * front one, '-' for neither and '2' for both. */
static char run(u32 front_face, u32 clear_stencil, const face* front, const face* back,
                u32 back_value, u32 front_value)
{
    if (rsx_draw_engine_init(64, 64) != 0) { CHECK(0, "engine init"); return '?'; }
    method(0x0194, 0xFEED0000);                 /* CONTEXT_DMA_COLOR_A: local */
    method(0x0198, 0xFEED0000);                 /* CONTEXT_DMA_ZETA: local */
    method(0x0210, 0x40000);  method(0x020C, 64 * 4);
    method(0x0214, 0x80000);  method(0x022C, 64 * 4);
    method(0x0200, 64u << 16); method(0x0204, 64u << 16);
    method(0x0208, 8u | (2u << 5) | (1u << 8)); /* A8R8G8B8, Z24S8, linear */
    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80010000);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80010010);
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80010020);
    const float v[36] = { -1, -1, .5f, 1,  1, 1, 1, 1,  0, 0, 0, 1,
                           3, -1, .5f, 1,  1, 1, 1, 1,  0, 0, 0, 1,
                          -1,  3, .5f, 1,  1, 1, 1, 1,  0, 0, 0, 1 };
    for (u32 i = 0; i < 36; i++) be_float(vm_base + 0x10000 + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0x0B80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + 0x60000, RSX_TEST_FP_SWZ_IDENT);
    method(0x08E4, 0x60002);
    method(0x1D60, 64);
    method(0x0A74, 0);                          /* DEPTH_TEST_ENABLE off */
    method(0x183C, 0);                          /* CULL_FACE_ENABLE off */
    method(0x08C0, 64u << 16); method(0x08C4, 64u << 16);
    method(0x1D90, 0);
    method(0x1D8C, 0xFFFFFF00u | clear_stencil);
    method(0x1D94, 0xF3);                       /* clear colour, depth, stencil */

    /* The pass under test: stencil only. */
    method(0x1834, front_face);
    method(0x0324, 0);                          /* COLOR_MASK: none */
    method(0x0328, 1);                          /* STENCIL_TEST_ENABLE */
    method(0x0348, 1);                          /* TWO_SIDED_STENCIL_TEST_ENABLE */
    put_face(0x032C, 0x0330, front);
    put_face(0x034C, 0x0350, back);
    draw();

    /* The probe: front state on both faces, EQUAL against one value per half. */
    face p = { 0, EQUAL, 0, 0xFF, KEEP, KEEP };
    method(0x0348, 0);
    method(0x0324, 0x01010101);
    p.ref = back_value;  put_face(0x032C, 0x0330, &p);
    method(0x08C0, 32u << 16); draw();
    p.ref = front_value; put_face(0x032C, 0x0330, &p);
    method(0x08C0, (32u << 16) | 32u); draw();

    s_engine_backend.submit_and_wait(NULL, 0);
    static u8 px[64 * 64 * 4];
    s_engine_backend.readback(NULL, g.surfaces[0].handle, 0, 0, 64, 64, px, 64 * 4);
    const int b = px[(32 * 64 + 16) * 4] != 0, f = px[(32 * 64 + 48) * 4] != 0;
    CHECK(s_eng_dropped == 0, "no dropped records");
    rsx_draw_engine_shutdown();
    return b && f ? '2' : b ? 'B' : f ? 'F' : '-';
}

static void both_faces(const char* what, u32 clear_stencil, const face* front, const face* back,
                       u32 back_value, u32 front_value)
{
    const char cw = run(0x0900, clear_stencil, front, back, back_value, front_value);
    const char ccw = run(0x0901, clear_stencil, front, back, back_value, front_value);
    CHECK((cw == 'B' && ccw == 'F') || (cw == 'F' && ccw == 'B'),
          "%s: one winding leaves the back value %02X, the other the front value %02X "
          "(CW %c, CCW %c)", what, back_value, front_value, cw, ccw);
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

        /* Write mask and reference: REPLACE writes ref & write mask. */
        const face f1 = { 0xFF, ALWAYS, 0x3C, 0xFF, REPLACE, REPLACE };
        const face b1 = { 0x0F, ALWAYS, 0xA5, 0xFF, REPLACE, REPLACE };
        both_faces("BACK_STENCIL_MASK and BACK_STENCIL_FUNC_REF", 0x00, &f1, &b1, 0xA5 & 0x0F, 0x3C);

        /* Compare mask alone (same reference on both faces): the stencil holds
         * 0x0F; 0x4F EQUAL under mask 0x0F passes and REPLACEs, under 0xFF it
         * fails and KEEPs. */
        const face f2 = { 0xFF, EQUAL, 0x4F, 0xFF, KEEP, REPLACE };
        const face b2 = { 0xFF, EQUAL, 0x4F, 0x0F, KEEP, REPLACE };
        both_faces("BACK_STENCIL_FUNC_MASK", 0x0F, &f2, &b2, 0x4F, 0x0F);

        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
    }
    printf("%u checks, %u failed (two-sided stencil back state, #62)\n", checks, failures);
    return failures ? 1 : 0;
}
