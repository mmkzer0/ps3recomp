/*
 * ps3recomp - host process exit
 *
 * A guest process exit (sys_process_exit, cellGameExitToShelf, the boot
 * harness's fallthrough) ends the host process while the PPU, SPU, RSX and
 * audio host threads are still running. lv2 ends every thread before it tears
 * anything down; the host cannot, so the faithful mapping is quick_exit(), not
 * exit(): exit() runs atexit handlers and C++ static destructors while those
 * threads are still using the global mutexes, the config registry, the JQ2
 * registries and the std containers they protect. Tearing those down
 * underneath a live thread is undefined behaviour.
 *
 * quick_exit() does NOT flush stdio, so every at_quick_exit handler flushes
 * its own output before it returns.
 */

#ifndef PS3EMU_HOST_EXIT_H
#define PS3EMU_HOST_EXIT_H

#include "ps3emu/stop.h"   /* PS3_NORETURN */

#ifdef __cplusplus
extern "C" {
#endif

/* fflush(NULL), then quick_exit(status). Any path that ends the process while
 * guest threads are live goes through this. */
PS3_NORETURN void ps3_host_exit(int status);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_HOST_EXIT_H */
