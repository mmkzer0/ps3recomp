/* Synthetic draw-engine test of what the fragment texture state contributes to
 * the pipeline identity and the program text: cube, volume and unnormalised
 * units mixed, sampled and not. The pipeline lookup reads those per-unit
 * facts in one sweep; this pins that the shader each state gets, and which
 * states share a pipeline, did not move when the sweeps were merged. The
 * expected program hashes were taken from the engine before the merge. No
 * title data.
 *
 * Build like test_rsx_draw_engine.c, substituting this file and adding
 * -I libs/video/tests. Run with --print to list the hashes it measures. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"

static char g_ps[65536];
static u32 capture_pipeline(void* u, const char* vs, const char* ps, const rsx_be_render_state* rs,
                            const rsx_vertex_layout_plan* l, u32 stride, rsx_be_format f, u32 n)
{
    snprintf(g_ps, sizeof g_ps, "%s", ps);
    return stub_pipeline_create(u, vs, ps, rs, l, stride, f, n);
}

static u64 fnv(const char* s)
{
    u64 h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    return h;
}

/* Unit `u`: SET_TEXTURE_OFFSET/FORMAT/CONTROL0/REMAP/IMAGE_RECT and the
 * pitch/depth word at 0x1840 + 4u. */
static void unit(u32 u, u32 fmt, u32 dim, int cube, u32 w, u32 h, u32 depth, u32 pitch, int on)
{
    const u32 b = M_TEX_UNIT + u * 0x20u;
    m(b + 0x00, 0x60000u + u * 0x1000u);
    m(b + 0x04, 2u | (cube ? 4u : 0u) | (dim << 4) | (fmt << 8) | (1u << 16));
    m(b + 0x0C, on ? 0x80000000u : 0u);
    m(b + 0x10, 0xAAE4u);
    m(b + 0x18, (w << 16) | h);
    m(0x1840u + u * 4u, (depth << 20) | pitch);
}

#define FMT_ARGB      0x85u
#define FMT_ARGB_UN   (0x85u | 0x20u | 0x40u)   /* linear, texel-addressed */

static int g_print;
static void expect(const char* what, u64 want)
{
    const u64 got = fnv(g_ps);
    if (g_print) printf("%s 0x%016llxull\n", what, (unsigned long long)got);
    else CHECK(got == want, "%s: the program text is the one the pre-merge engine built", what);
}

/* The mixed state: a cube on 0, a volume on 1, a texel-addressed unit on 2 and an ordinary one on 3. */
static void mixed(void)
{
    unit(0, FMT_ARGB, 2, 1, 8, 8, 1, 0, 1);
    unit(1, FMT_ARGB, 3, 0, 8, 8, 4, 0, 1);
    unit(2, FMT_ARGB_UN, 2, 0, 8, 4, 1, 32, 1);
    unit(3, FMT_ARGB, 2, 0, 8, 8, 1, 0, 1);
}

int main(int argc, char** argv)
{
    g_print = argc > 1 && !strcmp(argv[1], "--print");
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    engine_up();
    rsx_draw_backend be = g_stub_backend;
    be.pipeline_create = capture_pipeline;
    be.texture_create_3d = stub_texture_create_3d;
    be.texture_upload_3d = stub_texture_upload_3d;
    rsx_draw_engine_set_backend(&be);

    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
    /* Four fetches, one from each of units 0..3. */
    for (u32 i = 0; i < 4; i++)
        rsx_test_fp_instr(vm_base + TEX_OFFSET + i * 16u,
            RSX_TEST_FP_OPC(RSX_TEST_FP_OP_TEX) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_TC0 |
                RSX_TEST_FP_TEXU(i) | (i == 3 ? RSX_TEST_FP_END : 0u),
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);

    mixed();
    draw_triangle();
    const u32 p_mixed = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 1 && p_mixed, "the mixed state builds one pipeline");
    expect("mixed cube/volume/unnormalised", 0x1cea8f6c3ccd56d3ull);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.bound_pipeline == p_mixed, "and the next draw reuses it");

    /* A cube on a unit the program does not sample is still part of the key. */
    unit(7, FMT_ARGB, 2, 1, 4, 4, 1, 0, 1);
    draw_triangle();
    const u32 p_cube7 = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 2 && p_cube7 != p_mixed, "a cube on an unsampled unit is another pipeline");
    expect("cube on unsampled unit", 0x867288c0453edc4eull);
    unit(7, FMT_ARGB, 2, 0, 4, 4, 1, 0, 0);

    /* A cube face is square: 8x4 is not one, and the declared cube goes. */
    unit(0, FMT_ARGB, 2, 1, 8, 4, 1, 0, 1);
    draw_triangle();
    const u32 p_flat = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 3 && p_flat != p_mixed && p_flat != p_cube7,
          "a non-square cube is not a cube");
    expect("non-square cube", 0x8814d38b9d4ed570ull);
    unit(0, FMT_ARGB, 2, 1, 8, 8, 1, 0, 1);

    /* The texel-addressed unit's size is baked into the program. */
    unit(2, FMT_ARGB_UN, 2, 0, 16, 4, 1, 64, 1);
    draw_triangle();
    const u32 p_un = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 4 && p_un != p_mixed, "a texel-addressed unit's size is identity");
    expect("texel-addressed 16x4", 0xc54b6198d62bbf3aull);

    /* ... but only for a unit the program samples. */
    unit(9, FMT_ARGB_UN, 2, 0, 32, 32, 1, 128, 1);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 4 && stub.bound_pipeline == p_un,
          "a texel-addressed unit the program does not sample is not");
    unit(9, FMT_ARGB_UN, 2, 0, 32, 32, 1, 128, 0);
    unit(2, FMT_ARGB_UN, 2, 0, 8, 4, 1, 32, 1);

    /* Switching the volume off drops its declaration. */
    unit(1, FMT_ARGB, 3, 0, 8, 8, 4, 0, 0);
    draw_triangle();
    const u32 p_novol = stub.bound_pipeline;
    CHECK(stub.n_pipeline_create == 5 && p_novol != p_mixed, "a disabled volume leaves the mixed pipeline");
    expect("volume disabled", 0xf484930be711c566ull);

    /* Back to the mixed state: its own pipeline again, nothing rebuilt. */
    unit(1, FMT_ARGB, 3, 0, 8, 8, 4, 0, 1);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 5 && stub.bound_pipeline == p_mixed,
          "restoring the mixed state finds the first pipeline again");

    /* A backend that cannot make a 3D texture sees the volume as a 2D one:
     * the same program as with the volume switched off. */
    be.texture_create_3d = NULL;
    be.texture_upload_3d = NULL;
    rsx_draw_engine_set_backend(&be);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 5 && stub.bound_pipeline == p_novol,
          "without 3D textures the volume is no volume");

    /* The render state's part of the identity moves with its registers and
     * comes back with them, however many draws sat in between. */
    m(M_COLOR_MASK, 0x01010100u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 6 && stub.bound_pipeline != p_novol, "a colour mask is another pipeline");
    draw_triangle();
    m(M_COLOR_MASK, 0x01010101u);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 6 && stub.bound_pipeline == p_novol,
          "and restoring it finds the earlier pipeline again");

    engine_down();
    free(vm_base);
    if (g_print) return 0;
    printf("texture facts failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
