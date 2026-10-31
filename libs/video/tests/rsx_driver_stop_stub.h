/*
 * ps3recomp - the rsx-driver-method stop for fixtures that link the draw engine without
 * runtime/diag/stop.cpp
 *
 * The engine ends a flip it cannot resolve in ps3_stop_rsx_driver_method(). A fixture that drives
 * only well-formed flips (display buffers registered, a surface rendered) never reaches it, so it
 * stubs the symbol: a fixture that does reach it has a bug, says which word, and ends with the stop
 * status. A fixture that expects the stop links the real runtime/diag/stop.cpp instead
 * (test_rsx_flip_semantics.c) and does not include this header.
 *
 * Include it once per executable, like gcm_fixture_stubs.h.
 */
#ifndef PS3RECOMP_RSX_DRIVER_STOP_STUB_H
#define PS3RECOMP_RSX_DRIVER_STOP_STUB_H

#include "ps3emu/stop.h"

#include <stdio.h>
#include <stdlib.h>

PS3_NORETURN void ps3_stop_rsx_driver_method(uint32_t method, uint32_t head, uint32_t arg,
                                             uint32_t buffer, const char* reason)
{
    printf("[FAIL] rsx-driver-method method 0x%04X head %u arg 0x%08X buffer 0x%X reason %s\n",
           method, head, arg, buffer, reason ? reason : "unknown");
    exit(70);
}

#endif
