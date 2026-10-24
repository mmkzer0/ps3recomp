/*
 * ps3recomp - tests/resc
 *
 * cellResc error numbering and cellRescVideoOutResolutionId2RescBufferMode,
 * called as a title does: pointers are guest effective addresses into a
 * calloc'd guest memory, outputs are read back big-endian.
 *
 * Numbering (firmware libresc return sites): 0x301 NOT_INITIALIZED,
 * 0x302 REINITIALIZED, 0x303 BAD_ALIGNMENT, 0x304 BAD_ARGUMENT,
 * 0x305 LESS_MEMORY, 0x306 GCM_FLIP_QUE_FULL, 0x307 BAD_COMBINATION.
 *
 * cellRescVideoOutResolutionId2RescBufferMode: NULL output -> 0x80210304;
 * id 1 -> 8, 2 -> 4, 4 -> 1, 5 -> 2; any other id -> 0x80210304 with the output
 * untouched; no initialisation check.
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

#define OUT_EA   0x1000u
#define CFG_EA   0x2000u
#define SRC_EA   0x3000u
#define DSTS_EA  0x4000u
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

static uint32_t gr32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

#define OUT() ((u32*)(uintptr_t)OUT_EA)

static void test_numbering(void)
{
    CHECK(CELL_RESC_ERROR_NOT_INITIALIZED   == 0x80210301u, "NOT_INITIALIZED");
    CHECK(CELL_RESC_ERROR_REINITIALIZED     == 0x80210302u, "REINITIALIZED");
    CHECK(CELL_RESC_ERROR_BAD_ALIGNMENT     == 0x80210303u, "BAD_ALIGNMENT");
    CHECK(CELL_RESC_ERROR_BAD_ARGUMENT      == 0x80210304u, "BAD_ARGUMENT");
    CHECK(CELL_RESC_ERROR_LESS_MEMORY       == 0x80210305u, "LESS_MEMORY");
    CHECK(CELL_RESC_ERROR_GCM_FLIP_QUE_FULL == 0x80210306u, "GCM_FLIP_QUE_FULL");
    CHECK(CELL_RESC_ERROR_BAD_COMBINATION   == 0x80210307u, "BAD_COMBINATION");
}

static void test_id2mode(void)
{
    static const struct { u32 id, mode; } map[] = {
        {1, 8}, {2, 4}, {4, 1}, {5, 2},
    };
    /* Before cellRescInit: the function has no initialisation check. */
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        gw32(OUT_EA, 0xDEADBEEFu);
        s32 r = cellRescVideoOutResolutionId2RescBufferMode(map[i].id, OUT());
        CHECK(r == 0, "id %u returned 0x%08X", map[i].id, (u32)r);
        CHECK(gr32(OUT_EA) == map[i].mode, "id %u wrote 0x%08X, want %u", map[i].id, gr32(OUT_EA), map[i].mode);
    }
    /* The result is stored big-endian: id 1 -> mode 8 is bytes 00 00 00 08. */
    gw32(OUT_EA, 0);
    cellRescVideoOutResolutionId2RescBufferMode(1, OUT());
    CHECK(vm_base[OUT_EA + 0] == 0 && vm_base[OUT_EA + 1] == 0 &&
          vm_base[OUT_EA + 2] == 0 && vm_base[OUT_EA + 3] == 8, "not stored big-endian");

    static const u32 bad[] = {0, 3, 6, 7, 8, 0xFFFFFFFFu};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        gw32(OUT_EA, 0xDEADBEEFu);
        s32 r = cellRescVideoOutResolutionId2RescBufferMode(bad[i], OUT());
        CHECK((u32)r == 0x80210304u, "id %u returned 0x%08X, want 0x80210304", bad[i], (u32)r);
        CHECK(gr32(OUT_EA) == 0xDEADBEEFu, "id %u touched the output", bad[i]);
    }

    /* NULL output wins over a valid id. */
    s32 r = cellRescVideoOutResolutionId2RescBufferMode(1, NULL);
    CHECK((u32)r == 0x80210304u, "NULL output returned 0x%08X", (u32)r);
}

static void test_num_color_buffers(void)
{
    /* Callable before init; the count is the return value; reserved must be 0. */
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_1280x720, CELL_RESC_PAL_50, 0) == 2, "720p");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_1920x1080, CELL_RESC_PAL_60_INTERPOLATE, 0) == 2, "1080p ignores PAL mode");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_720x576, CELL_RESC_PAL_50, 0) == 2, "576 PAL_50");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_720x576, CELL_RESC_PAL_60_DROP, 0) == 3, "576 DROP");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_720x576, CELL_RESC_PAL_60_INTERPOLATE, 0) == 6, "576 INTERPOLATE");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_720x576, CELL_RESC_PAL_60_INTERPOLATE_30_DROP, 0) == 6, "576 30_DROP");
    CHECK(cellRescGetNumColorBuffers(CELL_RESC_720x576, CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE, 0) == 6, "576 FLEX");
    CHECK((u32)cellRescGetNumColorBuffers(CELL_RESC_1280x720, CELL_RESC_PAL_50, 1) == 0x80210304u, "reserved != 0");
}

static void test_error_paths(void)
{
    /* Before init. */
    CHECK((u32)cellRescSetDisplayMode(CELL_RESC_1280x720) == 0x80210301u, "SetDisplayMode before init");
    CHECK((u32)cellRescSetSrc(0, (const CellRescSrc*)(uintptr_t)SRC_EA) == 0x80210301u, "SetSrc before init");
    CHECK((u32)cellRescSetConvertAndFlip(NULL, 0) == 0x80210301u, "ConvertAndFlip before init");

    /* Init argument check, then re-init. */
    CHECK((u32)cellRescInit(NULL) == 0x80210304u, "Init(NULL)");
    memset(vm_base + CFG_EA, 0, 28);
    CHECK(cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0, "Init");
    CHECK((u32)cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA) == 0x80210302u, "Init twice");
    CHECK((u32)cellRescInit(NULL) == 0x80210302u, "Init(NULL) when initialised reports REINITIALIZED first");

    /* Bad arguments once initialised. */
    CHECK((u32)cellRescSetSrc(0, NULL) == 0x80210304u, "SetSrc NULL");
    CHECK((u32)cellRescSetSrc(-1, (const CellRescSrc*)(uintptr_t)SRC_EA) == 0x80210304u, "SetSrc -1");
    CHECK((u32)cellRescSetSrc(8, (const CellRescSrc*)(uintptr_t)SRC_EA) == 0x80210304u, "SetSrc 8");
    CHECK(cellRescSetSrc(7, (const CellRescSrc*)(uintptr_t)SRC_EA) == 0, "SetSrc 7");
    CHECK((u32)cellRescSetDsts(CELL_RESC_1280x720, NULL) == 0x80210304u, "SetDsts NULL");
    CHECK(cellRescSetDsts(CELL_RESC_1280x720, (const CellRescDsts*)(uintptr_t)DSTS_EA) == 0, "SetDsts");
    CHECK((u32)cellRescGetDisplayMode(NULL) == 0x80210304u, "GetDisplayMode NULL");
    CHECK((u32)cellRescGetLastFlipTime(NULL) == 0x80210304u, "GetLastFlipTime NULL");
    CHECK((u32)cellRescCreateInterlaceTable(NULL, 1.0f, 16, 0) == 0x80210304u, "CreateInterlaceTable NULL");

    /* The Id2BufferMode function is unaffected by the init state either way. */
    gw32(OUT_EA, 0);
    CHECK(cellRescVideoOutResolutionId2RescBufferMode(2, OUT()) == 0 && gr32(OUT_EA) == 4, "Id2Mode after init");

    cellRescExit();
    CHECK((u32)cellRescSetDisplayMode(CELL_RESC_1280x720) == 0x80210301u, "SetDisplayMode after exit");
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = VM_SIZE;

    /* Id2BufferMode first: it must work with cellResc never initialised. */
    test_numbering();
    test_id2mode();
    test_num_color_buffers();
    test_error_paths();

    free(vm_base);
    if (g_fail) { printf("resc: FAILED\n"); return 1; }
    printf("resc: all checks passed\n");
    return 0;
}
