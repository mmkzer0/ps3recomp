/* The Metal backend's BC1/2/3 emulation, through the backend's own texture hooks (eng_texture_create,
 * eng_texture_upload, the cube and 3D variants), under each rsx.bc_path value the first argument names (the
 * argument, not the registry's environment override, so the fixture reads no environment knob of its own):
 *
 *   cpu | gpu | tiered   the texture is RGBA8Unorm and every level, cube face and 3D slice holds exactly what
 *                        rsx_bc_decode makes of the blocks uploaded; the uploads ran on the tiers the contract
 *                        says (cpu: all CPU; gpu: all GPU while the texture is fresh; tiered: every level of a
 *                        texture whose largest level has rsx.bc_gpu_min_texels texels or more on the GPU, every
 *                        level of a smaller one on the CPU), and left one PS3_FTRACE BC_UPLOAD span each, saying so
 *   auto (or none)       this Mac samples BC: the texture stays a BC texture, nothing is decoded, no span
 *
 * The GPU tier's decode is read back by a blit committed on the backend's queue right after the upload with no
 * wait in between, as a draw's command buffer follows it: a queue that did not run them in order would read
 * the texture before the decode wrote it. A texture that has seen a commit since its creation takes the CPU
 * tier even under `gpu` (a rewrite would race the draws recorded against it). Synthetic blocks only. Skips (77)
 * without a Metal device. */
#define RSX_METAL_BC_TEST_HOOKS 1   /* the counters of CPU writes against pending GPU decodes */
#include "../rsx_metal_backend.m"
#include "rsx_bc_test_corpus.h"

#include <stdio.h>
#include <string.h>

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
int  rsx_draw_engine_frame_identity(rsx_frame_identity* out) { (void)out; return 0; }
u32  rsx_draw_engine_present_diag(void) { return 0; }
void rsx_fetch_attrib(const rsx_state* st, int idx, u32 vi, float out[4])
{ (void)st; (void)idx; (void)vi; out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; }
int  rsx_capture_install_from_config(const rsx_capture_environment* env) { (void)env; return 0; }
void cellGcm_set_host_active_clock(unsigned long long (*active_ms)(void)) { (void)active_ms; }

static unsigned s_failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (s_failures++ < 20) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                 fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } \
} while (0)

typedef struct { rsx_bc_format bc; rsx_be_format be; u32 rsx_fmt; } fmt_row;
static const fmt_row k_formats[3] = {
    { RSX_BC_FORMAT_BC1, RSX_BE_FMT_BC1, 0xA6 },
    { RSX_BC_FORMAT_BC2, RSX_BE_FMT_BC2, 0xA7 },
    { RSX_BC_FORMAT_BC3, RSX_BE_FMT_BC3, 0xA8 },
};

/* What each upload should have been: the contract of rsx.bc_path, stated apart from the implementation. */
typedef struct { u32 format, texels, tier; } expected_upload;
static expected_upload s_expect[4096];
static unsigned s_expect_n;
static int s_emulating;
static int s_path;   /* ENG_BC_PATH_* of the run; 0 = auto on this Mac */

/* The tier is a property of the texture: its largest level (the creation size) decides it for every level, face
 * and slice. A texture that has seen a commit since its creation is CPU whatever the path says. */
static u32 expect_tier(u32 texture_texels, int fresh)
{
    if (s_path == ENG_BC_PATH_CPU || !fresh) return 1;
    if (s_path == ENG_BC_PATH_TIERED && texture_texels < 1024u) return 1;
    return 2;
}

static void note_upload(const fmt_row* f, u32 texture_texels, u32 w, u32 h, int fresh)
{
    if (!s_emulating) return;
    s_expect[s_expect_n++] = (expected_upload){ (u32)f->bc, w * h, expect_tier(texture_texels, fresh) };
}

static uint8_t* read_back(id<MTLTexture> t, NSUInteger slice, NSUInteger level, NSUInteger z, u32 w, u32 h)
{
    id<MTLBuffer> buf = [s_dev newBufferWithLength:(NSUInteger)w * h * 4u options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:slice sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, z)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:(NSUInteger)w * 4u destinationBytesPerImage:(NSUInteger)w * h * 4u];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    uint8_t* out = malloc((size_t)w * h * 4u);
    memcpy(out, [buf contents], (size_t)w * h * 4u);
    return out;
}

static void expect_level(const char* what, id<MTLTexture> t, NSUInteger slice, NSUInteger level, NSUInteger z,
                         const rsx_bc_test_image* im, rsx_bc_format bc)
{
    if (!s_emulating) return;
    uint8_t* want = malloc((size_t)im->width * im->height * 4u);
    CHECK(rsx_bc_decode(im->bytes, im->size, im->width, im->height, bc, want, (size_t)im->width * 4u),
          "%s: the CPU decoder refused the level", what);
    uint8_t* got = read_back(t, slice, level, z, im->width, im->height);
    size_t bad = 0;
    for (size_t i = 0; i < (size_t)im->width * im->height * 4u; ++i)
        if (got[i] != want[i] && bad++ < 3)
            fprintf(stderr, "  %s level %lu slice %lu z %lu byte %zu: got %u want %u\n", what, (unsigned long)level,
                    (unsigned long)slice, (unsigned long)z, i, got[i], want[i]);
    CHECK(bad == 0, "%s %ux%u level %lu: %zu bytes differ", what, im->width, im->height, (unsigned long)level, bad);
    free(got);
    free(want);
}

static void check_pixel_format(const char* what, id<MTLTexture> t, const fmt_row* f)
{
    if (s_emulating)
        CHECK([t pixelFormat] == MTLPixelFormatRGBA8Unorm, "%s: an emulated BC texture is RGBA8Unorm", what);
    else
        CHECK([t pixelFormat] == eng_pixel_format(f->be), "%s: a native BC texture keeps its BC format", what);
}

static u32 chain_levels(u32 w, u32 h)
{
    u32 n = 1;
    while ((w >> n) || (h >> n)) ++n;
    return n;
}

static void scenario_2d(const fmt_row* f, u32 w, u32 h, uint32_t* rng)
{
    const u32 levels = chain_levels(w, h);
    const u32 handle = eng_texture_create(NULL, f->be, w, h, levels, 1, 0xAAE4, f->rsx_fmt);
    CHECK(handle != 0, "create %ux%u", w, h);
    id<MTLTexture> t = eng_obj(handle);
    check_pixel_format("2D", t, f);
    rsx_bc_test_image im[16];
    for (u32 m = 0; m < levels; ++m) {
        const u32 lw = w >> m ? w >> m : 1u, lh = h >> m ? h >> m : 1u;
        CHECK(rsx_bc_test_make_random_image(&im[m], f->bc, lw, lh, rng), "level image");
        eng_texture_upload(NULL, handle, 0, m, lw, lh, im[m].bytes, (u32)im[m].row_bytes, im[m].blocks_h);
        note_upload(f, w * h, lw, lh, 1);
    }
    for (u32 m = 0; m < levels; ++m) {
        expect_level("2D", t, 0, m, 0, &im[m], f->bc);
        rsx_bc_test_image_free(&im[m]);
    }
    eng_obj_release(NULL, handle);
}

static void scenario_cube(const fmt_row* f, u32 side, uint32_t* rng)
{
    const u32 levels = chain_levels(side, side);
    const u32 handle = eng_texture_create(NULL, f->be, side, side, levels, 6, 0xAAE4, f->rsx_fmt);
    CHECK(handle != 0, "create cube");
    id<MTLTexture> t = eng_obj(handle);
    CHECK([t textureType] == MTLTextureTypeCube, "a six-face texture is a cube");
    check_pixel_format("cube", t, f);
    rsx_bc_test_image im[6][16];
    for (u32 face = 0; face < 6; ++face) {
        for (u32 m = 0; m < levels; ++m) {
            const u32 l = side >> m ? side >> m : 1u;
            CHECK(rsx_bc_test_make_random_image(&im[face][m], f->bc, l, l, rng), "cube image");
            eng_texture_upload(NULL, handle, face, m, l, l, im[face][m].bytes, (u32)im[face][m].row_bytes,
                               im[face][m].blocks_h);
            note_upload(f, side * side, l, l, 1);
        }
    }
    for (u32 face = 0; face < 6; ++face)
        for (u32 m = 0; m < levels; ++m) {
            expect_level("cube", t, face, m, 0, &im[face][m], f->bc);
            rsx_bc_test_image_free(&im[face][m]);
        }
    eng_obj_release(NULL, handle);
}

static void scenario_3d(const fmt_row* f, u32 w, u32 h, u32 depth, uint32_t* rng)
{
    const u32 handle = eng_texture_create_3d(NULL, f->be, w, h, depth, 2, 0xAAE4, f->rsx_fmt);
    CHECK(handle != 0, "create 3D");
    id<MTLTexture> t = eng_obj(handle);
    CHECK([t textureType] == MTLTextureType3D, "a volume is a 3D texture");
    check_pixel_format("3D", t, f);
    rsx_bc_test_image im[2][8];
    u32 d = depth;
    for (u32 m = 0; m < 2; ++m, d = d > 1 ? d >> 1 : 1u) {
        const u32 lw = w >> m ? w >> m : 1u, lh = h >> m ? h >> m : 1u;
        for (u32 z = 0; z < d; ++z) {
            CHECK(rsx_bc_test_make_random_image(&im[m][z], f->bc, lw, lh, rng), "slice image");
            eng_texture_upload_3d(NULL, handle, z, m, lw, lh, im[m][z].bytes, (u32)im[m][z].row_bytes);
            note_upload(f, w * h, lw, lh, 1);
        }
    }
    d = depth;
    for (u32 m = 0; m < 2; ++m, d = d > 1 ? d >> 1 : 1u)
        for (u32 z = 0; z < d; ++z) {
            expect_level("3D", t, 0, m, z, &im[m][z], f->bc);
            rsx_bc_test_image_free(&im[m][z]);
        }
    eng_obj_release(NULL, handle);
}

/* A texture a commit has gone past is not fresh: its upload takes the CPU tier whatever rsx.bc_path says. */
static void scenario_stale(const fmt_row* f, uint32_t* rng)
{
    const u32 handle = eng_texture_create(NULL, f->be, 64, 64, 1, 1, 0xAAE4, f->rsx_fmt);
    CHECK(handle != 0, "create stale");
    eng_encode_and_commit(nil);
    rsx_bc_test_image im;
    CHECK(rsx_bc_test_make_random_image(&im, f->bc, 64, 64, rng), "stale image");
    eng_texture_upload(NULL, handle, 0, 0, 64, 64, im.bytes, (u32)im.row_bytes, im.blocks_h);
    note_upload(f, 64u * 64u, 64, 64, 0);
    expect_level("stale", eng_obj(handle), 0, 0, 0, &im, f->bc);
    rsx_bc_test_image_free(&im);
    eng_obj_release(NULL, handle);
}

static void scenario_exhaustive(void)
{
    rsx_bc_test_image bc3;
    CHECK(rsx_bc_test_make_bc3_alpha_image(&bc3), "exhaustive BC3 alpha");
    if (!bc3.bytes) return;
    const fmt_row* f = &k_formats[2];
    const u32 handle = eng_texture_create(NULL, f->be, bc3.width, bc3.height, 1, 1, 0xAAE4, f->rsx_fmt);
    CHECK(handle != 0, "create exhaustive");
    eng_texture_upload(NULL, handle, 0, 0, bc3.width, bc3.height, bc3.bytes, (u32)bc3.row_bytes, bc3.blocks_h);
    note_upload(f, bc3.width * bc3.height, bc3.width, bc3.height, 1);
    expect_level("exhaustive BC3 alpha", eng_obj(handle), 0, 0, 0, &bc3, f->bc);
    eng_obj_release(NULL, handle);
    rsx_bc_test_image_free(&bc3);
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        id<MTLDevice> probe = MTLCreateSystemDefaultDevice();
        if (!probe) { puts("no Metal device: skipped"); return 77; }
        setenv("PS3RECOMP_METAL_HEADLESS", "1", 1);
        setenv("PS3_FTRACE", "0:100000:/dev/null", 1);
        if (rsx_metal_backend_init(64, 64, "bc emulation") != 0) { fprintf(stderr, "backend init failed\n"); return 1; }

        /* What the draw engine would hand over from the registry (rsx.bc_path; 1024 texels splits a 64x64 chain
         * between the tiers), then the policy the first BC texture would read, now, so the run's tier is known
         * up front. */
        const char* want_path = argc > 1 && argv[1][0] ? argv[1] : "auto";
        eng_bc_set_policy(NULL, want_path, 1024);
        eng_bc_policy_read();
        s_emulating = s_eng_bc_emulate;
        s_path = s_emulating ? s_eng_bc_path : 0;
        CHECK(eng_device_supports_bc(), "a Mac samples BC natively");
        CHECK(s_emulating == (strcmp(want_path, "auto") != 0), "emulation follows rsx.bc_path (%s)", want_path);
        (void)PS3_FT_ON();

        uint32_t rng = 0x6D2B79F5u;
        for (unsigned i = 0; i < 3; ++i) {
            const fmt_row* f = &k_formats[i];
            scenario_2d(f, 64, 64, &rng);       /* 64x64 .. 1x1: levels either side of the 1024-texel split */
            scenario_2d(f, 127, 129, &rng);     /* partial edge blocks at every level */
            scenario_2d(f, 1, 1, &rng);
            scenario_cube(f, 32, &rng);
            scenario_3d(f, 16, 16, 4, &rng);
            scenario_stale(f, &rng);
        }
        scenario_exhaustive();

        unsigned cpu = 0, gpu = 0;
        for (unsigned i = 0; i < s_expect_n; ++i) {
            if (s_expect[i].tier == 1) cpu++; else gpu++;
        }
        CHECK(s_eng_bc_tier_uploads[1] == cpu && s_eng_bc_tier_uploads[2] == gpu,
              "uploads per tier: cpu %u gpu %u, expected cpu %u gpu %u", s_eng_bc_tier_uploads[1],
              s_eng_bc_tier_uploads[2], cpu, gpu);
        if (s_emulating && s_path == ENG_BC_PATH_TIERED)
            CHECK(cpu > 0 && gpu > 0, "tiered ran both tiers (cpu %u gpu %u)", cpu, gpu);
        if (s_emulating && s_path == ENG_BC_PATH_GPU)
            CHECK(gpu > 0 && s_eng_bc_gpu != nil, "gpu ran the GPU tier");
        if (s_emulating && s_path == ENG_BC_PATH_CPU)
            CHECK(gpu == 0 && s_eng_bc_gpu == nil, "cpu never built the GPU decoder");
        if (!s_emulating)
            CHECK(s_expect_n == 0 && s_eng_bc_tier_uploads[1] == 0 && s_eng_bc_tier_uploads[2] == 0 &&
                  s_eng_bc_gpu == nil, "native BC decodes nothing");

        /* One BC_UPLOAD span per emulated upload, in order: who = format, a = texels, b = tier. */
        unsigned spans = 0;
        for (uint32_t i = 0; i < g_ps3_ft.n; ++i) {
            const ps3_ft_rec* r = &g_ps3_ft.buf[i];
            if (r->kind != PS3_FT_BC_UPLOAD) continue;
            if (spans < s_expect_n)
                CHECK(r->who == s_expect[spans].format && r->a == s_expect[spans].texels && r->b == s_expect[spans].tier &&
                      r->t1 >= r->t0, "span %u: format %u texels %u tier %u, expected %u %u %u", spans, r->who, r->a,
                      r->b, s_expect[spans].format, s_expect[spans].texels, s_expect[spans].tier);
            spans++;
        }
        CHECK(spans == s_expect_n, "%u BC_UPLOAD spans for %u emulated uploads", spans, s_expect_n);
        CHECK([s_eng_bc_gpu failureCount] == 0, "a GPU decode failed");
        CHECK(s_test_bc_cpu_write_hazards == 0 && s_test_bc_guard_waits == 0,
              "no CPU write met a pending GPU decode of its texture (hazards %u, guard waits %u)",
              s_test_bc_cpu_write_hazards, s_test_bc_guard_waits);
        CHECK(!__atomic_load_n(&s_eng_encoding_failed, __ATOMIC_RELAXED), "the backend marked itself failed");

        printf("[metal_bc_emulation] path=%s emulating=%d uploads cpu=%u gpu=%u spans=%u failures=%u\n", want_path,
               s_emulating, cpu, gpu, spans, s_failures);
        rsx_metal_backend_shutdown();
    }
    return s_failures ? 1 : 0;
}
