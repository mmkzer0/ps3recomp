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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Called by every generated SPU image before it registers its function table. */
void ps3_spu_lifter_abi_check(uint32_t got);

/* The lifted-code ABI this runtime implements. Bump it together with
 * PPU_LIFTER_ABI in tools/ppu_lifter.py whenever generated code must change to
 * stay correct against the runtime (a new store hook, a changed hook ABI). */
#define PPU_RUNTIME_LIFTER_ABI 2u

/* Generated SPU code and its inline context helpers/macros read these members.
 * Keep the ordered field list stable: an offset or size change requires an ABI
 * bump and a new pin.
 *
 * The signature depends on the host data model: spu_channel.lock is a
 * `volatile long`, so it is 8 bytes under LP64 (macOS/Linux) and 4 under
 * Windows' LLP64, which moves every pinned field after the channels. Each
 * supported model has its own measured pin; an unlisted model is a compile
 * error, never a guess. */
#define SPU_RUNTIME_LIFTER_LP64_CONTEXT_LAYOUT_SIGNATURE_PIN UINT64_C(0x8B375EA16245430D)
#define SPU_RUNTIME_LIFTER_WIN64_CONTEXT_LAYOUT_SIGNATURE_PIN UINT64_C(0x918F8F23859E16E5)
#define SPU_RUNTIME_LIFTER_ABI 1u
#if defined(_WIN32)
#  if !defined(_WIN64)
#    error "SPU lifter ABI: no verified spu_context layout pin for 32-bit Windows"
#  endif
#  define SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE_PIN \
       SPU_RUNTIME_LIFTER_WIN64_CONTEXT_LAYOUT_SIGNATURE_PIN
#elif defined(__LP64__) || defined(_LP64)
#  define SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE_PIN \
       SPU_RUNTIME_LIFTER_LP64_CONTEXT_LAYOUT_SIGNATURE_PIN
#else
#  error "SPU lifter ABI: no verified spu_context layout pin for this data model"
#endif
#define PS3EMU_SPULIFTER_LAYOUT_STEP(hash, value) \
    ((((uint64_t)(hash)) ^ (uint64_t)(value)) * UINT64_C(1099511628211))
#define PS3EMU_SPULIFTER_LAYOUT_MEMBER(hash, type, member) \
    PS3EMU_SPULIFTER_LAYOUT_STEP( \
        PS3EMU_SPULIFTER_LAYOUT_STEP((hash), offsetof(type, member)), \
        sizeof(((type*)0)->member))
#define SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE_BASE(type) \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
        UINT64_C(14695981039346656037), type, gpr), \
        type, pc), \
        type, status), \
        type, stop_code), \
        type, image_id), \
        type, event_status), \
        type, event_mask), \
        type, srr0), \
        type, int_enable), \
        type, host_depth), \
        type, reloc_delta)
#define SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE(type) \
    PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
        PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
            PS3EMU_SPULIFTER_LAYOUT_MEMBER( \
                SPU_RUNTIME_LIFTER_CONTEXT_LAYOUT_SIGNATURE_BASE(type), \
                type, ls), \
            type, policy_mode), \
        type, steps)

/* Compare the ABI the linked lift was generated for (the `ppu_lifter_abi`
 * symbol its source defines) with PPU_RUNTIME_LIFTER_ABI. Returns when they
 * match; otherwise stops (lifter-abi) with both values. The loader calls this
 * before it reads the executable, so a mismatched pair never starts a guest. */
void ps3_lifter_abi_check(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_LIFTER_ABI_H */
