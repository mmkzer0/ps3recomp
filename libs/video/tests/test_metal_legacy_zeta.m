/* The legacy (vtable) present path opens one Metal render pass per run of records aimed at the same
 * surfaces, and takes the pass's depth attachment from the record that opens it. Draws that name the same
 * colour surface but different zeta buffers therefore have to land in different passes, or the later draw
 * depth-tests and writes the earlier draw's depth surface. Observed on the zeta textures themselves after a
 * real present, on the path rsx_metal_backend_present takes when the draw engine is not active.
 *
 * Needs a Metal device, no window (PS3RECOMP_METAL_HEADLESS=1); skips (77) without one. The draw engine is
 * stubbed out as in test_metal_init_gate.m, except that it reports itself disabled so init picks the vtable
 * route.
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <math.h>
#include <stdio.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { (void)b; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b) { (void)b; }
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return 0; }
int  rsx_draw_engine_init(u32 w, u32 h) { (void)w; (void)h; return -1; }
void rsx_draw_engine_shutdown(void) {}
int  rsx_draw_engine_present(void) { return 1; }
void rsx_draw_engine_require_represent(void) {}
u32  rsx_draw_engine_readback_center(void) { return 0; }
u32  rsx_draw_engine_guest_draws(void) { return 0; }
/* Every colour offset the test uses is an offscreen surface. */
int  cellGcmOffsetIsDisplay(u32 off) { (void)off; return 0; }
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

#define SURF_W 32u
#define SURF_H 32u
#define COLOR_OFF 0x00100000u
#define ZETA_A    0x00200000u
#define ZETA_B    0x00300000u

/* One full-surface triangle at clip depth `z`, aimed at the colour surface and the given zeta buffer, with
 * the depth test ALWAYS and writes on: the depth that ends up in a zeta texture is then the last draw that
 * was encoded against it, whatever it held before. */
static void add_draw(u32 zeta_off, float z)
{
    assert(s_rec_count < MTL_MAX_RECORDS && s_vert_count + 3 <= MTL_MAX_VERTS);

    rsx_state st;
    memset(&st, 0, sizeof st);
    st.depth_test_enable = 1;
    st.depth_mask = 1;
    st.depth_func = 0x0207;                       /* ALWAYS */

    MtlRecord* r = &s_records[s_rec_count++];
    memset(r, 0, sizeof *r);
    r->kind = MTL_REC_DRAW;
    MtlDraw* d = &r->u.draw;
    d->vs_idx = d->fs_idx = -1;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) d->tex[u] = d->samp[u] = d->tex_rt[u] = -1;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++) d->vtex[u] = d->vsamp[u] = -1;
    d->ds_idx = ds_slot_for(&st);
    assert(d->ds_idx >= 0);
    d->base = s_vert_count;
    d->count = 3;
    d->topology = MTLPrimitiveTypeTriangle;
    d->color_mask = 0x01010101u;
    d->cull = MTLCullModeNone;
    d->winding = MTLWindingClockwise;
    d->mvp[0] = d->mvp[5] = d->mvp[10] = d->mvp[15] = 1.0f;
    d->rt.off = COLOR_OFF;
    d->rt.w = SURF_W; d->rt.h = SURF_H;
    d->rt.zeta_off = zeta_off;
    d->rt.vp_near = 0.0f; d->rt.vp_far = 1.0f;

    static const float xy[3][2] = { {-1, -1}, {3, -1}, {-1, 3} };
    for (int i = 0; i < 3; i++) {
        MtlVertex* v = &s_verts[s_vert_count++];
        memset(v, 0, sizeof *v);
        v->a[0][0] = xy[i][0]; v->a[0][1] = xy[i][1]; v->a[0][2] = z; v->a[0][3] = 1.0f;
        v->a[3][0] = 1.0f;     v->a[3][3] = 1.0f;
    }
}

/* Every texel of the zeta texture's depth plane equals `want`. */
static int depth_is(id<MTLTexture> t, float want)
{
    const NSUInteger row = 256, size = row * t.height;
    id<MTLBuffer> buf = [s_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [s_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    assert(buf && cb && blit);
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:size
                   options:MTLBlitOptionDepthFromDepthStencil];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    assert(cb.status == MTLCommandBufferStatusCompleted);
    for (NSUInteger y = 0; y < t.height; y++)
        for (NSUInteger x = 0; x < t.width; x++) {
            float got;
            memcpy(&got, (const unsigned char*)buf.contents + y * row + x * 4, 4);
            /* Negated so a NaN (what an unwritten depth plane may hold) is a mismatch, not a pass. */
            if (!(fabsf(got - want) <= 1e-6f)) {
                fprintf(stderr, "depth(%lu,%lu) = %g, want %g\n", (unsigned long)x, (unsigned long)y,
                        got, want);
                return 0;
            }
        }
    return 1;
}

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 77; }

        assert(rsx_metal_backend_init(64, 64, "legacy zeta") == 0);
        assert(!s_eng_active && s_headless);

        assert(surface_get(COLOR_OFF, SURF_W, SURF_H, 0) >= 0);
        assert(zeta_get(ZETA_A, SURF_W, SURF_H) && zeta_get(ZETA_B, SURF_W, SURF_H));

        /* A, B, A over one colour surface: each draw's depth must reach its own zeta buffer, and the second
         * visit to A must find A (not the buffer the pass before it opened with). */
        add_draw(ZETA_A, 0.25f);
        add_draw(ZETA_B, 0.75f);
        add_draw(ZETA_A, 0.5f);
        rsx_metal_backend_present();
        assert(s_rec_count == 0);                 /* the frame was consumed */

        assert(depth_is(zeta_get(ZETA_A, SURF_W, SURF_H), 0.5f));
        assert(depth_is(zeta_get(ZETA_B, SURF_W, SURF_H), 0.75f));

        rsx_metal_backend_shutdown();
        puts("Metal legacy zeta passes scenario passed");
    }
    return 0;
}
