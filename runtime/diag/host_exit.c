/*
 * ps3recomp - host process exit (see include/ps3emu/host_exit.h)
 */
#include "ps3emu/host_exit.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

void ps3_host_exit(int status)
{
    /* Two guest threads can exit at once, and calling quick_exit a second time
     * is undefined (C11 7.22.4.7). The first caller ends the process; any other
     * parks until it does, as a thread that lv2 has already stopped. */
    static atomic_flag s_exiting = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&s_exiting)) {
        for (;;) {
#ifdef _WIN32
            Sleep(INFINITE);
#else
            pause();
#endif
        }
    }
    /* quick_exit does not flush stdio, and no handler can flush output other
     * handlers printed after it. Get everything buffered out before the
     * quick-exit handlers run. */
    fflush(NULL);
    quick_exit(status);
}
