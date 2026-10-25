/* The stamp a lift carries: tools/ppu_lifter.py writes this definition into a
 * lift's generated source from PPU_LIFTER_ABI in its header. The driver builds
 * this file with PPU_PROBE_ABI set to the value under test. */

#include "ps3emu/lifter_abi.h"

#include <stdint.h>

const uint32_t ppu_lifter_abi = PPU_PROBE_ABI;
