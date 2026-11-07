/*
 * ps3recomp - frame dump selection and naming (rsx_frame_select.h)
 *
 * The contract a diff of presented frames relies on: the present numbers the user names are exactly the
 * presents written, under a name that carries the present's identity, and a value that cannot be honoured
 * is refused instead of shortened (a frame missing from a replay's worth of dumps says nothing about why).
 */
#include "rsx_frame_select.h"

#include <stdio.h>
#include <string.h>

#define NO_FLIP 0xFFFFFFFFu

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

static rsx_frame_select_status parse(rsx_frame_select* s, const char* range, const char* list, unsigned* count)
{
    return rsx_frame_select_parse(s, range, list, count);
}

/* The presents in [0, limit) the selection names, as "a,b,c". */
static void named(const rsx_frame_select* s, unsigned limit, char* out, size_t n)
{
    size_t at = 0;
    out[0] = 0;
    for (unsigned q = 0; q < limit; q++)
        if (rsx_frame_select_wanted(s, q))
            at += (size_t)snprintf(out + at, n - at, "%s%u", at ? "," : "", q);
}

static void test_list(void)
{
    printf("-- LIST\n");
    rsx_frame_select s;
    char got[256];
    CHECK(parse(&s, NULL, "120,240,300", NULL) == RSX_FRAME_SELECT_OK, "a list parses");
    named(&s, 400, got, sizeof got);
    CHECK(!strcmp(got, "120,240,300"), "exactly the listed presents are wanted (%s)", got);

    CHECK(parse(&s, NULL, " 7 , 0x10,0X1f ,8", NULL) == RSX_FRAME_SELECT_OK, "blanks and hex are accepted");
    named(&s, 64, got, sizeof got);
    CHECK(!strcmp(got, "7,8,16,31"), "decimal and 0x entries (%s)", got);

    CHECK(parse(&s, NULL, "0120", NULL) == RSX_FRAME_SELECT_OK && rsx_frame_select_wanted(&s, 120) &&
          !rsx_frame_select_wanted(&s, 80), "a leading zero is decimal, not octal");

    CHECK(parse(&s, NULL, "4294967295", NULL) == RSX_FRAME_SELECT_OK && rsx_frame_select_wanted(&s, 0xFFFFFFFFu),
          "the largest present number is accepted");
    CHECK(parse(&s, NULL, "4294967296", NULL) == RSX_FRAME_SELECT_BAD_LIST, "one more does not wrap, it is refused");
    CHECK(parse(&s, NULL, "0x100000000", NULL) == RSX_FRAME_SELECT_BAD_LIST, "nor does a wide hex number");
    CHECK(parse(&s, NULL, "99999999999999999999999", NULL) == RSX_FRAME_SELECT_BAD_LIST, "nor a number past 64 bits");

    static const char* const bad[] = { "120,,240", "120,", ",120", "abc", "12x", "-5", "+5", "1 2", "0x", "0xg", "1;2", " " };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const rsx_frame_select_status st = parse(&s, NULL, bad[i], NULL);
        CHECK(st == RSX_FRAME_SELECT_BAD_LIST, "\"%s\" is refused (%d)", bad[i], (int)st);
        CHECK(!s.have_list && !s.have_range && !s.n_list, "a refused list keeps nothing (\"%s\")", bad[i]);
    }
}

static void test_list_length(void)
{
    printf("-- LIST length\n");
    rsx_frame_select s;
    char text[512];
    unsigned count = 0;
    size_t at = 0;
    for (unsigned i = 0; i < RSX_FRAME_SELECT_LIST_MAX; i++)
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%u", i ? "," : "", 1000 + i);
    CHECK(parse(&s, NULL, text, &count) == RSX_FRAME_SELECT_OK && s.n_list == RSX_FRAME_SELECT_LIST_MAX,
          "32 entries are kept");
    CHECK(rsx_frame_select_wanted(&s, 1031), "the 32nd is wanted");

    snprintf(text + at, sizeof text - at, ",%u", 1000u + RSX_FRAME_SELECT_LIST_MAX);
    count = 0;
    CHECK(parse(&s, NULL, text, &count) == RSX_FRAME_SELECT_LIST_TOO_LONG, "33 entries are refused, not cut to 32");
    CHECK(count == RSX_FRAME_SELECT_LIST_MAX + 1, "the refusal counts them (%u)", count);
    CHECK(!s.have_list && !rsx_frame_select_wanted(&s, 1000), "a refused list wants nothing");
}

static void test_range(void)
{
    printf("-- RANGE\n");
    rsx_frame_select s;
    CHECK(parse(&s, "120-180", NULL, NULL) == RSX_FRAME_SELECT_OK, "a range parses");
    CHECK(!rsx_frame_select_wanted(&s, 119) && rsx_frame_select_wanted(&s, 120) && rsx_frame_select_wanted(&s, 150) &&
          rsx_frame_select_wanted(&s, 180) && !rsx_frame_select_wanted(&s, 181),
          "both ends are inclusive and nothing outside is wanted");
    CHECK(parse(&s, "5:5", NULL, NULL) == RSX_FRAME_SELECT_OK && rsx_frame_select_wanted(&s, 5) &&
          !rsx_frame_select_wanted(&s, 4) && !rsx_frame_select_wanted(&s, 6), "first == last is one present, ':' works");
    CHECK(parse(&s, " 0x10 - 0x12 ", NULL, NULL) == RSX_FRAME_SELECT_OK && rsx_frame_select_wanted(&s, 17),
          "blanks and hex around the dash");
    CHECK(parse(&s, "0-4294967295", NULL, NULL) == RSX_FRAME_SELECT_OK && rsx_frame_select_wanted(&s, 0xFFFFFFFFu),
          "the whole range");

    static const char* const bad[] = { "180-120", "120", "120-", "-180", "a-b", "1-2-3", "1-2x", "1--2", "0-4294967296" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const rsx_frame_select_status st = parse(&s, bad[i], NULL, NULL);
        CHECK(st == RSX_FRAME_SELECT_BAD_RANGE, "\"%s\" is refused (%d)", bad[i], (int)st);
        CHECK(!s.have_range && !rsx_frame_select_wanted(&s, 150), "a refused range keeps nothing (\"%s\")", bad[i]);
    }
}

static void test_combination(void)
{
    printf("-- both, neither\n");
    rsx_frame_select s;
    CHECK(parse(&s, "1-100", "7,9", NULL) == RSX_FRAME_SELECT_OK, "both set");
    CHECK(rsx_frame_select_wanted(&s, 7) && rsx_frame_select_wanted(&s, 9) && !rsx_frame_select_wanted(&s, 8) &&
          !rsx_frame_select_wanted(&s, 50), "the list wins over the range");

    CHECK(parse(&s, "bad", "7", NULL) == RSX_FRAME_SELECT_BAD_RANGE, "a bad range is refused even beside a good list");
    CHECK(parse(&s, "1-3", "bad", NULL) == RSX_FRAME_SELECT_BAD_LIST, "a bad list is refused even beside a good range");
    CHECK(!s.have_range && !s.have_list, "and the good half is not kept");

    CHECK(parse(&s, NULL, NULL, NULL) == RSX_FRAME_SELECT_OK && !s.have_list && !s.have_range,
          "neither set: nothing selected, the caller's default decides");
    CHECK(parse(&s, "", "", NULL) == RSX_FRAME_SELECT_OK && !s.have_list && !s.have_range, "empty strings are unset");
    CHECK(!rsx_frame_select_wanted(&s, 0) && !rsx_frame_select_wanted(&s, 120), "an empty selection wants no present");
}

static void test_names(void)
{
    printf("-- file names\n");
    char out[128];
    size_t need = rsx_frame_dump_name(out, sizeof out, "/tmp/run/f", 42, 41, NO_FLIP);
    CHECK(!strcmp(out, "/tmp/run/f_s00042_f00041.ppm") && need == strlen(out), "present and flip are zero-padded (%s)", out);

    rsx_frame_dump_name(out, sizeof out, "/tmp/run/f", 7, NO_FLIP, NO_FLIP);
    CHECK(!strcmp(out, "/tmp/run/f_s00007_fnone.ppm"), "a present before the first flip says fnone (%s)", out);

    rsx_frame_dump_name(out, sizeof out, "/tmp/run/f.ppm", 120, 0, NO_FLIP);
    CHECK(!strcmp(out, "/tmp/run/f_s00120_f00000.ppm"), "a trailing .ppm on the base is replaced (%s)", out);

    rsx_frame_dump_name(out, sizeof out, "/tmp/a.ppm.d/f", 1, 1, NO_FLIP);
    CHECK(!strcmp(out, "/tmp/a.ppm.d/f_s00001_f00001.ppm"), "a .ppm inside the base stays (%s)", out);

    rsx_frame_dump_name(out, sizeof out, ".ppm", 1, 1, NO_FLIP);
    CHECK(!strcmp(out, ".ppm_s00001_f00001.ppm"), "a base that is only the suffix is kept (%s)", out);

    rsx_frame_dump_name(out, sizeof out, "x", 123456, 7654321, NO_FLIP);
    CHECK(!strcmp(out, "x_s123456_f7654321.ppm"), "numbers wider than the padding are not cut (%s)", out);

    char a[64], b[64];
    rsx_frame_dump_name(a, sizeof a, "x", 5, 3, NO_FLIP);
    rsx_frame_dump_name(b, sizeof b, "x", 6, 3, NO_FLIP);
    CHECK(strcmp(a, b) != 0, "two presents that show the same flip get different files");

    char tiny[8];
    need = rsx_frame_dump_name(tiny, sizeof tiny, "/tmp/run/f", 42, 41, NO_FLIP);
    CHECK(need >= sizeof tiny && strlen(tiny) < sizeof tiny, "a name that does not fit reports its full length (%zu)", need);
}

int main(void)
{
    test_list();
    test_list_length();
    test_range();
    test_combination();
    test_names();
    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
