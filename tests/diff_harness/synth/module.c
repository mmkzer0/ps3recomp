/* Synthetic PPU module for the differential harness' firmware-free CI fixture.
 *
 * Compiled for powerpc64-unknown-linux-gnu (big-endian ELFv1) by a PPC-capable
 * clang, linked with ld.lld and lifted by the toolkit's own ppu_lifter.py. No
 * libc, no globals, no jump tables: everything the lifted code needs arrives in
 * registers and guest memory. The NID suffixes are synthetic (0x91xxxxxx).
 *
 * The HLE implementations live in synth_hle.cpp; the deliberately wrong ones
 * mutate exactly one comparator dimension each (see cases_synth.cpp). */
typedef unsigned int u32;

#define EXPORT __attribute__((noinline, used))

/* SCE-shaped import stub, defined in module_asm.S. */
extern u32 synth_import_91000100(u32 a, u32 b);

/* return dimension */
EXPORT u32 synth_nid_91000001(u32 a) { return a * 3u + 1u; }
/* memory dimension: a write both sides must make */
EXPORT u32 synth_nid_91000002(u32 p) { *(volatile u32*)p = 0xDEADBEEFu; return 0; }
/* import dimension: the only observable effect is the call itself */
EXPORT u32 synth_nid_91000003(u32 a, u32 b) { return synth_import_91000100(a, b) + 1u; }
/* fault dimension: dereference an EA no mapping covers */
EXPORT u32 synth_nid_91000004(u32 p) { return *(volatile u32*)p; }
/* hang dimension */
EXPORT u32 synth_nid_91000005(u32 x)
{
    volatile u32 spin = x;
    for (;;) spin = spin * 3u + 1u;
    return spin;
}
/* no-HLE dimension (the HLE registers no handler for this NID) */
EXPORT u32 synth_nid_91000008(u32 a) { return a ^ 0x55u; }

/* correct firmware twins of the mutated HLE handlers */
EXPORT u32 synth_nid_91000011(u32 a) { return a * 3u + 1u; }
EXPORT u32 synth_nid_91000012(u32 p) { *(volatile u32*)p = 0xDEADBEEFu; return 0; }
EXPORT u32 synth_nid_91000013(u32 dst, u32 src)
{
    const volatile unsigned char* s = (const volatile unsigned char*)src;
    volatile unsigned char* d = (volatile unsigned char*)dst;
    for (u32 i = 0; i < 8; ++i) d[i] = s[i];
    return 0;
}
EXPORT u32 synth_nid_91000014(u32 a, u32 b) { return synth_import_91000100(a, b) + 1u; }
EXPORT u32 synth_nid_91000015(u32 p) { return *(volatile u32*)p; }
EXPORT u32 synth_nid_91000016(u32 p) { return *(volatile u32*)p; }
EXPORT u32 synth_nid_91000017(u32 p) { return *(volatile u32*)p; }
/* mask-boundary twin: same 8-byte copy; the case masks only part of the HLE's
 * extra padding, so the unmasked part must still diverge */
EXPORT u32 synth_nid_91000018(u32 dst, u32 src)
{
    const volatile unsigned char* s = (const volatile unsigned char*)src;
    volatile unsigned char* d = (volatile unsigned char*)dst;
    for (u32 i = 0; i < 8; ++i) d[i] = s[i];
    return 0;
}
