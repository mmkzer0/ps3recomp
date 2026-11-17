/*
 * ps3recomp - the run log's stop runtime for fixtures that supply their own stops
 *
 * A fixture that links runtime/diag/ps3_log.c but stubs the stop runtime itself (the single-TU GCM,
 * engine and Metal fixtures) cannot link runtime/diag/stop.cpp: the two would define the same symbols.
 * ps3_log_configure() is what installs the mirror, and these fixtures configure no log, so the mirror
 * here answers that reference. The configuration and the generic ps3_stop are weak so a fixture that
 * defines its own (the GCM and engine stub headers, test_depth_range_metal.m) keeps it; this stop
 * reports and ends with the stop status, the way the fixtures' own stubs do.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ps3emu/config.h"
#include "ps3emu/stop.h"

void ps3_stop_set_mirror(ps3_stop_quiesce_fn quiesce, ps3_stop_mirror_fn mirror)
{
    (void)quiesce;
    (void)mirror;
}

/* A fixture that answers the typed configuration itself keeps its version; these fixtures never
 * configure the log, so this one is a placeholder for the reference. */
__attribute__((weak)) const ps3_config* ps3_config_get(void)
{
    static ps3_config config;
    return &config;
}

__attribute__((weak)) PS3_NORETURN void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                                                 const ps3_stop_field* fields, size_t nfields,
                                                 const ps3_stop_site* site)
{
    (void)remedy; (void)site;
    printf("[FAIL] stop kind %d: %s", (int)kind, what);
    for (size_t i = 0; i < nfields; i++) printf(" %s=%s", fields[i].key, fields[i].value);
    printf("\n");
    fflush(stdout);
    _Exit(PS3_STOP_EXIT_CODE);
}
