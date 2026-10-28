/*
 * ps3recomp - a recording stub backend for the register-file draw engine tests
 *
 * The engine is driven through rsx_draw_engine_method() exactly as the FIFO
 * walker drives it, against a backend that records what it was asked for and
 * allocates nothing. No GPU, no host graphics API, no game content: the guest
 * memory below is a flat synthetic buffer.
 *
 * Every test executable includes this header once, so the guest-memory symbols
 * the engine links against live here and the stub's state is that test's own.
 */
#ifndef PS3RECOMP_RSX_DRAW_ENGINE_TEST_STUB_H
#define PS3RECOMP_RSX_DRAW_ENGINE_TEST_STUB_H

#include "rsx_draw_engine.h"
#include "rsx_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Main memory at the bottom, local video memory above it: the two number
 * spaces the engine resolves RSX offsets in. */
#define STUB_GUEST_BYTES (16u << 20)
#define STUB_LOCAL_EA    (8u << 20)
#define STUB_VTX_OFFSET  0x00010000u     /* main memory, clear of ea 0 */

/* ---- guest memory -------------------------------------------------------- */

static u8* stub_guest;

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? STUB_LOCAL_EA + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < STUB_LOCAL_EA ? offset : 0;
}

/* No cellGcmSys here: nothing was registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void) {}

static void stub_guest_init(void)
{
    stub_guest = (u8*)calloc(1, STUB_GUEST_BYTES);
    if (!stub_guest) { printf("[FAIL] guest memory\n"); exit(1); }
    vm_base = stub_guest;
    ppu_vm_size = STUB_GUEST_BYTES;
    /* Three vertices of float4 1.0, so the fetch has real data to read. */
    for (u32 i = 0; i < 3 * 4; i++) {
        u8* p = stub_guest + STUB_VTX_OFFSET + i * 4;
        p[0] = 0x3F; p[1] = 0x80; p[2] = 0; p[3] = 0;
    }
}

/* ---- the stub backend ---------------------------------------------------- */

typedef struct {
    u32 next_handle;
    int fail_pipelines;              /* every pipeline_create returns 0 */
    int fail_color;                  /* every color_target_create returns 0 */
    int n_pipeline_create, n_pipeline_fail;
    int n_draw, n_present, n_clear_color, n_clear_ds, n_readback;
    u32 last_present_surface;
    u32 presented[16];
    u32 next_present;
    u32 bound_targets[RSX_BE_MAX_COLOR_TARGETS];
    u32 bound_target_count;
    u32 draw_vertices;
    rsx_topology draw_topology;
    u8 readback_byte;                /* what readback fills rows with */
} stub_state;

static stub_state stub;

static u32 stub_handle(void)
{
    return ++stub.next_handle;
}

static int stub_init(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; return 0; }
static void stub_shutdown(void* u) { (void)u; }
static void stub_submit(void* u, u32 reason) { (void)u; (void)reason; }

static u32 stub_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips,
                               u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces; (void)remap; (void)rsx_fmt;
    return stub_handle();
}

static void stub_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                                const void* src, u32 row, u32 rows)
{ (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h; (void)src; (void)row; (void)rows; }

static void stub_texture_release(void* u, u32 t) { (void)u; (void)t; }

static u32 stub_color_create(void* u, rsx_be_format f, u32 w, u32 h,
                             const void* seed, u32 seed_row)
{
    (void)u; (void)f; (void)w; (void)h; (void)seed; (void)seed_row;
    if (stub.fail_color) return 0;
    return stub_handle();
}

static void stub_color_release(void* u, u32 s) { (void)u; (void)s; }

static u32 stub_depth_create(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; return stub_handle(); }

static void stub_depth_release(void* u, u32 d) { (void)u; (void)d; }

static u32 stub_pipeline_create(void* u, const char* vs, const char* ps,
                                const rsx_be_render_state* rs,
                                const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt, u32 rt_count)
{
    (void)u; (void)rs; (void)layout; (void)stride; (void)rt; (void)rt_count;
    stub.n_pipeline_create++;
    if (stub.fail_pipelines || !vs || !ps || !*vs || !*ps) {
        stub.n_pipeline_fail++;
        return 0;
    }
    return stub_handle();
}

static void stub_pipeline_release(void* u, u32 p) { (void)u; (void)p; }

static void stub_bind_targets(void* u, const u32* s, u32 count, u32 depth)
{
    (void)u; (void)depth;
    stub.bound_target_count = count;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        stub.bound_targets[i] = i < count ? s[i] : 0;
}
static void stub_bind_pipeline(void* u, u32 p) { (void)u; (void)p; }
static void stub_bind_vs(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void stub_bind_ps(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void stub_bind_textures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; (void)t; (void)s; (void)m; }
static void stub_bind_vtextures(void* u, const u32* t, const rsx_be_sampler_desc* s, u32 m)
{ (void)u; (void)t; (void)s; (void)m; }
static void stub_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void stub_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }
static void stub_stencil_ref(void* u, u32 r, u32 back) { (void)u; (void)r; (void)back; }

static void stub_draw(void* u, rsx_topology topology, const void* verts,
                      u32 count, u32 stride, const u32* indices, u32 index_count)
{
    (void)u; (void)verts; (void)stride; (void)indices; (void)index_count;
    stub.n_draw++;
    stub.draw_topology = topology;
    stub.draw_vertices = count;
}

static void stub_clear_color(void* u, u32 s, const float rgba[4])
{ (void)u; (void)s; (void)rgba; stub.n_clear_color++; }

static void stub_clear_ds(void* u, u32 d, u32 flags, float z, u8 s)
{ (void)u; (void)d; (void)flags; (void)z; (void)s; stub.n_clear_ds++; }

static void stub_present(void* u, u32 surface)
{
    (void)u;
    stub.n_present++;
    stub.last_present_surface = surface;
    if (stub.next_present < 16) stub.presented[stub.next_present++] = surface;
}

static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                          void* out, u32 out_pitch)
{
    (void)u; (void)s; (void)x; (void)y;
    stub.n_readback++;
    for (u32 row = 0; row < h; row++)
        memset((u8*)out + (size_t)row * out_pitch, stub.readback_byte, (size_t)w * 4u);
}

static const rsx_draw_backend stub_backend = {
    .user                = NULL,
    .init                = stub_init,
    .shutdown            = stub_shutdown,
    .submit_and_wait     = stub_submit,
    .texture_create      = stub_texture_create,
    .texture_upload      = stub_texture_upload,
    .texture_release     = stub_texture_release,
    .color_target_create = stub_color_create,
    .color_target_release = stub_color_release,
    .depth_target_create = stub_depth_create,
    .depth_target_release = stub_depth_release,
    .pipeline_create     = stub_pipeline_create,
    .pipeline_release    = stub_pipeline_release,
    .bind_targets        = stub_bind_targets,
    .bind_pipeline       = stub_bind_pipeline,
    .bind_vs_constants   = stub_bind_vs,
    .bind_ps_constants   = stub_bind_ps,
    .bind_textures       = stub_bind_textures,
    .bind_vertex_textures = stub_bind_vtextures,
    .set_viewport        = stub_viewport,
    .set_scissor         = stub_scissor,
    .set_stencil_ref     = stub_stencil_ref,
    .draw                = stub_draw,
    .clear_color         = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .present             = stub_present,
    .readback            = stub_readback,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_SURFACE_FORMAT      0x0208
#define M_COLOR_A_OFFSET      0x0210
#define M_COLOR_TARGET        0x0220
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_GCM_QUEUE_FLIP      0xE944
#define M_GCM_FLIP_HEAD       0xE924
#define DMA_LOCAL             0xFEED0000u

static void sm(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

/* A 64x64 colour surface at local offset 0x40000, and a float4 vertex array
 * in main memory: the state every draw in these tests starts from. */
static void stub_engine_up(int with_surface)
{
    memset(&stub, 0, sizeof stub);
    stub.readback_byte = 0x40;
    rsx_draw_engine_set_backend(&stub_backend);
    rsx_draw_engine_reset_draw_stats();
    if (rsx_draw_engine_init(64, 64) != 0) {
        printf("[FAIL] engine init\n");
        exit(1);
    }
    if (with_surface) {
        sm(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
        sm(M_SURFACE_CLIP_H, 64u << 16);
        sm(M_SURFACE_CLIP_V, 64u << 16);
        sm(M_COLOR_A_OFFSET, 0x40000u);
    }
    sm(M_VTXFMT, 2u | (4u << 4) | (16u << 8));       /* float32 x4, stride 16 */
    sm(M_VTXBUF_OFFSET, 0x80000000u | STUB_VTX_OFFSET);
}

static void stub_engine_down(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}

static void stub_begin_end(u32 prim)
{
    sm(M_BEGIN_END, prim);
    sm(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    sm(M_BEGIN_END, 0u);
}

/* One flip through the FIFO words the walker feeds: the queue word names the
 * display buffer, the head word (bit 31) is the present. */
static void stub_flip(u32 buffer)
{
    sm(M_GCM_QUEUE_FLIP, buffer & 7u);
    sm(M_GCM_FLIP_HEAD, 0x8000010Fu);
}

#endif /* PS3RECOMP_RSX_DRAW_ENGINE_TEST_STUB_H */
