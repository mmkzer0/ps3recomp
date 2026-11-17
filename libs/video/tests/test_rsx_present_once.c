/*
 * ps3recomp - one present per guest flip (toolkit #144)
 *
 * A guest flip used to reach eng_present twice: the FIFO's flip head word presented through the
 * engine's own sink, and the host's frame clock called rsx_draw_engine_present() for the same frame,
 * repeating the encode, blit and commit (and possibly a drawable acquisition) every guest frame.
 * The engine now skips the host present when the frame it would show -- the same flip id and the same
 * buffer -- was already presented and nothing requires a re-present. This fixture pins the count and
 * every condition that must still present. No GPU, no host API: the recording stub backend counts the
 * presents that reach it (eng_present's g.be->present).
 */
#include <stdio.h>
#include <stdlib.h>

#include "rsx_draw_engine_test_stub.h"

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

/* The issue's count: one guest flip driven through both paths -- the FIFO's head word and the host's
 * frame-clock present -- is one present. */
static void test_one_present_per_flip(void)
{
    printf("-- a flip through the FIFO head word and the host's frame clock\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);   /* a rendered surface to present */

    stub_flip(2);                              /* the head word presents flip 0 */
    CHECK(stub.n_present == 1, "the flip head is one present (%d)", stub.n_present);
    const int rep0 = rsx_draw_engine_present();
    CHECK(stub.n_present == 1 && rep0 == 0,
          "the host's frame-clock present of the same flip is skipped and reports it: "
          "one present per flip (%d presents, returned %d)", stub.n_present, rep0);

    stub_flip(3);                              /* flip 1: the next frame */
    CHECK(stub.n_present == 2, "the next flip's head is the second present (%d)", stub.n_present);
    const int rep1 = rsx_draw_engine_present();
    CHECK(stub.n_present == 2 && rep1 == 0, "its host present is skipped and reports it too "
          "(%d presents, returned %d)", stub.n_present, rep1);

    rsx_frame_identity fid;
    CHECK(rsx_draw_engine_frame_identity(&fid) == 1 && fid.flip_id == 1 && fid.display_buffer == 3,
          "the identity stays the flip's (flip %u buffer %u)", fid.flip_id, fid.display_buffer);
    stub_engine_down();
}

/* A title that never flips through a head word (the harness, the boot smoke title, the SetFlip HLE and
 * the 0xFEAD marker): the host call is its only present and must present every time. */
static void test_host_only_presents_every_call(void)
{
    printf("-- a host that never flips through a head word\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    CHECK(rsx_draw_engine_present() == 1, "a host call presents and reports it");
    rsx_draw_engine_present();
    rsx_draw_engine_present();
    CHECK(stub.n_present == 3, "three host calls are three presents (%d)", stub.n_present);
    stub_engine_down();
}

/* A flip the engine never presents itself (the 0xFEAD marker, the SetFlip HLE) reaches it as a
 * rsx_draw_engine_note_flip: the host's present must show that flip even when it names the buffer the
 * last head already showed. The note cellGcmSys emits for a head word the engine did present (the
 * echo) changes nothing. */
static void test_note_flip(void)
{
    printf("-- a flip the engine did not present\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);

    stub_flip(1);                              /* the head presents flip 0, buffer 1 */
    rsx_draw_engine_note_flip(1);              /* cellGcmSys's echo of that same head word */
    const int echo = rsx_draw_engine_present();
    CHECK(stub.n_present == 1 && echo == 0, "the echo note does not defeat the skip "
          "(%d presents, returned %d)", stub.n_present, echo);

    rsx_draw_engine_note_flip(2);              /* a flip the engine never saw: its frame must be shown */
    const int noted = rsx_draw_engine_present();
    rsx_frame_identity fid;
    CHECK(stub.n_present == 2 && noted == 1 && rsx_draw_engine_frame_identity(&fid) == 1 &&
          fid.flip_id == 0 && fid.display_buffer == 2,
          "a noted flip presents on the host call and reports it (%d presents, returned %d, "
          "flip %u buffer %u)", stub.n_present, noted, fid.flip_id, fid.display_buffer);
    stub_engine_down();
}

/* A re-present requirement (the backends' resize / recreation / background-return / missed-present
 * signal, rsx_draw_engine_require_represent) defeats the skip. */
static void test_require_represent(void)
{
    printf("-- a re-present requirement\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    stub_flip(0);
    const int r0 = rsx_draw_engine_present();
    CHECK(stub.n_present == 1 && r0 == 0, "before the event the host present is skipped "
          "(%d presents, returned %d)", stub.n_present, r0);
    rsx_draw_engine_require_represent();
    const int r1 = rsx_draw_engine_present();
    CHECK(stub.n_present == 2 && r1 == 1, "after it the same frame presents again and reports it "
          "(%d presents, returned %d)", stub.n_present, r1);
    const int r2 = rsx_draw_engine_present();
    CHECK(stub.n_present == 2 && r2 == 0, "and the next call is skipped again "
          "(%d presents, returned %d)", stub.n_present, r2);
    stub_engine_down();
}

/* rsx_draw_engine_present_buffer is the same host-driven present: skipped for the frame the head
 * already showed, present every time for a host-only run. */
static void test_present_buffer(void)
{
    printf("-- the explicit-buffer host present\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    rsx_draw_engine_present_buffer(0);
    CHECK(stub.n_present == 1, "a host-only present_buffer presents (%d)", stub.n_present);
    stub_flip(2);
    CHECK(stub.n_present == 2, "the head is the second present (%d)", stub.n_present);
    rsx_draw_engine_present_buffer(2);
    CHECK(stub.n_present == 2, "an explicit present of the shown frame is skipped (%d)", stub.n_present);
    rsx_draw_engine_present_buffer(3);
    CHECK(stub.n_present == 3, "an explicit present of another buffer presents (%d)", stub.n_present);
    stub_engine_down();
}

/* A head word without bit 31 does not advance the flip id (cellGcmSys counts only bit-31 words), so the
 * host's present of that frame cannot be recognised as a duplicate and still presents: the skip never
 * drops a present it cannot prove is a duplicate. */
static void test_offset_form_head(void)
{
    printf("-- a head word without bit 31\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    sm(M_GCM_QUEUE_FLIP, 2);
    sm(M_GCM_FLIP_HEAD, 0x00040000u);          /* the offset form: resolves display buffer 0 */
    CHECK(stub.n_present == 1, "the word itself presents (%d)", stub.n_present);
    rsx_draw_engine_present();
    CHECK(stub.n_present == 2, "the host present cannot be proven a duplicate and runs (%d)",
          stub.n_present);
    stub_engine_down();
}

int main(void)
{
    stub_guest_init();
    test_one_present_per_flip();
    test_host_only_presents_every_call();
    test_note_flip();
    test_require_represent();
    test_present_buffer();
    test_offset_form_head();

    free(stub_guest);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
