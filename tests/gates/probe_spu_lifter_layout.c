#include "ps3emu/lifter_abi.h"
#include "spu_context.h"
#include <stdio.h>

int main(void)
{
    const uint64_t signature =
        SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE(spu_context);
    printf("%016llX\n", (unsigned long long)signature);
    return 0;
}
