/* The draw engine's side of an RSX capture (rsx_capture.h, ps3diff.rsx/1).
 *
 * The engine runs against the recording stub backend and a synthetic guest memory, driven by the
 * capture's own sink the way the FIFO walker drives it: one step per method packet, the engine's
 * methods inside it. Leaves the capture in the directory argv[1] for tools/test_rsx_capture.py:
 *
 *   frame 0  one triangle drawn from a vertex buffer, then queue + head flip;
 *   frame 1  the SAME buffer rewritten by the "guest" between the two draws: the capture must hold
 *            each version in the draw that consumed it, not the buffer as it ends up;
 *   frame 2  a draw whose vertex span is not guest memory: dropped, counted under fetch-failed,
 *            and the frame is not valid.
 *
 * argv[2] names a scenario that must end in a stop (the process exits 70):
 *   bad-format   the presented surface is in a format the readback contract does not define
 */
#include "rsx_draw_engine_test_stub.h"
#include "rsx_capture.h"
#include "ps3emu/config.h"
#include "rsx_test_programs.h"

#define FP_OFFSET 0x00030000u

#include <stdio.h>
#include <stdlib.h>

static const rsx_capture_sink* g_sink;
void cellGcm_set_capture_sink(const rsx_capture_sink* sink) { g_sink = sink; }

const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    return &cfg;
}

static int commit_ok(void* user) { (void)user; return 1; }

static u32 io2ea(u32 io) { return io < 0x400000u ? 0x100000u + io : 0u; }

static u32 g_get = 0x2000;

/* One method packet: header word, then `n` data words; the engine sees each as the walker would
 * deliver it. incr = 1 for the type-0 (incrementing) form. */
static void packet(u32 subch, u32 method, const u32* data, u32 n)
{
    u32 words[16];
    words[0] = (n << 18) | (subch << 13) | method;
    for (u32 i = 0; i < n; i++) words[1 + i] = data[i];
    rsx_cap_step st = { RSX_CAP_STEP_METHOD, g_get, g_get + 4 + 4 * n, 0, subch, 1, n, 0, words, 1 + n, io2ea };
    g_sink->step_begin(&st);
    for (u32 i = 0; i < n; i++)
        rsx_draw_engine_method((subch << 13) | (method + 4 * i), data[i]);
    g_sink->step_end();
    g_get += 4 + 4 * n;
}

static void cmd(u32 method, u32 arg)
{
    /* Driver methods (0xE9xx) carry their subchannel in the address. */
    if (method >= 0xE000u) packet(7, method & 0x1FFCu, &arg, 1);
    else packet(0, method, &arg, 1);
}

static void draw(void)
{
    cmd(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    cmd(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    cmd(M_BEGIN_END, 0u);
}

static void flip(u32 buffer)
{
    cmd(M_GCM_QUEUE_FLIP, buffer & 7u);
    cmd(M_GCM_FLIP_HEAD, 0x8000010Fu);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: %s <capture dir> [scenario]\n", argv[0]); return 2; }
    const char* scenario = argc > 2 ? argv[2] : "";

    stub_guest_init();
    memset(&stub, 0, sizeof stub);
    stub.readback_byte = 0x40;
    rsx_draw_backend be = stub_backend;
    be.last_commit_ok = commit_ok;
    rsx_draw_engine_set_backend(&be);
    if (rsx_draw_engine_init(64, 64) != 0) { puts("[FAIL] engine init"); return 1; }

    rsx_capture_options opt = { argv[1], 0 };
    rsx_capture_environment env = { "synthetic", "n/a", "n/a", "Debug", "n/a", "n/a", "synthetic", "n/a" };
    (void)rsx_capture_install(&env, &opt);
    g_sink->gcm_init(0x80000, 0x400000, 0x100000);
    g_sink->display_buffer(1, 0x40000, 256, 64, 64);

    cmd(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    cmd(M_SURFACE_CLIP_H, 64u << 16);
    cmd(M_SURFACE_CLIP_V, 64u << 16);
    cmd(M_COLOR_A_OFFSET, 0x40000u);
    cmd(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    cmd(M_VTXBUF_OFFSET, 0x80000000u | STUB_VTX_OFFSET);

    /* The guest's own programs, so a draw is not a fixed-function one (a frame that ran the
     * built-in pipeline is not a baseline). */
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    cmd(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); cmd(0xB80 + i * 4, w); }
    cmd(0x1EA0, 0);
    rsx_test_fp_mov_r0_col0(vm_base + FP_OFFSET, RSX_TEST_FP_SWZ_IDENT);
    cmd(0x8E4, FP_OFFSET | 2u);

    if (!strcmp(scenario, "bad-format"))
        cmd(M_SURFACE_FORMAT, 0x0Du);                      /* R32F: no readback format is defined for it */

    draw();
    flip(1);

    /* The guest rewrites the vertex buffer between two consumes. */
    stub_guest[STUB_VTX_OFFSET + 3] = 0x01;
    draw();
    flip(1);

    cmd(M_VTXBUF_OFFSET, 0x80000000u | 0x00FFFFF8u);       /* a span past the end of guest memory */
    draw();
    flip(1);

    rsx_capture_finish();
    rsx_draw_engine_stats st;
    rsx_draw_engine_draw_stats(&st);
    const int ok = st.submitted == 3 && st.executed == 2 && st.by_reason[RSX_DROP_FETCH_FAILED] == 1;
    printf("[%s] 3 draws submitted, 2 executed, 1 dropped as fetch-failed (%llu/%llu/%llu)\n", ok ? "PASS" : "FAIL",
           st.submitted, st.executed, st.dropped);
    rsx_draw_engine_shutdown();
    return ok ? 0 : 1;
}
