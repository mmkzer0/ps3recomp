#include "ps3emu/lifter_abi.h"
#include "ps3emu/stop.h"

/* Defined by the linked lift: tools/ppu_lifter.py writes
 * `const uint32_t ppu_lifter_abi = PPU_LIFTER_ABI;` into the generated source
 * (the function-table file) from the macro in the generated header. Declared
 * here rather than in a shared header on purpose: this check is the only
 * consumer, and a header that promised the symbol to every other translation
 * unit would let a lift without it link and run unchecked. A lift made before
 * the symbol existed therefore fails to link the loader -- loud, never silent. */
#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t ppu_lifter_abi;
#ifdef __cplusplus
}
#endif

void ps3_lifter_abi_check(void)
{
    const uint32_t got = ppu_lifter_abi;
    if (got != PPU_RUNTIME_LIFTER_ABI)
        ps3_stop_lifter_abi(PPU_RUNTIME_LIFTER_ABI, got);
}
