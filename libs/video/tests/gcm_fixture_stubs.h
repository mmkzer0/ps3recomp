/* What the single-TU GCM fixtures (test_gcm_flip_stream.c, test_gcm_get_on_jump.c,
 * test_gcm_get_on_release.c) take from the rest of the runtime, beyond the graphics stubs each declares:
 * the typed configuration cellGcmSys.c reads (gcm.wait_timeout_ms) and the named stop it ends a wait in.
 * Include after "../cellGcmSys.c". The fixtures also link libs/video/gcm_default_state.c (see
 * tools/test_gcm_*.py).
 *
 * A wait that stands the whole timeout is a bug in a fixture, never a thing to pass: the stop stub ends
 * the process with the runtime's stop status (70) and says which wait it was. */
#ifndef PS3RECOMP_GCM_FIXTURE_STUBS_H
#define PS3RECOMP_GCM_FIXTURE_STUBS_H

#include <stdio.h>
#include <stdlib.h>

const ps3_config* ps3_config_get(void)
{
    static ps3_config cfg;
    cfg.gcm_wait_timeout_ms = 10000;   /* the default, docs/CONFIG.md */
    return &cfg;
}

void ps3_stop_gcm_wait_timeout(const char* func, const char* what, uint32_t offset, uint32_t value,
                               uint32_t current, uint32_t waited_ms)
{
    printf("[FAIL] gcm-wait-timeout in %s: %s offset 0x%08X value 0x%08X current 0x%08X after %u ms\n", func, what,
           offset, value, current, waited_ms);
    exit(70);
}

#endif
