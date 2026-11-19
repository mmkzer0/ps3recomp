/*
 * ps3recomp - one PRESENT span per engine present (toolkit #144)
 *
 * The ftrace PRESENT span (kind 10) used to be emitted by the Metal backend's host entry point only.
 * A title whose FIFO carries the flip head words was then left with no PRESENT span at all once #144
 * skipped the duplicate host call: the traces still counted the presents through ENG_PRESENT (kind 37),
 * but the report's timed "present" row saw zero. The span now times every present at the engine's one
 * present path (eng_present): the flip head, the host's pacing call and an explicit buffer present.
 * A skipped duplicate never reaches it. No GPU, no host API: the recording stub backend counts the
 * presents that reach it, and the test reads back the trace the recorder wrote.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ps3emu/ps3_ftrace.h"
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

/* The spans of one kind in the trace file ps3_ft_dump() wrote: "# ...", "T ..." and "J ..." are not spans. */
static unsigned count_spans(const char* path, unsigned kind)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    unsigned n = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned k;
        if (sscanf(line, "%u", &k) == 1 && k == kind) n++;
    }
    fclose(f);
    return n;
}

int main(void)
{
    char path[160], env[192];
    snprintf(path, sizeof path, "/tmp/ps3recomp_present_span_%ld.txt", (long)getpid());
    snprintf(env, sizeof env, "0:3600:%s", path);
    if (setenv("PS3_FTRACE", env, 1) != 0 || !ps3_ft_init()) {
        printf("[FAIL] cannot start the trace recorder at %s\n", path);
        return 1;
    }
    printf("-- a trace file at %s\n", path);

    stub_guest_init();
    unsigned presented = 0;

    /* The issue's sequence: a flip driven through both paths -- the FIFO's head word and the host's
     * frame-clock present -- is one present and must leave one span. */
    printf("-- a flip through the FIFO head word and the host's frame clock\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    stub_flip(2);
    CHECK(stub.n_present == 1, "the flip head presents (%d)", stub.n_present);
    const int rep0 = rsx_draw_engine_present();
    CHECK(stub.n_present == 1 && rep0 == 0,
          "the host's duplicate is skipped (%d presents, returned %d)", stub.n_present, rep0);
    stub_flip(3);
    CHECK(stub.n_present == 2, "the next flip's head presents (%d)", stub.n_present);
    const int rep1 = rsx_draw_engine_present();
    CHECK(stub.n_present == 2 && rep1 == 0,
          "its host duplicate is skipped (%d presents, returned %d)", stub.n_present, rep1);
    presented += (unsigned)stub.n_present;
    stub_engine_down();

    /* A title that never flips through a head word: every host call is a present, so every call is a span. */
    printf("-- a host that never flips through a head word\n");
    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);
    CHECK(rsx_draw_engine_present() == 1, "a host call presents");
    CHECK(rsx_draw_engine_present() == 1, "and so does the next");
    CHECK(stub.n_present == 2, "two host calls are two presents (%d)", stub.n_present);
    presented += (unsigned)stub.n_present;
    stub_engine_down();
    free(stub_guest);

    ps3_ft_dump();
    const unsigned present_spans = count_spans(path, PS3_FT_PRESENT);
    const unsigned eng_spans = count_spans(path, PS3_FT_ENG_PRESENT);
    unlink(path);

    CHECK(present_spans == presented,
          "one PRESENT span (kind %d) per present: %u presents, %u spans",
          PS3_FT_PRESENT, presented, present_spans);
    CHECK(eng_spans == presented,
          "and one ENG_PRESENT instant (kind %d) per present: %u presents, %u spans",
          PS3_FT_ENG_PRESENT, presented, eng_spans);

    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
