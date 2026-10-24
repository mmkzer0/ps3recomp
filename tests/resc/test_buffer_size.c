/*
 * ps3recomp - tests/resc: cellRescGetBufferSize (libresc 0x02004B48, size helper 0x020022A0) and
 * the pieces it reads: the per-mode Dsts records (cellRescSetDsts, 0x020006B4), the destination
 * buffer size SetDisplayMode takes from them, the colour-buffer count and the SDK version.
 *
 * Expected values are written out here from the firmware's rules, independently of the
 * implementation:
 *   buffer(mode)  = ((heightAlign + h - 1) & -heightAlign) * pitch      h = 480, 576, 720, 1080
 *   colour        = constant VRAM (policy bit 0): that buffer of the SetDisplayMode mode (taken at
 *                   that call) * (576: 6 with PAL 2..4, 3 with PAL 1, else 2; other modes 2);
 *                   otherwise max over the supported modes of buffer(mode) * GetNumColorBuffers(mode)
 *   vertex        = 384
 *   fragment      = constant VRAM: microcode size of the current program (0x10 DC, 0x60 E0, 0x1E0 E4, 0x240 E8);
 *                   else SDK > 0x27FFFF: 768; else config size 20: the program size,
 *                   24/28: interlaced ? (576 && PAL != 0 ? 640 : 512) : (576 supported && PAL 2..4 ? 96 : 16),
 *                   any other size: 0
 * The SDK version is the one syscall 25 reports: PS3_SDK_VERSION here (the title's own value reaches
 * it through ppu_load_elf, tools/test_proc_param.py).
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

#define CFG_EA   0x1000u
#define DSTS_EA  0x2000u
#define OUT_EA   0x3000u
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

static void gw32(uint32_t ea, uint32_t v)
{
    vm_base[ea] = (uint8_t)(v >> 24); vm_base[ea + 1] = (uint8_t)(v >> 16);
    vm_base[ea + 2] = (uint8_t)(v >> 8); vm_base[ea + 3] = (uint8_t)v;
}
static uint32_t gr32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct { u32 size, policy, support, pal, interlace, flip; } Cfg;

static void init(const Cfg* c)
{
    cellRescExit();
    gw32(CFG_EA + 0, c->size);
    gw32(CFG_EA + 4, c->policy);
    gw32(CFG_EA + 8, c->support);
    gw32(CFG_EA + 12, 0);
    gw32(CFG_EA + 16, c->pal);
    gw32(CFG_EA + 20, c->interlace);
    gw32(CFG_EA + 24, c->flip);
    s32 r = cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA);
    CHECK(r == 0, "Init 0x%08X", (u32)r);
}

static s32 set_dsts(u32 mode, u32 pitch, u32 align)
{
    gw32(DSTS_EA, 8);
    gw32(DSTS_EA + 4, pitch);
    gw32(DSTS_EA + 8, align);
    return cellRescSetDsts(mode, (const CellRescDsts*)(uintptr_t)DSTS_EA);
}

static u32 height_of(u32 mode) { return mode == 1 ? 480 : mode == 2 ? 576 : mode == 4 ? 720 : 1080; }
static u32 buffer(u32 mode, u32 pitch, u32 align) { return ((align + height_of(mode) - 1) & (0u - align)) * pitch; }
static u32 count_for(u32 mode, u32 pal)
{
    if (mode != 2) return 2;
    return (pal >= 2 && pal <= 4) ? 6 : pal == 1 ? 3 : 2;
}

static void sdk(const char* v)
{
    if (v) setenv("PS3_SDK_VERSION", v, 1); else unsetenv("PS3_SDK_VERSION");
}

static void get(const char* what, u32 color, u32 fragment)
{
    memset(vm_base + OUT_EA, 0xEE, 12);
    s32 r = cellRescGetBufferSize((u32*)(uintptr_t)(OUT_EA), (u32*)(uintptr_t)(OUT_EA + 4), (u32*)(uintptr_t)(OUT_EA + 8));
    CHECK(r == 0, "%s: returned 0x%08X", what, (u32)r);
    CHECK(gr32(OUT_EA) == color, "%s: colour 0x%X, want 0x%X", what, gr32(OUT_EA), color);
    CHECK(gr32(OUT_EA + 4) == 384, "%s: vertex %u, want 384", what, gr32(OUT_EA + 4));
    CHECK(gr32(OUT_EA + 8) == fragment, "%s: fragment %u, want %u", what, gr32(OUT_EA + 8), fragment);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) return 1;
    ppu_vm_size = VM_SIZE;

    /* Before init, and the optional outputs. */
    CHECK((u32)cellRescGetBufferSize(NULL, NULL, NULL) == 0x80210301u, "before init");
    CHECK((u32)cellRescSetDsts(4, (const CellRescDsts*)(uintptr_t)DSTS_EA) == 0x80210301u, "SetDsts before init");

    /* Odin: 720p only, minimum GPU load, config size 28, PAL_60_DROP, SDK 4.75. Dsts set after
     * SetDisplayMode, as the title does (rescg-002: pitch 0x1400, heightAlign 0x40). */
    sdk("0x475001");
    Cfg odin = {28, 2, 4, 1, 0, 0};
    init(&odin);
    CHECK(cellRescSetDisplayMode(4) == 0, "SetDisplayMode");
    CHECK(set_dsts(4, 0x1400, 0x40) == 0, "SetDsts");
    get("odin", buffer(4, 0x1400, 0x40) * 2, 768);
    CHECK(buffer(4, 0x1400, 0x40) * 2 == 0x780000, "odin literal: the oracle's second tile (RPCS3 general seq 64)");
    cellRescGetBufferSize(NULL, NULL, NULL);   /* every output is optional */
    memset(vm_base + OUT_EA, 0xEE, 12);
    CHECK(cellRescGetBufferSize((u32*)(uintptr_t)OUT_EA, NULL, NULL) == 0 && gr32(OUT_EA) == 0x780000 &&
          gr32(OUT_EA + 4) == 0xEEEEEEEEu && gr32(OUT_EA + 8) == 0xEEEEEEEEu, "only colour requested");

    /* No Dsts yet: every record is zero, the size is 0. */
    init(&odin);
    get("no dsts", 0, 768);

    /* Largest over the supported modes, each with its own record. */
    Cfg two = {28, 2, 4 | 8, 1, 0, 0};
    init(&two);
    set_dsts(4, 0x1400, 0x40);
    set_dsts(8, 0x2000, 0x40);
    get("720+1080", buffer(8, 0x2000, 0x40) * 2, 768);
    /* A record of an unsupported mode is ignored. */
    init(&odin);
    set_dsts(4, 0x1400, 0x40);
    set_dsts(8, 0x2000, 0x40);
    get("1080 record ignored", buffer(4, 0x1400, 0x40) * 2, 768);

    /* The colour-buffer count follows the PAL mode for 576 only. */
    for (u32 pal = 0; pal <= 4; ++pal) {
        Cfg c = {28, 2, 2, pal, 0, 0};
        init(&c);
        set_dsts(2, 0x1000, 0x20);
        get("576 pal", buffer(2, 0x1000, 0x20) * count_for(2, pal), 768);
        Cfg d = {28, 2, 4, pal, 0, 0};
        init(&d);
        set_dsts(4, 0x1400, 0x40);
        get("720 ignores pal", buffer(4, 0x1400, 0x40) * 2, 768);
    }
    /* All four modes at once. */
    Cfg all = {28, 2, 1 | 2 | 4 | 8, 2, 0, 0};
    init(&all);
    set_dsts(1, 0x800, 0x20);
    set_dsts(2, 0x1000, 0x20);
    set_dsts(4, 0x1400, 0x40);
    set_dsts(8, 0x2000, 0x40);
    {
        u32 m = buffer(1, 0x800, 0x20) * 2, v;
        v = buffer(2, 0x1000, 0x20) * 6; if (v > m) m = v;
        v = buffer(4, 0x1400, 0x40) * 2; if (v > m) m = v;
        v = buffer(8, 0x2000, 0x40) * 2; if (v > m) m = v;
        get("all modes", m, 768);
    }

    /* SetDsts argument checks: NULL, and a mode that is not 1/2/4/8. */
    init(&odin);
    CHECK((u32)cellRescSetDsts(4, NULL) == 0x80210304u, "SetDsts NULL");
    for (u32 mode = 0; mode <= 16; ++mode) {
        if (mode == 1 || mode == 2 || mode == 4 || mode == 8) continue;
        CHECK((u32)set_dsts(mode, 0x1400, 0x40) == 0x80210304u, "SetDsts mode %u", mode);
    }
    CHECK((u32)cellRescSetDsts(0xFFFFFFFFu, (const CellRescDsts*)(uintptr_t)DSTS_EA) == 0x80210304u, "SetDsts mode -1");

    /* The SDK boundary: above 0x27FFFF the fragment buffer is 768, at or below it the config decides. */
    Cfg p28 = {28, 2, 4, 1, 0, 0};
    init(&p28);
    set_dsts(4, 0x1400, 0x40);
    sdk("0x280000"); get("sdk 0x280000", buffer(4, 0x1400, 0x40) * 2, 768);
    sdk("0x27FFFF"); get("sdk 0x27FFFF", buffer(4, 0x1400, 0x40) * 2, 16);
    sdk("0x270000"); get("sdk 0x270000", buffer(4, 0x1400, 0x40) * 2, 16);
    sdk(NULL);       get("sdk default", buffer(4, 0x1400, 0x40) * 2, 768);   /* 0x350001 */

    /* Below the threshold, by config size, interlace and PAL. */
    sdk("0x270000");
    {
        /* progressive: 96 when 576 is supported and PAL is 2..4, else 16 */
        for (u32 size = 24; size <= 28; size += 4)
            for (u32 pal = 0; pal <= 5; ++pal)
                for (u32 support = 1; support <= 15; ++support) {
                    Cfg c = {size, 2, support, pal, 0, 0};
                    init(&c);
                    const u32 want = ((support & 2) && pal >= 2 && pal <= 4) ? 96 : 16;
                    get("sdk<=0x27FFFF progressive", 0, want);
                }
        /* interlaced: 640 when the display mode is 576 and PAL != 0, else 512 */
        for (u32 pal = 0; pal <= 5; ++pal) {
            Cfg c = {28, 2, 2 | 4, pal, 1, pal == 5 ? 1 : 0};   /* PAL 5 needs a non-zero flip mode */
            init(&c);
            CHECK(cellRescSetDisplayMode(4) == 0, "SetDisplayMode 720 pal %u", pal);
            get("interlaced 720", 0, 512);
            CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 pal %u", pal);
            get("interlaced 576", 0, pal != 0 ? 640 : 512);
        }
        /* config size 20: the current program's microcode; any other size: 0 */
        Cfg s20 = {20, 2, 4, 1, 0, 0};
        init(&s20);
        get("size 20", 0, 0x10);
        Cfg s16 = {16, 2, 4, 1, 0, 0};
        init(&s16);
        get("size 16", 0, 0);
        Cfg s32 = {32, 2, 4, 1, 0, 0};
        init(&s32);
        get("size 32", 0, 0);
    }
    /* interlaced 576 */
    {
        Cfg c = {28, 2, 2, 2, 1, 0};
        init(&c);
        CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 interlaced");
        get("interlaced 576 pal 2", 0, 640);
        Cfg z = {28, 2, 2, 0, 1, 0};
        init(&z);
        CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 pal 0");
        get("interlaced 576 pal 0", 0, 512);
        /* size 20: the config carries no interlace or flip field (the firmware copies `size` bytes), so
         * the program is E0 or DC by PAL alone. */
        Cfg e = {20, 2, 2, 2, 1, 0};
        init(&e);
        CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 size 20");
        get("size 20, program E0", 0, 0x60);
        Cfg e0 = {20, 2, 4, 0, 1, 0};
        init(&e0);
        CHECK(cellRescSetDisplayMode(4) == 0, "SetDisplayMode 720 size 20");
        get("size 20, program DC", 0, 0x10);
    }

    /* Constant VRAM (policy bit 0): the size SetDisplayMode took from the record, the SDK does not matter. */
    sdk("0x270000");
    {
        Cfg c = {28, 1, 4, 1, 0, 0};
        init(&c);
        set_dsts(4, 0x1400, 0x40);
        CHECK(cellRescSetDisplayMode(4) == 0, "SetDisplayMode");
        get("constant VRAM 720", buffer(4, 0x1400, 0x40) * 2, 0x10);
        /* a record set after SetDisplayMode does not change it, and a mode set before any record gives 0 */
        init(&c);
        CHECK(cellRescSetDisplayMode(4) == 0, "SetDisplayMode first");
        set_dsts(4, 0x1400, 0x40);
        get("constant VRAM, Dsts after SetDisplayMode", 0, 0x10);
        /* 576: the colour-buffer count by PAL mode, the program by PAL and interlace */
        for (u32 pal = 0; pal <= 4; ++pal) {
            Cfg p = {28, 1, 2, pal, 0, 0};
            init(&p);
            set_dsts(2, 0x1000, 0x20);
            CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 pal %u", pal);
            get("constant VRAM 576", buffer(2, 0x1000, 0x20) * count_for(2, pal), (pal >= 2 && pal <= 4) ? 0x60 : 0x10);
        }
        Cfg i = {28, 1, 2, 3, 1, 0};
        init(&i);
        set_dsts(2, 0x1000, 0x20);
        CHECK(cellRescSetDisplayMode(2) == 0, "SetDisplayMode 576 interlaced");
        get("constant VRAM 576 interlaced", buffer(2, 0x1000, 0x20) * 6, 0x240);
    }
    sdk("0x475001");
    {
        Cfg c = {28, 1, 4, 1, 0, 0};
        init(&c);
        set_dsts(4, 0x1400, 0x40);
        cellRescSetDisplayMode(4);
        get("constant VRAM, SDK 4.75", buffer(4, 0x1400, 0x40) * 2, 0x10);
    }

    free(vm_base);
    if (g_fail) { printf("resc_buffer_size: FAILED\n"); return 1; }
    printf("resc_buffer_size: all checks passed\n");
    return 0;
}
