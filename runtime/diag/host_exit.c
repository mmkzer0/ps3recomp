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

/*
 * Why there is a table of our own on Apple platforms: libSystem exports
 * at_quick_exit and quick_exit in iOS 18.0 but not in iOS 16.0 or 17.0 (the
 * export lists of libsystem_c in those dyld shared caches), yet the iOS SDK
 * declares both in _stdlib.h with no availability annotation. Neither
 * -Werror=unguarded-availability-new nor the linker flags a build with a lower
 * deployment target that imports them, and dyld refuses to load the app
 * ("Symbol not found: _at_quick_exit"). Both are therefore weak imports, tested
 * at run time; where either is absent the handlers live in this LIFO table and
 * ps3_host_exit runs them and ends the process with _Exit(), which is what
 * quick_exit does after its handlers.
 *
 * PS3_HOST_EXIT_FORCE_TABLE is the unit test's way onto the table path on a
 * host whose libc has quick_exit (tests/test_host_at_quick_exit.c).
 */
#if !defined(_WIN32) && (defined(__APPLE__) || defined(PS3_HOST_EXIT_FORCE_TABLE))
#define PS3_HOST_EXIT_TABLE 1
#include <pthread.h>
#endif

#if defined(__APPLE__)
extern int at_quick_exit(void (*)(void)) __attribute__((weak_import));
extern void quick_exit(int) __attribute__((weak_import));
#endif

#ifdef PS3_HOST_EXIT_TABLE
/* C11 7.22.4.3 requires room for 32. */
#define PS3_QUICK_EXIT_MAX 32

static pthread_mutex_t s_qe_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*s_qe_fn[PS3_QUICK_EXIT_MAX])(void);
static unsigned s_qe_count;   /* guarded by s_qe_lock */

static int libc_has_quick_exit(void)
{
#if defined(PS3_HOST_EXIT_FORCE_TABLE)
    return 0;
#else
    return at_quick_exit != NULL && quick_exit != NULL;
#endif
}

static void run_table(void)
{
    /* Pop under the lock, run outside it: a handler may register another one
     * (C11 allows it), and that one runs next. */
    for (;;) {
        pthread_mutex_lock(&s_qe_lock);
        void (*fn)(void) = s_qe_count ? s_qe_fn[--s_qe_count] : NULL;
        pthread_mutex_unlock(&s_qe_lock);
        if (!fn) return;
        fn();
    }
}
#endif

int ps3_host_at_quick_exit(void (*fn)(void))
{
#ifdef PS3_HOST_EXIT_TABLE
    if (!libc_has_quick_exit()) {
        int full;
        pthread_mutex_lock(&s_qe_lock);
        full = s_qe_count >= PS3_QUICK_EXIT_MAX;
        if (!full) s_qe_fn[s_qe_count++] = fn;
        pthread_mutex_unlock(&s_qe_lock);
        return full;
    }
#endif
    return at_quick_exit(fn);
}

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
#ifdef PS3_HOST_EXIT_TABLE
    if (!libc_has_quick_exit()) {
        run_table();
        _Exit(status);
    }
#endif
    quick_exit(status);
}
