/*
 * ps3recomp - ps3_host_work_interval_* (include/ps3emu/host_work_interval.h)
 *
 * What a unit test can hold without a host that grants the mechanism:
 *   off        host.work_interval false: join/begin/end return, print nothing, stop nothing.
 *   unjoined   option on, no join: begin and end are no-ops on every host.
 *   nohost     option on where the host has no interval workgroup (not macOS): join ends the run
 *              with config-invalid naming host.work_interval. On macOS the join is a run-time
 *              check against a title, so this scenario skips (77).
 *   edge <us>  host.work_interval_us at a range edge loads and reads back.
 *   bad       host.work_interval_us outside 50..16000: config-invalid naming the key.
 *
 * One scenario per process: the configuration loads once from the environment (ctest sets it).
 *
 * Build:
 *   cc -std=gnu17 -Wall -Wextra -I include runtime/platform/tests/test_host_work_interval.c \
 *      runtime/platform/host_work_interval.c runtime/config/config.cpp runtime/diag/stop.cpp \
 *      runtime/diag/ps3_log.c runtime/diag/host_exit.c   (+ -framework AudioToolbox on macOS)
 *
 * Exit status is the number of failed checks, 77 when the scenario does not apply.
 */
#include "ps3emu/host_work_interval.h"

#include "ps3emu/config.h"
#include "ps3emu/stop.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
#define CHECKF(cond, ...) do { if (cond) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); fprintf(stderr, __VA_ARGS__); \
    fputc('\n', stderr); } } while (0)

/* A stop on this thread lands here instead of ending the process. */
static jmp_buf g_stopped;
static char g_kind[32], g_what[512];
static void on_stop(void* user, ps3_stop_kind kind, const char* what)
{
    (void)user;
    snprintf(g_kind, sizeof g_kind, "%s", ps3_stop_kind_id(kind));
    snprintf(g_what, sizeof g_what, "%s", what);
    longjmp(g_stopped, 1);
}

/* Everything the runtime printed to stderr while `body` ran. */
static char g_log[2048];
static void capture_begin(int* saved, FILE** cap)
{
    *cap = tmpfile();
    const int tmp = *cap ? fileno(*cap) : -1;
    *saved = tmp >= 0 ? dup(2) : -1;
    CHECKF(tmp >= 0 && *saved >= 0, "capture file");
    fflush(stderr);
    if (tmp >= 0 && *saved >= 0) dup2(tmp, 2);
}
static void capture_end(int saved, FILE* cap)
{
    fflush(stderr);
    g_log[0] = 0;
    if (saved < 0 || !cap) return;
    lseek(fileno(cap), 0, SEEK_SET);
    size_t n = 0; ssize_t r;
    while (n < sizeof g_log - 1 && (r = read(fileno(cap), g_log + n, sizeof g_log - 1 - n)) > 0) n += (size_t)r;
    g_log[n] = 0;
    dup2(saved, 2); close(saved);
    fclose(cap);
}

static int scenario_off(void)
{
    CHECKF(!ps3_cfg_host_work_interval(), "the option is not off by default");
    int saved; FILE* cap;
    capture_begin(&saved, &cap);
    ps3_stop_divert_begin(on_stop, NULL);
    int stopped = 0;
    if (setjmp(g_stopped) == 0) {
        ps3_host_work_interval_join("test thread");
        ps3_host_work_interval_begin();
        ps3_host_work_interval_end();
        ps3_host_work_interval_begin();
        ps3_host_work_interval_end();
        ps3_stop_divert_end();
    } else {
        stopped = 1;
    }
    capture_end(saved, cap);
    CHECKF(!stopped, "off stopped: %s: %s", g_kind, g_what);
    CHECKF(g_log[0] == 0, "off printed: %s", g_log);
    return g_fail;
}

static int scenario_unjoined(void)
{
    CHECKF(ps3_cfg_host_work_interval(), "the environment did not turn the option on");
    int saved; FILE* cap;
    capture_begin(&saved, &cap);
    ps3_stop_divert_begin(on_stop, NULL);
    int stopped = 0;
    if (setjmp(g_stopped) == 0) {
        ps3_host_work_interval_end();     /* nothing open */
        ps3_host_work_interval_begin();   /* never joined */
        ps3_host_work_interval_end();
        ps3_stop_divert_end();
    } else {
        stopped = 1;
    }
    capture_end(saved, cap);
    CHECKF(!stopped, "unjoined begin/end stopped: %s: %s", g_kind, g_what);
    CHECKF(g_log[0] == 0, "unjoined begin/end printed: %s", g_log);
    return g_fail;
}

static int scenario_nohost(void)
{
#if defined(__APPLE__)
    return 77;
#else
    CHECKF(ps3_cfg_host_work_interval(), "the environment did not turn the option on");
    int saved; FILE* cap;
    capture_begin(&saved, &cap);
    ps3_stop_divert_begin(on_stop, NULL);
    int stopped = 0;
    if (setjmp(g_stopped) == 0) {
        ps3_host_work_interval_join("test thread");
        ps3_stop_divert_end();
    } else {
        stopped = 1;
    }
    capture_end(saved, cap);
    CHECKF(stopped, "join with the option on returned on a host with no interval workgroup");
    CHECKF(strcmp(g_kind, "config-invalid") == 0, "kind '%s'", g_kind);
    CHECKF(strstr(g_what, "host.work_interval") != NULL, "stop does not name the option: %s", g_what);
    CHECKF(strstr(g_log, "[work-interval]") != NULL, "no reason line before the stop: %s", g_log);
    return g_fail;
#endif
}

static int scenario_edge(const char* want)
{
    const uint32_t us = ps3_cfg_host_work_interval_us();
    CHECKF(us == (uint32_t)strtoul(want, NULL, 10), "deadline reads %u, wanted %s", (unsigned)us, want);
    return g_fail;
}

static int scenario_bad(void)
{
    ps3_stop_divert_begin(on_stop, NULL);
    int stopped = 0;
    if (setjmp(g_stopped) == 0) {
        (void)ps3_cfg_host_work_interval_us();   /* first read loads, and refuses */
        ps3_stop_divert_end();
    } else {
        stopped = 1;
    }
    CHECKF(stopped, "an out-of-range deadline loaded");
    CHECKF(strcmp(g_kind, "config-invalid") == 0, "kind '%s'", g_kind);
    CHECKF(strstr(g_what, "host.work_interval_us") != NULL && strstr(g_what, "out of range") != NULL,
           "stop does not name the key and the range: %s", g_what);
    return g_fail;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s off|unjoined|nohost|edge <us>|bad\n", argv[0]); return 2; }
    int rc;
    if (!strcmp(argv[1], "off")) rc = scenario_off();
    else if (!strcmp(argv[1], "unjoined")) rc = scenario_unjoined();
    else if (!strcmp(argv[1], "nohost")) rc = scenario_nohost();
    else if (!strcmp(argv[1], "edge") && argc == 3) rc = scenario_edge(argv[2]);
    else if (!strcmp(argv[1], "bad")) rc = scenario_bad();
    else { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
    if (rc != 77) printf("host_work_interval %s: %d passed, %d failed\n", argv[1], g_pass, g_fail);
    return rc;
}
