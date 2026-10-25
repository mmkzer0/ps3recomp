/*
 * ps3recomp - lifted-code ABI gate
 *
 * The lifter and the runtime are two halves of one contract: the generated code
 * expects the runtime to provide the hooks its ABI names (for example the RSX
 * label store hook, PPU_STORE_HOOK), and the runtime expects the generated code
 * to route everything that ABI covers through them. A lift built by one
 * ppu_lifter.py and run against a runtime written for another silently skips
 * whatever was added in between -- the store still happens, the hook never
 * runs, and nothing reports it. That is a wrong frame, not a missing feature.
 *
 * The lifter therefore stamps its ABI into the generated header
 * (PPU_LIFTER_ABI) and defines `const uint32_t ppu_lifter_abi` in the generated
 * source. The runtime keeps the value it implements here, references the
 * symbol, and compares the two at startup. A lift from an older lifter has no
 * symbol at all, which is a link error: the check cannot be skipped silently.
 */

#ifndef PS3EMU_LIFTER_ABI_H
#define PS3EMU_LIFTER_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The lifted-code ABI this runtime implements. Bump it together with
 * PPU_LIFTER_ABI in tools/ppu_lifter.py whenever generated code must change to
 * stay correct against the runtime (a new store hook, a changed hook ABI). */
#define PPU_RUNTIME_LIFTER_ABI 1u

/* Compare the ABI the linked lift was generated for (the `ppu_lifter_abi`
 * symbol its source defines) with PPU_RUNTIME_LIFTER_ABI. Returns when they
 * match; otherwise stops (lifter-abi) with both values. The loader calls this
 * before it reads the executable, so a mismatched pair never starts a guest. */
void ps3_lifter_abi_check(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_LIFTER_ABI_H */
