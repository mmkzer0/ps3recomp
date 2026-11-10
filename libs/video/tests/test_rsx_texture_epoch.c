/*
 * ps3recomp - when the draw engine rechecks a cached texture against guest memory (#64)
 *
 * The engine cannot see a guest rewrite of a texture's bytes in place, so it rechecks each cached
 * texture against guest memory itself: at most once per texture epoch. The epoch advances at every
 * presented frame and at every NV4097_INVALIDATE_L2 (0x1FD8), which is the driver's "drop the
 * texture cache" (psl1ght cellGcmSetInvalidateTextureCache).
 *
 * This drives eng_texture_slot, the engine's cache lookup, directly (the engine source is included
 * so the static is reachable) against the recording stub backend. A recheck that finds new bytes
 * uploads a new host texture, so the handle it returns is the observable: no GPU, no host API.
 * The pixels the same contract produces on a GPU are test_metal_invalidate_l2.
 */
#include "rsx_draw_engine_test_stub.h"
#include "rsx_draw_engine.c"

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

#define TEX_SIDE   8u
#define TEX_A      0x20000u       /* main memory, clear of the vertex data at 0x10000 */
#define TEX_B      0x30000u
#define FMT_ARGB   0xA5u          /* A8R8G8B8, linear */
#define REMAP_ID   0xAAE4u

static void rewrite(u32 offset, u8 value)
{
    memset(stub_guest + offset, value, TEX_SIDE * TEX_SIDE * 4u);
}

static u32 sample(u32 offset)
{
    return eng_texture_slot(RSX_LOCATION_MAIN, offset, FMT_ARGB, TEX_SIDE, TEX_SIDE, 1,
                            TEX_SIDE * 4u, 0, REMAP_ID, 1);
}

int main(void)
{
    stub_guest_init();
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);        /* a current surface, so a flip presents */

    rewrite(TEX_A, 0x11);
    rewrite(TEX_B, 0x22);
    const u32 a1 = sample(TEX_A), b1 = sample(TEX_B);
    CHECK(a1 && b1 && a1 != b1, "two textures cached (%u, %u)", a1, b1);

    /* Without the method nothing changed from before: one recheck per frame, and the first sample
     * of the frame (above) was it. */
    rewrite(TEX_A, 0x33);
    rewrite(TEX_B, 0x44);
    CHECK(sample(TEX_A) == a1 && sample(TEX_B) == b1,
          "a rewrite after the first sample of a frame is not seen without INVALIDATE_L2");

    /* One word drops the whole cache: the next sample of EVERY cached texture rechecks. */
    sm(0x1FD8, 1u);
    const u32 a2 = sample(TEX_A), b2 = sample(TEX_B);
    CHECK(a2 && a2 != a1, "INVALIDATE_L2: the next sample of the first texture sees its new bytes (%u -> %u)", a1, a2);
    CHECK(b2 && b2 != b1, "...and of the second texture, one word dropping the whole cache (%u -> %u)", b1, b2);

    /* The recheck spends the epoch: a second sample neither rechecks nor re-uploads. */
    rewrite(TEX_A, 0x55);
    CHECK(sample(TEX_A) == a2, "after the recheck the epoch is spent: no per-draw rehash");

    /* Any argument invalidates, and an unchanged texture is rechecked, found equal, and kept. */
    sm(0x1FD8, 0u);
    const u32 a3 = sample(TEX_A), b3 = sample(TEX_B);
    CHECK(a3 != a2, "argument 0 invalidates too: the rewrite made after the last recheck is seen");
    CHECK(b3 == b2, "an unchanged texture keeps its host texture across an invalidate");

    /* The frame flip still advances the epoch, as before. */
    rewrite(TEX_B, 0x66);
    CHECK(sample(TEX_B) == b3, "a rewrite inside the frame is still not seen");
    stub_flip(0);
    CHECK(sample(TEX_B) != b3, "the flip advanced the epoch: the next frame sees the rewrite");

    /* An entry created inside one epoch is rechecked by the next invalidate. */
    sm(0x1FD8, 1u);
    rewrite(0x40000u, 0x77);
    const u32 c1 = sample(0x40000u);
    rewrite(0x40000u, 0x78);
    sm(0x1FD8, 1u);
    CHECK(c1 && sample(0x40000u) != c1, "an entry created in one epoch is rechecked in the next");

    stub_engine_down();
    free(stub_guest);
    printf("test_rsx_texture_epoch: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
