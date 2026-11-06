/*
 * ps3recomp - an in-process ps3_stop_guest_range for fixtures that check span bounds
 *
 * A handler that is given a guest span outside guest memory ends the process with a named stop
 * (hle-guest-range). A fixture that links no runtime/diag/stop.cpp defines the symbol itself; this one
 * records the stop's facts and returns to the test through longjmp, so one process can refuse many spans:
 *
 *     GR_TRY(sceNpSignalingGetPeerNetInfo(ctx, conn, (u32*)EA(bad), NULL));
 *     CHECK(gr_named("sceNpSignalingGetPeerNetInfo", "peerAddr"));
 *
 * gr_named() is true when the call ended in the stop, naming that function and argument. A stop outside
 * a GR_TRY (nothing armed) prints what stopped and aborts, so a span a test did not mean to refuse is a
 * failure, not a silent pass; a check that misses does not stop at all and gr_named() is false.
 * Include it once per executable. Handlers must check before taking a lock: a longjmp past a held lock
 * would leave it held.
 */
#ifndef PS3RECOMP_TEST_GUEST_RANGE_CATCH_H
#define PS3RECOMP_TEST_GUEST_RANGE_CATCH_H

#include "../include/ps3emu/stop.h"   /* relative: a fixture's driver need not pass -I include */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf g_gr_jmp;
static int g_gr_armed, g_gr_hit;
static char g_gr_func[96], g_gr_arg[48];
static uint64_t g_gr_ea, g_gr_size, g_gr_limit;

#ifdef __cplusplus
extern "C" {
#endif
void ps3_stop_guest_range(const char* func, const char* arg, uint64_t ea, uint64_t size, uint64_t limit)
{
    if (!g_gr_armed) {
        fprintf(stderr, "unexpected hle-guest-range stop: func=%s arg=%s ea=0x%llX size=0x%llX limit=0x%llX\n",
                func, arg, (unsigned long long)ea, (unsigned long long)size, (unsigned long long)limit);
        abort();
    }
    snprintf(g_gr_func, sizeof g_gr_func, "%s", func);
    snprintf(g_gr_arg, sizeof g_gr_arg, "%s", arg);
    g_gr_ea = ea; g_gr_size = size; g_gr_limit = limit;
    g_gr_armed = 0;
    longjmp(g_gr_jmp, 1);
}
#ifdef __cplusplus
}
#endif

#define GR_TRY(call) do { \
        g_gr_hit = 0; g_gr_armed = 1; \
        if (setjmp(g_gr_jmp) == 0) { (void)(call); g_gr_armed = 0; } else g_gr_hit = 1; \
    } while (0)

static int gr_named(const char* func, const char* arg)
{
    return g_gr_hit && strcmp(g_gr_func, func) == 0 && strcmp(g_gr_arg, arg) == 0;
}

#endif
