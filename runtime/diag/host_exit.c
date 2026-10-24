/*
 * ps3recomp - host process exit (see include/ps3emu/host_exit.h)
 */
#include "ps3emu/host_exit.h"

#include <stdio.h>
#include <stdlib.h>

void ps3_host_exit(int status)
{
    /* quick_exit does not flush stdio, and no handler can flush output other
     * handlers printed after it. Get everything buffered out before the
     * quick-exit handlers run. */
    fflush(NULL);
    quick_exit(status);
}
