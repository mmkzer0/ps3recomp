/*
 * ps3recomp - the pass dump's present naming: M3 gaps 3a/3b
 *
 * Before H1a the presented surface's image was named by the pass counter,
 * which does not advance on a present: two presents between two draw passes
 * wrote the same presentNNN.ppm and the later image replaced the earlier one.
 * G0 measured 130 of 389 present images lost that way, and it also made the
 * RESC source's own presents unreadable.
 *
 * Each present now has its own file, named by present_seq and the flip head it
 * follows, and the pass images carry the frame's identity too. The test drives
 * three presents with no pass between two of them and asserts distinct files,
 * distinct index lines, and the identity in the names.
 */
#include "rsx_draw_engine_test_stub.h"

#include <dirent.h>
#include <unistd.h>

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

static int file_exists(const char* dir, const char* name)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static int count_prefix(const char* dir, const char* prefix)
{
    DIR* d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
    closedir(d);
    return n;
}

static char* slurp(const char* dir)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/passes.txt", dir);
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* text = (char*)calloc(1, (size_t)(size > 0 ? size : 0) + 1);
    if (text && size > 0 && fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

static void clear_dir(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

int main(void)
{
    char dir[] = "/tmp/rs3recomp-rsx-dump-XXXXXX";
    if (!mkdtemp(dir)) { printf("[FAIL] mkdtemp\n"); return 1; }
    stub_guest_init();
    setenv("RSX_ENGINE_PASS_DUMP", dir, 1);
    setenv("RSX_ENGINE_PASS_DUMP_MAX", "16", 1);
    unsetenv("RSX_ENGINE_PASS_DUMP_AFTER");
    /* Zero: arm on the first present. The stub's draw runs the built-in
     * program pair, so guest_draws stays 0 and the default minimum of 1 would
     * never arm. */
    setenv("RSX_ENGINE_PASS_DUMP_MINDRAWS", "0", 1);

    stub_engine_up(1);
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);       /* guest_draws = 1 */

    /* Present 0 (flip 0's head) arms the dump (the arming present writes no
     * image). A flip is one present, at its head word: the two host presents
     * that follow flip 1's head are duplicates of the shown flip and are
     * skipped (#144), so they write no file and take no ordinal. */
    stub_flip(1);                                  /* 0: flip 0's head */
    stub_flip(0);                                  /* 1: flip 1's head */
    rsx_draw_engine_present();                     /* skipped: flip 1 is already shown */
    rsx_draw_engine_present();                     /* skipped again */
    stub_begin_end(RSX_PRIMITIVE_TRIANGLES);       /* a pass after them */
    stub_flip(1);                                  /* 2: flip 2's head closes the pass */
    stub_engine_down();

    /* A present is named by the flip whose scanout it shows: the head that
     * retires a flip, one present per flip. Present 0 (flip 0's head) only
     * arms the dump and writes no image. */
    CHECK(file_exists(dir, "present_s00001_f00001.ppm"),
          "present 1 is named by its own sequence and the flip its head retired (1)");
    CHECK(file_exists(dir, "present_s00002_f00002.ppm"),
          "present 2 exists after the pass and names the flip its head retired (2)");
    CHECK(count_prefix(dir, "present_s") == 2,
          "two present images in all, the skipped host presents wrote none (%d)",
          count_prefix(dir, "present_s"));
    /* A pass is named by the last head counted when it opened. */
    CHECK(file_exists(dir, "s00002_f00001_pass000_t0.ppm"),
          "the pass image carries the frame's present sequence and the head it follows");

    char* text = slurp(dir);
    CHECK(text && strstr(text, "present_s00001:") && strstr(text, "present_s00002:"),
          "the index names each present by its own sequence");
    CHECK(text && strstr(text, "flip=00000") && strstr(text, "flip=00001"),
          "the index lines carry the flip ordinal");
    CHECK(text && strstr(text, "s=2 flip=00001"),
          "the pass line carries the frame identity");
    free(text);

    clear_dir(dir);
    rmdir(dir);
    free(stub_guest);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}
