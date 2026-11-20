/* Synthetic draw-engine test of the vertex-constant block a backend is left
 * holding when each draw runs. The engine copies the transform constants only
 * when one was written and asks a backend that can to keep the previous
 * block for an unchanged draw; whichever way a draw got its block, the bytes
 * the GPU reads must be the register file's constants and the viewport
 * epilogue, exactly. The recording backend models what a staging arena
 * does: it holds the last block it was given, and can lose it. No title
 * data.
 *
 * Build like test_rsx_draw_engine.c, substituting this file and adding
 * -I libs/video/tests. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main

#define VS_BLOCK_BYTES ((RSX_DSP_NUM_CONSTANTS + 2u) * 16u)
#define M_VP_TRANSLATE 0x0A20u
#define M_VP_SCALE     0x0A30u

static u8 g_held[VS_BLOCK_BYTES];
static int g_held_valid, g_allow_reuse = 1;
static int g_n_bind, g_n_reuse;

static void rec_bind_vs(void* u, const void* d, u32 n)
{
    (void)u;
    if (n != VS_BLOCK_BYTES) { printf("[FAIL] block size %u\n", n); g_failures++; return; }
    memcpy(g_held, d, n);
    g_held_valid = 1;
    g_n_bind++;
}

static int rec_reuse_vs(void* u)
{
    (void)u;
    g_n_reuse++;
    return g_held_valid && g_allow_reuse;
}

/* The register file as the test alone tracks it. */
static u32 g_model[RSX_DSP_NUM_CONSTANTS][4];

static void upload(u32 first_slot, const u32* words, u32 n)
{
    m(M_VP_UPLOAD_CONST_ID, first_slot);
    for (u32 w = 0; w < n; w++) {
        m(M_VP_UPLOAD_CONST + w * 4u, words[w]);
        const u32 slot = first_slot + (w >> 2);
        if (slot < RSX_DSP_NUM_CONSTANTS) g_model[slot][w & 3] = words[w];
    }
}

static int held_constants_are_model(void)
{
    return g_held_valid && !memcmp(g_held, g_model, sizeof g_model);
}

static const u8* held_xf(void) { return g_held + RSX_DSP_NUM_CONSTANTS * 16u; }

static rsx_draw_backend make_backend(void)
{
    rsx_draw_backend be = g_stub_backend;
    be.bind_vs_constants = rec_bind_vs;
    be.reuse_vs_constants = rec_reuse_vs;
    return be;
}

static u32 lcg_state = 12345u;
static u32 lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state >> 8; }

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    rsx_draw_backend be = make_backend();
    engine_up_with(&be);

    draw_triangle();
    CHECK(g_n_bind == 1 && held_constants_are_model(), "the first draw is bound with the register file's constants");

    draw_triangle();
    CHECK(g_n_bind == 1 && g_n_reuse == 1 && held_constants_are_model(),
          "an unchanged draw keeps the block instead of binding it again");

    const u32 v[4] = { 0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u };
    upload(5, v, 4);
    draw_triangle();
    CHECK(g_n_bind == 2 && held_constants_are_model(), "a constant written between draws is in the next draw's block");

    /* A write the register file refuses changes nothing the block holds. */
    upload(600, v, 4);
    draw_triangle();
    CHECK(held_constants_are_model(), "a write past the last slot leaves the block as it was");

    /* The backend lost its copy (an arena recycled): the engine binds again. */
    g_allow_reuse = 0;
    const int binds = g_n_bind;
    draw_triangle();
    CHECK(g_n_bind == binds + 1 && held_constants_are_model(), "a backend that cannot keep the block is given it again");
    g_allow_reuse = 1;
    draw_triangle();
    CHECK(g_n_bind == binds + 1 && held_constants_are_model(), "and keeps the new copy");

    /* A burst across several vectors. */
    u32 burst[32];
    for (u32 i = 0; i < 32; i++) burst[i] = 0x3F000000u + i * 0x100u;
    upload(100, burst, 32);
    draw_triangle();
    CHECK(held_constants_are_model(), "a 32-word upload reaches the block whole");

    /* The viewport epilogue is part of the block: a moved viewport rebinds it. */
    u8 before[32];
    memcpy(before, held_xf(), sizeof before);
    const int binds_vp = g_n_bind;
    mf(M_VP_TRANSLATE, 128.0f); mf(M_VP_TRANSLATE + 4, 128.0f); mf(M_VP_TRANSLATE + 8, 0.5f);
    mf(M_VP_SCALE, 128.0f);     mf(M_VP_SCALE + 4, -128.0f);    mf(M_VP_SCALE + 8, 0.5f);
    draw_triangle();
    CHECK(g_n_bind == binds_vp + 1 && memcmp(held_xf(), before, sizeof before) != 0 && held_constants_are_model(),
          "a changed viewport changes the block the draw reads");
    draw_triangle();
    CHECK(g_n_bind == binds_vp + 1 && held_constants_are_model(), "and the same viewport does not rebind");

    /* Writes and draws in an order the test does not choose. */
    int all_equal = 1;
    for (u32 i = 0; i < 400; i++) {
        const u32 r = lcg();
        if (r & 1u) {
            u32 w[8];
            const u32 n = 1u + (lcg() % 8u);
            for (u32 k = 0; k < n; k++) w[k] = lcg() * 2654435761u;
            upload(lcg() % (RSX_DSP_NUM_CONSTANTS + 8u), w, n);
        }
        if (r & 2u) g_allow_reuse = (int)(lcg() & 1u);
        if (r & 4u) {
            draw_triangle();
            if (!held_constants_are_model()) all_equal = 0;
        }
    }
    g_allow_reuse = 1;
    CHECK(all_equal, "400 interleaved writes, draws and lost copies: every draw read the register file's constants");

    /* A backend set mid-run is bound afresh, whatever the old one held. */
    const int binds_swap = g_n_bind;
    rsx_draw_engine_set_backend(&be);
    draw_triangle();
    CHECK(g_n_bind == binds_swap + 1 && held_constants_are_model(), "a backend set mid-run is bound afresh");

    /* An engine initialised again without a shutdown restarts the dispatcher's
     * epoch: the same number of writes of other values must not pass for the
     * constants already mirrored. */
    memset(g_model, 0, sizeof g_model);
    engine_up_with(&be);
    upload(0, v, 4);
    draw_triangle();
    const u32 w2[4] = { 0x41000000u, 0x41100000u, 0x41200000u, 0x41300000u };
    engine_up_with(&be);
    memset(g_model, 0, sizeof g_model);
    upload(0, w2, 4);
    draw_triangle();
    CHECK(held_constants_are_model(), "a second init's writes are not taken for the first one's");

    /* A restarted engine has a fresh register file: nothing of the last run's
     * constants may survive in the block, even in a backend that kept them. */
    engine_down();
    memset(g_model, 0, sizeof g_model);
    engine_up_with(&be);
    const int binds_up = g_n_bind;
    draw_triangle();
    CHECK(g_n_bind == binds_up + 1 && held_constants_are_model(),
          "after a restart the first draw is bound with the new register file's constants");

    engine_down();
    free(vm_base);
    printf("vs constants failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
