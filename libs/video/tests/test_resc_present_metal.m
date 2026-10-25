/* The RESC output path on a real Metal device: a composed surface, the library's convert-and-flip
 * stream sampling it as a texture into the display buffer, and the present of that buffer.
 *
 * Synthetic, no title data. A gradient is drawn into a 1280x720 A8R8G8B8 surface (the "source"),
 * cellRescSetSrc / SetBufferAddress / SetConvertAndFlip then build the stream the library emits,
 * and the test feeds it to the real draw engine and Metal backend. The display buffer must carry
 * the source's pixels: the convert program runs only if SetBufferAddress copied its microcode into
 * the fragment buffer, so a library that leaves that buffer as the title left it (zeros) presents
 * black.
 *
 * Build and run: tools/test_metal_resc_present.py, or the CMake target test_resc_present_metal. */
#include "rsx_metal_backend.m"
#define eng_texture_upload test_engine_texture_upload
#define eng_present test_engine_present
#include "rsx_draw_engine.c"
#undef eng_texture_upload
#undef eng_present
#include "rsx_test_programs.h"
#include "cellResc.h"
#include "cellGcmSys.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdlib.h>

/* ---- the guest: one flat block; a local offset is the same number as the guest address ------- */

#define GUEST_SIZE   0x01000000u
#define W            1280u
#define H            720u
#define PITCH        5120u
#define SRC_OFF      0x00400000u   /* the composed surface */
#define DISP_OFF     0x00800000u   /* RESC colour buffer 0 = display buffer 0 */
#define VTX_EA       0x00C00000u   /* RESC quad vertices */
#define FRAG_EA      0x00C10000u   /* RESC fragment-program buffer */
#define CFG_EA       0x00020000u
#define SRC_REC_EA   0x00020100u
#define SCENE_VTX    0x00010000u   /* compose triangle (main-memory location) */
#define SCENE_FP     0x00060000u

u8* vm_base;
float cellAvconfExt_display_gamma(void) { return 1.0f; }
u32 ppu_vm_size;
u32 g_ww_lo = 1, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t a) { (void)a; }

/* The engine's guest-memory resolution: a flat guest. */
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset < ppu_vm_size ? offset : 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}

/* Stops are failures of this test. */
static void die(const char* f, const char* a, const char* r)
{
    fprintf(stderr, "STOP %s %s: %s\n", f, a, r);
    abort();
}
void ps3_stop_guest_range(const char* f, const char* a, uint64_t ea, uint64_t size, uint64_t limit)
{
    fprintf(stderr, "STOP guest range %s %s ea=%llx size=%llx limit=%llx\n", f, a,
            (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
    abort();
}
void ps3_stop_hle_uninitialized(const char* f, const char* m, const char* r) { die(f, m, r); }
void ps3_stop_gcm_unmodelled(const char* f, const char* feature, const char* r) { die(f, feature, r); }
u32 ps3_process_sdk_version(void) { return 0x00400000u; }

/* The included backend's legacy fixed-function path references these. Debug builds dead-strip that
 * path; at -O3 the references survive in functions the linker keeps (CI release job). The engine
 * path under test never calls them, so reaching one is a test failure. */
u32 cellGcmResolveOffset(u32 offset) { (void)offset; die("cellGcmResolveOffset", "legacy", "reached"); return 0; }
int cellGcmOffsetIsDisplay(u32 offset) { (void)offset; die("cellGcmOffsetIsDisplay", "legacy", "reached"); return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{
    (void)st; (void)idx; (void)vi; (void)out;
    die("rsx_fetch_attrib", "legacy", "reached");
}

/* libgcm as the library sees it: display buffers go to the engine, the stream goes to the engine's
 * method decoder. The flip command is recorded; the test presents explicitly. */
static u32 g_flip_buffer = 0xFFFFFFFFu;
s32 cellGcmSetDisplayBuffer(u32 id, u32 offset, u32 pitch, u32 w, u32 h)
{
    rsx_draw_engine_set_display_buffer(id, 0, offset, pitch, w, h);
    return 0;
}
u32 cellGcmGetTiledPitchSize(u32 size) { return size; }
s32 cellGcm_address_to_offset(u32 ea, u32* off) { *off = ea; return 0; }
s32 cellGcm_emit_flip_command(u32 ctx, u32 id) { (void)ctx; g_flip_buffer = id; return 0; }
void cellGcm_ctx_emit(u32 ctx, const u32* w, u32 n)
{
    (void)ctx;
    for (u32 i = 0; i < n;) {
        const u32 hdr = w[i++];
        const u32 count = (hdr >> 18) & 0x7FFu;
        u32 method = hdr & 0x3FFCu;
        for (u32 k = 0; k < count && i < n; k++, i++) {
            rsx_draw_engine_method(method, w[i]);
            if (!(hdr & 0x40000000u)) method += 4;
        }
    }
}
void cellGcmResetFlipStatus(void) {}
u32 cellGcmGetFlipStatus(void) { return 0; }
u64 cellGcmGetLastFlipTime(void) { return 0; }
void cellGcmSetFlipHandler(CellGcmFlipHandler h) { (void)h; }
void cellGcmSetVBlankHandler(CellGcmVBlankHandler h) { (void)h; }

static unsigned checks, failures;
#define T(x, label) do { checks++; if (!(x)) { failures++; if (failures < 20) fprintf(stderr, "FAIL %s\n", label); } } while (0)

static void method(u32 a, u32 v) { rsx_draw_engine_method(a, v); }
static void bf(u8* p, float f) { u32 v; memcpy(&v, &f, 4); p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* The composed frame: a clear to opaque black, then one full-screen triangle whose three vertex
 * colours differ, so every pixel of the source is distinct from its neighbours' region and from the
 * other corners. */
static void compose(void)
{
    method(0x194, 0xFEED0000);
    method(0x210, SRC_OFF); method(0x20C, PITCH);
    method(0x200, W << 16); method(0x204, H << 16);
    method(0x208, 0x148);
    method(0x8C0, W << 16); method(0x8C4, H << 16);
    method(0x1D90, 0xFF000000); method(0x1D94, 0xF0);

    method(0x1740, 2u | (4u << 4) | (48u << 8)); method(0x1680, 0x80000000u | SCENE_VTX);
    method(0x1744, 2u | (4u << 4) | (48u << 8)); method(0x1684, 0x80000000u | (SCENE_VTX + 0x10));
    method(0x1760, 2u | (4u << 4) | (48u << 8)); method(0x16A0, 0x80000000u | (SCENE_VTX + 0x20));
    const float v[36] = { -1, -1, .5f, 1,   1, 0, 0, 1,   0, 0, 0, 1,
                           3, -1, .5f, 1,   0, 1, 0, 1,   0, 0, 0, 1,
                          -1,  3, .5f, 1,   0, 0, 1, 1,   0, 0, 0, 1 };
    for (u32 i = 0; i < 36; i++) bf(vm_base + SCENE_VTX + i * 4, v[i]);
    u8 vp[48];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 1, RSX_TEST_VP_SWZ_IDENT, 1, 0);
    rsx_test_vp_mov_out(vp + 32, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    method(0x1E9C, 0);
    for (u32 i = 0; i < 12; i++) { u32 w; memcpy(&w, vp + i * 4, 4); method(0xB80 + i * 4, w); }
    method(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + SCENE_FP, RSX_TEST_FP_SWZ_IDENT);
    method(0x8E4, SCENE_FP | 2u); method(0x1D60, 64);
    method(0xA74, 0); method(0xA70, 0);           /* no depth test, no depth write */
    method(0x1808, RSX_PRIMITIVE_TRIANGLES); method(0x1814, 2u << 24); method(0x1808, 0);
}

static void put_cfg(void)
{
    const u32 cfg[7] = { 28, 2, CELL_RESC_1280x720, 0, 0, 0, 0 };
    for (u32 i = 0; i < 7; i++) vm_write32(CFG_EA + i * 4u, cfg[i]);
}

static u32 surface_slot(u32 offset)
{
    for (u32 i = 0; i < g.n_surfaces; i++)
        if (g.surfaces[i].handle && g.surfaces[i].location == 0 && g.surfaces[i].offset == offset) return i;
    return ENG_INVALID;
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
        T(rsx_draw_engine_init(W, H) == 0, "engine init");

        compose();

        /* The library's side: what a title does once per mode change and once per frame. */
        put_cfg();
        T(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "cellRescInit");
        T(cellRescSetDisplayMode(CELL_RESC_1280x720) == 0, "cellRescSetDisplayMode");
        T(cellRescSetBufferAddress((void*)(uintptr_t)DISP_OFF, (void*)(uintptr_t)VTX_EA,
                                   (void*)(uintptr_t)FRAG_EA) == 0, "cellRescSetBufferAddress");
        vm_write32(SRC_REC_EA + 0, 0xA5); vm_write32(SRC_REC_EA + 4, PITCH);
        vm_write16(SRC_REC_EA + 8, W); vm_write16(SRC_REC_EA + 10, H); vm_write32(SRC_REC_EA + 12, SRC_OFF);
        T(cellRescSetSrc(0, (const CellRescSrc*)(uintptr_t)SRC_REC_EA) == 0, "cellRescSetSrc");
        T(cellRescSetConvertAndFlip((void*)(uintptr_t)0x1000, 0) == 0, "cellRescSetConvertAndFlip");
        T(g_flip_buffer == 0, "the flip names colour buffer 0");

        rsx_draw_engine_present_buffer(g_flip_buffer);
        s_engine_backend.submit_and_wait(NULL, 0);

        const u32 src = surface_slot(SRC_OFF), dst = surface_slot(DISP_OFF);
        T(src != ENG_INVALID && dst != ENG_INVALID, "source and display surfaces are registered");
        T(dst != ENG_INVALID && g.last_present_surface == dst, "the presented surface is the display buffer");
        T(s_eng_dropped == 0, "no dropped records");
        if (src != ENG_INVALID && dst != ENG_INVALID) {
            static u8 a[H * PITCH], b[H * PITCH];
            s_engine_backend.readback(NULL, g.surfaces[src].handle, 0, 0, W, H, a, PITCH);
            s_engine_backend.readback(NULL, g.surfaces[dst].handle, 0, 0, W, H, b, PITCH);
            /* The composed frame is a real gradient: three corners carry three different primaries. */
            const u8* c0 = a + 0 * PITCH;                    /* top-left */
            const u8* c1 = a + (H - 1) * PITCH + 0;          /* bottom-left */
            T(a[(H / 2) * PITCH + (W / 2) * 4] || a[(H / 2) * PITCH + (W / 2) * 4 + 1] ||
              a[(H / 2) * PITCH + (W / 2) * 4 + 2], "the composed surface is not black");
            T(memcmp(c0, c1, 4) != 0, "the composed surface is not uniform");
            /* 1:1 point-aligned linear sampling: the display buffer is the source, to the rounding
             * of the sampler. */
            u32 worst = 0; u64 sum = 0;
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W * 4; x++) {
                    const int d = abs((int)a[y * PITCH + x] - (int)b[y * PITCH + x]);
                    if ((u32)d > worst) worst = (u32)d;
                    sum += (u64)d;
                }
            const double mad = (double)sum / ((double)W * H * 4.0);
            fprintf(stderr, "display vs source: worst %u, mean abs diff %.4f\n", worst, mad);
            T(worst <= 1, "the display buffer equals the composed surface (worst channel error <= 1)");
        }
        rsx_draw_engine_shutdown();
        rsx_draw_engine_set_backend(NULL);
        free(vm_base);
        printf("RESC present checks=%u failures=%u\n", checks, failures);
        return failures ? 1 : 0;
    }
}
