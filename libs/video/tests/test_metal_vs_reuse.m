/* The draw engine's vertex-constant block kept in the staging arena: a draw
 * whose block is unchanged is pointed at the copy an earlier draw staged
 * (eng_reuse_vs_constants) instead of staging 8 KB again, and that is good only
 * while the arena the copy sits in is the one being recorded. Growing the arena
 * keeps its bytes at their offsets; a commit hands the arena to the command
 * buffer and the next recording starts in another, so a draw pointed at an old
 * offset would read bytes a later draw owns. Needs a Metal device, no window
 * (PS3RECOMP_METAL_HEADLESS=1); skips (77) without one. The draw engine is
 * stubbed out, as in test_metal_init_gate.m.
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
void rsx_set_backend(rsx_backend* b) { (void)b; }
void rsx_draw_engine_set_backend(const rsx_draw_backend* b) { (void)b; }
void rsx_draw_engine_set_default(int on) { (void)on; }
int  rsx_draw_engine_enabled(void) { return 1; }
int  rsx_draw_engine_init(u32 w, u32 h) { return eng_init(NULL, w, h); }
void rsx_draw_engine_shutdown(void) { eng_shutdown(NULL); }
int  rsx_draw_engine_present(void) { return 1; }
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

#define BLOCK_BYTES ((512u + 2u) * 16u)

int main(void)
{
    @autoreleasepool {
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev) { puts("no Metal device: skipped"); return 77; }
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);

        static u8 block[BLOCK_BYTES];
        for (u32 i = 0; i < sizeof block; i++) block[i] = (u8)(i * 13u + 5u);

        /* Nothing staged yet: nothing to keep. */
        assert(!eng_reuse_vs_constants(NULL));

        eng_bind_vs_constants(NULL, block, sizeof block);
        const u32 off = s_eng_pending.vs_cb_off;
        const u32 used = s_eng_stage_used;
        assert(s_eng_pending.vs_cb_bytes == sizeof block);
        assert(!memcmp(s_eng_stage + off, block, sizeof block));

        /* Kept: the same offset, and not a byte more staged. */
        s_eng_pending.vs_cb_off = 0xDEAD; s_eng_pending.vs_cb_bytes = 0;
        assert(eng_reuse_vs_constants(NULL));
        assert(s_eng_pending.vs_cb_off == off && s_eng_pending.vs_cb_bytes == sizeof block);
        assert(s_eng_stage_used == used);

        /* Other staging in between does not disturb it. */
        u8 ps[64] = {1, 2, 3};
        eng_bind_ps_constants(NULL, ps, sizeof ps);
        assert(eng_reuse_vs_constants(NULL) && s_eng_pending.vs_cb_off == off);
        assert(!memcmp(s_eng_stage + off, block, sizeof block));

        /* Growing the arena moves the bytes to a bigger buffer at the same offsets. */
        u32 grown_off = 0;
        assert(eng_stage_reserve(s_eng_stage_cap + 1u, &grown_off));
        assert(eng_reuse_vs_constants(NULL) && s_eng_pending.vs_cb_off == off);
        assert(!memcmp(s_eng_stage + off, block, sizeof block));

        /* A commit hands the arena over: the copy is no longer here to point at. */
        eng_encode_and_commit(nil);
        assert(!s_eng_stage_buf && !s_eng_stage_used);
        assert(!eng_reuse_vs_constants(NULL));

        /* The next block is staged into the new arena and kept from there. */
        block[0] ^= 0xFF;
        eng_bind_vs_constants(NULL, block, sizeof block);
        assert(s_eng_pending.vs_cb_bytes == sizeof block);
        assert(!memcmp(s_eng_stage + s_eng_pending.vs_cb_off, block, sizeof block));
        assert(eng_reuse_vs_constants(NULL));

        /* A block that was not staged (zero bytes) leaves nothing to keep. */
        eng_bind_vs_constants(NULL, block, 0);
        assert(!eng_reuse_vs_constants(NULL));

        /* Shutdown forgets it too. */
        eng_bind_vs_constants(NULL, block, sizeof block);
        assert(eng_reuse_vs_constants(NULL));
        eng_shutdown(NULL);
        assert(!eng_reuse_vs_constants(NULL));

        puts("The Metal staging arena keeps the vertex constant block only while its arena is recorded into");
    }
}
