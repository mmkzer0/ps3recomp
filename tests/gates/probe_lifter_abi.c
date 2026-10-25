/* Runs the runtime's lifted-code ABI check. The lift's stamp (the
 * `ppu_lifter_abi` symbol a real lift defines, see ps3emu/lifter_abi.h) comes
 * from stamp_lifter_abi.c, which the driver builds with PPU_PROBE_ABI set to the
 * value under test -- once the runtime's own and once a different one. Linked
 * without that stamp, the same file is the "lift from before the gate existed"
 * case: the runtime cannot link, which is the point. */

#include "ps3emu/lifter_abi.h"

int main(void)
{
    ps3_lifter_abi_check();
    return 0;
}
