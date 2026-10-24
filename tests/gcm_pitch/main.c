/*
 * ps3recomp - tests/gcm_pitch: cellGcmGetTiledPitchSize against libgcm_sys's
 * own table.
 *
 * The firmware (libgcm_sys 0x0200126C) walks a 32-entry table (index 0 is a 0
 * sentinel; the tiled pitches 0x200 .. 0xE000 follow) and returns the first
 * entry that is at least `size`; a size above the last entry, and size 0, give
 * 0. The table below is written out here independently of the implementation.
 * Every size from 0 to 0x12000 is checked, which covers the 0xE001..0x10000
 * range the generic table used to round up to 0x10000.
 *
 * Exit code 0 when every check passes.
 */
#include "cellGcmSys.h"
#include <stdint.h>
#include <stdio.h>

uint8_t* vm_base = NULL;

static const u32 k_pitches[31] = {
    0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800, 0xA00, 0xC00, 0xD00, 0xE00,
    0x1000, 0x1400, 0x1800, 0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000, 0xC000, 0xD000, 0xE000,
};

static u32 reference(u32 size)
{
    if (size == 0)
        return 0;
    for (unsigned i = 0; i < 31; ++i)
        if (k_pitches[i] >= size)
            return k_pitches[i];
    return 0;
}

int main(void)
{
    int fail = 0;
    for (u32 size = 0; size <= 0x12000; ++size) {
        const u32 got = cellGcmGetTiledPitchSize(size), want = reference(size);
        if (got != want) {
            printf("[FAIL] size 0x%X -> 0x%X, want 0x%X\n", size, got, want);
            if (++fail > 20) break;
        }
    }
    struct { u32 size, want; } spot[] = {
        {0, 0}, {1, 0x200}, {0x200, 0x200}, {0x201, 0x300}, {0x1E00, 0x2000},   /* 0x1E00: the title's 1080p row */
        {0x1400, 0x1400}, {0xE000, 0xE000}, {0xE001, 0}, {0xFFFF, 0}, {0x10000, 0}, {0xFFFFFFFFu, 0},
    };
    for (unsigned i = 0; i < sizeof spot / sizeof spot[0]; ++i) {
        const u32 got = cellGcmGetTiledPitchSize(spot[i].size);
        if (got != spot[i].want) {
            printf("[FAIL] spot 0x%X -> 0x%X, want 0x%X\n", spot[i].size, got, spot[i].want);
            ++fail;
        }
    }
    if (fail) { printf("gcm_pitch: FAILED\n"); return 1; }
    printf("gcm_pitch: all checks passed\n");
    return 0;
}
