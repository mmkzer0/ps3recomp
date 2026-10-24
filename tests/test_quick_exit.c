/*
 * ps3recomp - a guest process exit must not run static destructors.
 *
 * The guest exit path ends the host process while the PPU, SPU, RSX and audio
 * host threads are still running. lv2 ends every thread before it tears
 * anything down; the host's faithful mapping is quick_exit(), which runs no
 * C++ static destructor and no atexit handler, so no global container or mutex
 * is destroyed underneath a live thread. exit() would run both.
 *
 * This fixture is that shape, small enough to check from a pipe:
 *
 *   - a diagnostic registers with at_quick_exit() and prints to stdout, then
 *     flushes it. stdout is a pipe here, so the line arrives only because the
 *     handler flushed; quick_exit does not flush anything itself.
 *   - a C++ object with static storage prints a marker from its destructor
 *     (tests/test_quick_exit_static.cpp). exit() runs it; quick_exit() must
 *     not.
 *   - a host thread keeps writing a global, as the frame clock and the RSX
 *     backend keep writing the runtime's state over a guest's exit.
 *
 * Then it calls sys_process_exit(7) and never returns. tests/run_quick_exit.py
 * asserts status 7, the diagnostic present, the destructor marker absent.
 */
#include "sysPrxForUser.h"          /* sys_process_exit */
#include "win32_compat.h"           /* CreateThread, Sleep: one host thread API */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The guest arena belongs to the host, as it does in tests/resc and
 * runtime/host/host_posix.c. This fixture never dereferences it: it is here
 * because runtime/host/host_stubs.c supplies the loader's other hooks for a
 * host that runs no lifted game. */
uint8_t* vm_base = NULL;

/* Written by the thread below for as long as the process lives. */
static volatile unsigned g_touched;

static DWORD WINAPI toucher(LPVOID arg)
{
    (void)arg;
    for (;;) {
        __atomic_add_fetch(&g_touched, 1u, __ATOMIC_RELAXED);
        Sleep(1);
    }
    return 0;
}

/* quick_exit does not flush stdio, so the handler flushes its own line. */
static void diagnostic(void)
{
    printf("[quick-exit] at_quick_exit diagnostic ran\n");
    fflush(NULL);
}

int main(void)
{
    at_quick_exit(diagnostic);

    CreateThread(NULL, 0, toucher, NULL, 0, NULL);

    /* Let the thread actually reach the global, so the exit happens under a
     * running thread rather than one that has only been created. */
    for (int i = 0; i < 2000 && !__atomic_load_n(&g_touched, __ATOMIC_RELAXED); i++)
        Sleep(1);
    if (!__atomic_load_n(&g_touched, __ATOMIC_RELAXED)) {
        fprintf(stderr, "[quick-exit] FAIL: the host thread never ran\n");
        return 2;
    }

    sys_process_exit(7);

    /* sys_process_exit does not return. */
    fprintf(stderr, "[quick-exit] FAIL: sys_process_exit returned\n");
    return 2;
}
