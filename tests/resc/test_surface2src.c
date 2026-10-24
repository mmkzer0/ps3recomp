/*
 * ps3recomp - tests/resc: cellRescGcmSurface2RescSrc (NID 0x01220224)
 *
 * Guest-memory in, guest-memory out, big-endian on both sides.
 *
 *   - NULL surface or NULL destination -> 0x80210304, destination untouched;
 *   - no initialisation check (works before cellRescInit, at 1x1 scale);
 *   - format byte: A8R8G8B8 -> 0x85 (swizzle) / 0xA5 (pitch), F_W16Z16Y16X16 ->
 *     0x9A / 0xBA, any other colour format or surface type -> 0xFF;
 *   - pitch = colorPitch[0], offset = colorOffset[0] (colour target 0);
 *   - width/height scale for anti-aliasing only when the init config's size is
 *     24 or 28 bytes: antialias 3 -> 2x1, 4 and 5 -> 2x2, others 1x1.
 *
 * Exit code 0 when every check passes.
 */
#include "cellResc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t ppu_vm_size;
uint8_t* vm_base = NULL;

#define SURF_EA  0x1000u
#define DST_EA   0x2000u
#define CFG_EA   0x3000u
#define VM_SIZE  0x10000u

static int g_fail;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            g_fail = 1;                                                        \
        }                                                                      \
    } while (0)

static void gw8(uint32_t ea, uint8_t v) { vm_base[ea] = v; }
static void gw16(uint32_t ea, uint16_t v) { vm_base[ea] = (uint8_t)(v >> 8); vm_base[ea + 1] = (uint8_t)v; }
static void gw32(uint32_t ea, uint32_t v)
{
    vm_base[ea] = (uint8_t)(v >> 24); vm_base[ea + 1] = (uint8_t)(v >> 16);
    vm_base[ea + 2] = (uint8_t)(v >> 8); vm_base[ea + 3] = (uint8_t)v;
}
static uint16_t gr16(uint32_t ea) { return (uint16_t)((vm_base[ea] << 8) | vm_base[ea + 1]); }
static uint32_t gr32(uint32_t ea)
{
    return ((uint32_t)vm_base[ea] << 24) | ((uint32_t)vm_base[ea + 1] << 16) |
           ((uint32_t)vm_base[ea + 2] << 8) | vm_base[ea + 3];
}

/* CellGcmSurface: type@0 antialias@1 colorFormat@2 colorOffset[4]@8
 * colorPitch[4]@0x18 width@0x34 height@0x36. Distinct values in every colour
 * target so reading the wrong one shows. */
static void make_surface(uint8_t type, uint8_t aa, uint8_t color_format, uint16_t w, uint16_t h)
{
    memset(vm_base + SURF_EA, 0, 0x40);
    gw8(SURF_EA + 0, type);
    gw8(SURF_EA + 1, aa);
    gw8(SURF_EA + 2, color_format);
    for (unsigned i = 0; i < 4; ++i) {
        gw32(SURF_EA + 8 + 4 * i, 0x11110000u * (i + 1) + 0x100);
        gw32(SURF_EA + 0x18 + 4 * i, 0x2000u * (i + 1));
    }
    gw16(SURF_EA + 0x34, w);
    gw16(SURF_EA + 0x36, h);
}

static s32 convert(void)
{
    memset(vm_base + DST_EA, 0xEE, 16);
    return cellRescGcmSurface2RescSrc((const void*)(uintptr_t)SURF_EA, (void*)(uintptr_t)DST_EA);
}

static void init_with_size(u32 size)
{
    cellRescExit();
    memset(vm_base + CFG_EA, 0, 28);
    gw32(CFG_EA, size);
    gw32(CFG_EA + 8, 0x0C);   /* supportModes: any non-zero, to prove fields decode */
    CHECK(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "Init(size=%u)", size);
}

static void check_dst(u32 fmt, u32 pitch, u32 w, u32 h, u32 offset, const char* what)
{
    CHECK(gr32(DST_EA + 0) == fmt, "%s: format 0x%X, want 0x%X", what, gr32(DST_EA + 0), fmt);
    CHECK(gr32(DST_EA + 4) == pitch, "%s: pitch 0x%X, want 0x%X", what, gr32(DST_EA + 4), pitch);
    CHECK(gr16(DST_EA + 8) == w, "%s: width %u, want %u", what, gr16(DST_EA + 8), w);
    CHECK(gr16(DST_EA + 10) == h, "%s: height %u, want %u", what, gr16(DST_EA + 10), h);
    CHECK(gr32(DST_EA + 12) == offset, "%s: offset 0x%X, want 0x%X", what, gr32(DST_EA + 12), offset);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = VM_SIZE;

    const u32 off0 = 0x11110100u, pitch0 = 0x2000u;

    /* Before cellRescInit: no init check, scale 1x1. */
    make_surface(1, 4, 8, 1280, 720);
    CHECK(convert() == 0, "convert before init");
    check_dst(0xA5, pitch0, 1280, 720, off0, "before init, pitch A8R8G8B8");

    /* Format byte table. */
    make_surface(2, 0, 8, 640, 360);
    CHECK(convert() == 0, "swizzle A8R8G8B8");
    check_dst(0x85, pitch0, 640, 360, off0, "swizzle A8R8G8B8");
    make_surface(1, 0, 11, 640, 360);
    CHECK(convert() == 0, "pitch F16");
    check_dst(0xBA, pitch0, 640, 360, off0, "pitch F16");
    make_surface(2, 0, 11, 640, 360);
    CHECK(convert() == 0, "swizzle F16");
    check_dst(0x9A, pitch0, 640, 360, off0, "swizzle F16");
    make_surface(1, 0, 3, 640, 360);       /* CELL_GCM_SURFACE_B8 */
    CHECK(convert() == 0, "unsupported colour format");
    check_dst(0xFF, pitch0, 640, 360, off0, "unsupported colour format");
    make_surface(3, 0, 8, 640, 360);       /* a type that is neither pitch nor swizzle */
    CHECK(convert() == 0, "unsupported surface type");
    check_dst(0xFF, pitch0, 640, 360, off0, "unsupported surface type");
    make_surface(0, 0, 8, 640, 360);
    CHECK(convert() == 0, "surface type 0");
    check_dst(0xFF, pitch0, 640, 360, off0, "surface type 0");

    /* NULL arguments: BAD_ARGUMENT, destination untouched. */
    make_surface(1, 0, 8, 640, 360);
    memset(vm_base + DST_EA, 0xEE, 16);
    CHECK((u32)cellRescGcmSurface2RescSrc(NULL, (void*)(uintptr_t)DST_EA) == 0x80210304u, "NULL surface");
    CHECK((u32)cellRescGcmSurface2RescSrc((const void*)(uintptr_t)SURF_EA, NULL) == 0x80210304u, "NULL dst");
    CHECK((u32)cellRescGcmSurface2RescSrc(NULL, NULL) == 0x80210304u, "NULL both");
    CHECK(gr32(DST_EA) == 0xEEEEEEEEu && gr32(DST_EA + 12) == 0xEEEEEEEEu, "destination touched by a failed call");

    /* Anti-aliasing scale: only with a 24- or 28-byte init config. */
    static const struct { u32 size; u8 aa; u16 w, h; } aa_cases[] = {
        {28, 0, 1, 1}, {28, 1, 1, 1}, {28, 2, 1, 1}, {28, 3, 2, 1}, {28, 4, 2, 2}, {28, 5, 2, 2}, {28, 6, 1, 1},
        {24, 0, 1, 1}, {24, 3, 2, 1}, {24, 4, 2, 2}, {24, 5, 2, 2}, {24, 255, 1, 1},
        {20, 3, 1, 1}, {20, 4, 1, 1}, {20, 5, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(aa_cases) / sizeof(aa_cases[0]); ++i) {
        init_with_size(aa_cases[i].size);
        make_surface(1, aa_cases[i].aa, 8, 640, 360);
        CHECK(convert() == 0, "aa convert");
        char what[64];
        snprintf(what, sizeof what, "init size %u, antialias %u", aa_cases[i].size, aa_cases[i].aa);
        check_dst(0xA5, pitch0, 640u * aa_cases[i].w, 360u * aa_cases[i].h, off0, what);
    }

    /* A width that does not fit 16 bits after scaling is truncated as `sth` does. */
    init_with_size(28);
    make_surface(1, 4, 8, 40000, 1);
    CHECK(convert() == 0, "wide surface");
    CHECK(gr16(DST_EA + 8) == (uint16_t)(40000u * 2u), "width truncation");

    /* After Exit the library object keeps working without an init check. */
    cellRescExit();
    make_surface(1, 4, 8, 640, 360);
    CHECK(convert() == 0, "convert after exit");

    free(vm_base);
    if (g_fail) { printf("resc_surface2src: FAILED\n"); return 1; }
    printf("resc_surface2src: all checks passed\n");
    return 0;
}
