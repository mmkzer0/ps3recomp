/*
 * test_ftrace_zst - the recorder under test for a PS3_FTRACE path that ends in .zst.
 *
 * The recorder reads PS3_FTRACE once and dumps once, at exit, so each scenario is one run of this program
 * with its own environment. tests/run_ftrace_zst.py sets PS3_FTRACE, PATH and the rest, runs the modes below
 * and inspects what the process left behind: its exit status, its stderr and the files at and beside the
 * output path. The runner decodes the output with tools/ftrace_report.py, the tool that reads these traces.
 *
 *   record        4 flip requests 20 ms apart, one HLE span and a thread name, then return from main (atexit)
 *   quick         the same, ended by quick_exit (at_quick_exit), the way a guest exit ends the process
 *   many          the same plus 300000 syscall spans (PS3_FTRACE_CAP must allow them): more bytes than a pipe
 *                 and the stdio buffer hold, so a dead child is met in the middle of the write, not at the close
 *   leak          the same after opening a descriptor that is not close-on-exec; its number goes to stdout
 *   closes-fds    prints 1 when the recorder closes such descriptors in the child on this platform, else 0
 *
 * Exit status: 0 recorded and ended cleanly; 3 recording was off (the recorder said why on stderr); 5 the dump
 * left SIGPIPE changed; 2, 4, 6 are setup failures of the test itself.
 * The span times are fixed offsets from the recorder's base, so the expected trace is exact.
 */
#include "ps3emu/ps3_ftrace.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { FLIPS = 4, MANY = 300000 };

/* Registered before the recorder's handlers, so it runs after the dump (atexit and at_quick_exit are LIFO):
 * the dump ignores SIGPIPE while it writes to the child and must put back what the process had: the default
 * disposition, which kills the process on a write to a dead pipe, so a dump that does not ignore it dies. */
static void check_sigpipe_restored(void)
{
    struct sigaction now;
    if (sigaction(SIGPIPE, NULL, &now) != 0 || now.sa_handler != SIG_DFL) {
        fprintf(stderr, "test_ftrace_zst: the dump left SIGPIPE changed\n");
        fflush(NULL);
        _exit(5);
    }
}

static uint64_t at_ms(double ms) { return g_ps3_ft.base + (uint64_t)(ms * 1e6); }

static int record(int many)
{
    if (!PS3_FT_ON()) return 3;
    ps3_ft_name(1, "main");
    for (int i = 0; i < FLIPS; i++)
        ps3_ft_span(PS3_FT_FLIP_REQ, 1, 0, 0, at_ms(20.0 * i), at_ms(20.0 * i));
    ps3_ft_span(PS3_FT_HLE, 1, 0x1573DC3Fu, 0x1000u, at_ms(5), at_ms(9));
    if (many)
        for (uint32_t i = 0; i < MANY; i++)
            ps3_ft_span(PS3_FT_SYSCALL, 2, 85, 0x2000u, at_ms(100 + i * 0.001), at_ms(100 + i * 0.001 + 0.0005));
    return 0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "record";
    if (!strcmp(mode, "closes-fds")) {
#ifdef PS3_FT_ZSTD_CLOSES_FDS
        puts("1");
#else
        puts("0");
#endif
        return 0;
    }
    if (!strcmp(mode, "leak")) {
        const int fd = open("/dev/null", O_RDONLY);   /* deliberately not close-on-exec */
        if (fd < 0) return 4;
        printf("%d\n", fd);
        fflush(stdout);
    } else if (strcmp(mode, "record") && strcmp(mode, "quick") && strcmp(mode, "many")) {
        fprintf(stderr, "test_ftrace_zst: unknown mode '%s'\n", mode);
        return 2;
    }
    if (signal(SIGPIPE, SIG_DFL) == SIG_ERR || atexit(check_sigpipe_restored) ||
        at_quick_exit(check_sigpipe_restored))
        return 6;
    const int rc = record(!strcmp(mode, "many"));
    if (rc) return rc;
    if (!strcmp(mode, "quick")) quick_exit(0);
    return 0;
}
