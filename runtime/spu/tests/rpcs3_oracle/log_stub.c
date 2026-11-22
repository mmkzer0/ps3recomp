/* The run log, for the differential test (build_diff_test.sh).
 *
 * spu_interp.c logs through ps3_log (PS3_LOG expands to ps3_log_site_admit and ps3_log_emit_admitted), and the
 * differential test compiles spu_interp.c into itself. The test compares instruction results against RPCS3;
 * its own output is the report it prints, and the interpreter's trace and statistics lines take no part in it.
 * So the log admits nothing: every PS3_LOG site is skipped, its arguments are never evaluated, and the real
 * log (which needs the typed configuration, the stop runtime and the quick-exit hook) stays out of this link.
 * ps3_log_emit_admitted is only reached after an admit that returned non-zero, so it is never called. */
#include "ps3emu/ps3_log.h"

int ps3_log_site_admit(ps3_log_site* site, ps3_log_level lvl)
{
    (void)site; (void)lvl;
    return 0;
}

void ps3_log_emit_admitted(ps3_log_site* site, ps3_log_level lvl, const char* fmt, ...)
{
    (void)site; (void)lvl; (void)fmt;
}
