#include <stdint.h>
#include "ps3emu/lifter_abi.h"

#ifndef SPU_PROBE_ABI
#error "SPU_PROBE_ABI must be supplied by the gate target"
#endif

extern void ps3_spu_lifter_abi_check(uint32_t got);

/* Mirrors the check emitted at the start of spu_recomp_register(). */
static void probe_spu_recomp_register(void)
{
    ps3_spu_lifter_abi_check(SPU_PROBE_ABI);
}

int main(void)
{
    probe_spu_recomp_register();
    return 0;
}
