#include "rsx_present_policy.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char* name)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

/* The Metal display decision (#94) as the backend drives it: `shown` stands in for s_eng_shown (0 = nothing). */
static rsx_display_state st;
static int shown;
static const rsx_display_view view60 = { 1280, 720, 2000, 0x3F800000u };

/* One present of `surface` with `open` records still recorded; returns 1 when it would acquire a drawable. A
 * drawable that is not `obtained` models a nil nextDrawable. */
static int present(int surface, uint32_t open, const rsx_display_view* v, int obtained)
{
    if (rsx_display_can_skip(&st, surface == shown, open, v)) return 0;
    if (!obtained) { rsx_display_note_missed(&st); return 1; }
    rsx_display_note_encoded(&st, open);
    shown = surface;
    rsx_display_note_shown(&st, v);
    return 1;
}

static void display_policy(void)
{
    rsx_display_reset(&st);
    shown = 0;
    check(present(1, 0, &view60, 1), "the first present shows even with no records: nothing is on screen");
    check(!present(1, 0, &view60, 1), "same surface, no records, same layer: skipped");
    check(!present(1, 0, &view60, 1), "and again: a skip leaves the state as it was");
    check(present(1, 3, &view60, 1), "new records on the same surface present");
    check(!present(1, 0, &view60, 1), "the records are shown: skip resumes");
    check(present(2, 0, &view60, 1), "a different surface presents");
    check(!present(2, 0, &view60, 1), "and is then skipped");

    /* A flush commit encodes records with no present: the count survives it and the next present shows them. */
    rsx_display_note_encoded(&st, 5);
    check(rsx_display_pending(&st, 0) == 5, "a flush keeps its records in the pending count");
    check(present(2, 0, &view60, 1), "records encoded by a flush make the next present show");
    check(rsx_display_pending(&st, 0) == 0, "showing clears the count");

    /* The layer or the blit changed. */
    rsx_display_view v = view60;
    v.width = 1920; v.height = 1080;
    check(present(2, 0, &v, 1), "a layer size change presents");
    check(!present(2, 0, &v, 1), "the new size is then the baseline");
    v.scale_milli = 1000;
    check(present(2, 0, &v, 1), "a content scale change presents");
    v.gamma_bits = 0x3F000000u;
    check(present(2, 0, &v, 1), "a gamma change presents: the same surface blits to other pixels");
    check(!present(2, 0, &v, 1), "the new gamma is then the baseline");

    /* A dropped image: what is on screen is not the last picture, so the same surface shows again. */
    check(present(2, 4, &v, 0), "a nil drawable with new records is an attempt, not a skip");
    check(rsx_display_pending(&st, 0) == 0, "the dropped present encoded nothing, so the open records stay with the caller");
    check(present(2, 4, &v, 1), "the retry presents the same records");
    check(!present(2, 0, &v, 1), "and the shown picture is skipped again");
    rsx_display_note_missed(&st);
    check(present(2, 0, &v, 1), "a present that missed the screen presents again even with no records");
    check(!present(2, 0, &v, 1), "once it lands the skip resumes");

    /* A blit that bailed out after its records were encoded keeps them pending and forces a re-present. */
    rsx_display_note_encoded(&st, 2);
    rsx_display_note_missed(&st);
    check(rsx_display_pending(&st, 0) == 2, "a failed blit leaves the encoded records pending");
    check(present(2, 0, &v, 1), "and the next present shows them");

    rsx_display_reset(&st);
    check(present(2, 0, &v, 1), "a reset forgets what was shown");

    /* The pending count saturates instead of wrapping to 0, which would read as "nothing new". */
    rsx_display_note_encoded(&st, UINT32_MAX);
    rsx_display_note_encoded(&st, 7);
    check(rsx_display_pending(&st, 7) == UINT32_MAX, "the pending count saturates");
    check(present(2, 0, &v, 1), "a saturated count still presents");
}

int main(void)
{
    check(!rsx_present_flip_already_rendered(0, 0, 0),
          "boot fallback remains active before a clear present");
    check(rsx_present_flip_already_rendered(1, 7, 7),
          "the flip rendered by a clear boundary is not rendered twice");
    check(!rsx_present_flip_already_rendered(1, 7, 8),
          "a later guest flip resumes normal presentation");
    check(rsx_present_flip_already_rendered(1, 0, 0),
          "a pre-flip clear present suppresses the duplicate boot present");
    display_policy();
    return failures ? 1 : 0;
}
