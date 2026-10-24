/*
 * ps3recomp - tests/resc: cellRescSetRegisterCount / cellRescGetRegisterCount
 * and the SetDisplayMode rules that pick the current convert program
 * (firmware libresc 020003A4, 020003D8, 02004774..02004A28).
 *
 *   - four convert fragment programs, default register counts 2, 3, 4, 4 for
 *     the +0xDC, +0xE0, +0xE4, +0xE8 program; Init selects +0xDC;
 *   - SetDisplayMode: not initialised 0x301; a mode outside supportModes or not
 *     1/2/4/8 0x304; 720x576 with a PAL mode the flip mode forbids 0x307; on
 *     success 720x576 + PAL_60_INTERPOLATE..FLEXIBLE picks E8 (interlaced) or E0,
 *     every other case E4 (interlaced) or DC;
 *   - Set patches the current program only, Get reads it back;
 *   - before Init (and after Exit) both stop as hle-uninitialized (run in a
 *     subprocess: the stop ends the process with status 70).
 *
 * Exit code 0 when every check passes.
 */
#include "cellResc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern uint32_t ppu_vm_size;
uint8_t* vm_base = NULL;

#define CFG_EA   0x1000u
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
    return ((uint32_t)vm_base[ea] << 24) | ((uint32_t)vm_base[ea + 1] << 16) |
           ((uint32_t)vm_base[ea + 2] << 8) | vm_base[ea + 3];
}

static void init_cfg(u32 support, u32 pal, u32 interlace, u32 flip)
{
    cellRescExit();
    gw32(CFG_EA + 0, 28);
    gw32(CFG_EA + 4, 2);
    gw32(CFG_EA + 8, support);
    gw32(CFG_EA + 12, 0);
    gw32(CFG_EA + 16, pal);
    gw32(CFG_EA + 20, interlace);
    gw32(CFG_EA + 24, flip);
    s32 r = cellRescInit((const CellRescInitConfig*)(uintptr_t)CFG_EA);
    CHECK(r == 0, "Init returned 0x%08X", (u32)r);
}

enum { DC, E0, E4, E8 };

/* The program a (mode, pal, interlace, flip) combination selects, or -1 when
 * SetDisplayMode must fail, with the expected return in *err. */
static int expected(u32 mode, u32 pal, u32 interlace, u32 flip, u32 support, u32* err)
{
    *err = 0;
    if ((mode & support) == 0) { *err = 0x80210304u; return -1; }
    if (mode != 1 && mode != 2 && mode != 4 && mode != 8) { *err = 0x80210304u; return -1; }
    if (mode == 2) {
        if ((pal >= 2 && pal <= 4) || pal == 1) {
            if (flip == 1) { *err = 0x80210307u; return -1; }
        } else if (pal == 5) {
            if (flip == 0) { *err = 0x80210307u; return -1; }
        }
    }
    if (mode == 2 && pal >= 2 && pal <= 4)
        return interlace == 1 ? E8 : E0;
    return interlace == 1 ? E4 : DC;
}

static int run_child(const char* self, const char* mode, char* out, size_t cap)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "'%s' %s 2>&1", self, mode);
    FILE* p = popen(cmd, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -2;
}

static void check_stop(const char* self, const char* mode, const char* func)
{
    char out[8192];
    int rc = run_child(self, mode, out, sizeof out);
    CHECK(rc == 70, "%s: exit status %d, want 70", mode, rc);
    char want[256];
    snprintf(want, sizeof want, "PS3STOP v=1 kind=hle-uninitialized func=%s module=cellResc", func);
    CHECK(strstr(out, want) != NULL, "%s: no '%s' in output", mode, want);
}

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = VM_SIZE;

    /* Child modes: the call under test must stop. */
    if (argc == 2) {
        if (!strcmp(argv[1], "uninit-set")) { cellRescSetRegisterCount(7); return 0; }
        if (!strcmp(argv[1], "uninit-get")) { (void)cellRescGetRegisterCount(); return 0; }
        if (!strcmp(argv[1], "exit-set")) { init_cfg(4, 0, 0, 0); cellRescExit(); cellRescSetRegisterCount(7); return 0; }
        if (!strcmp(argv[1], "exit-get")) { init_cfg(4, 0, 0, 0); cellRescExit(); (void)cellRescGetRegisterCount(); return 0; }
        return 2;
    }

    /* The accessor reads 0 while uninitialised. */
    CHECK(cellResc_convert_register_count() == 0, "accessor before init");

    /* SetDisplayMode before Init. */
    CHECK((u32)cellRescSetDisplayMode(CELL_RESC_1280x720) == 0x80210301u, "SetDisplayMode before init");

    /* Defaults, by selecting each program first (counts have not been set yet). */
    init_cfg(0xF, 0, 0, 0);
    CHECK(cellRescGetRegisterCount() == 2, "Init selects DC (default 2), got %d", cellRescGetRegisterCount());
    CHECK(cellResc_convert_register_count() == 2, "accessor DC");
    CHECK(cellRescSetDisplayMode(CELL_RESC_720x576) == 0 && cellRescGetRegisterCount() == 2, "576 PAL_50 -> DC");
    init_cfg(0xF, CELL_RESC_PAL_60_INTERPOLATE, 0, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_720x576) == 0 && cellRescGetRegisterCount() == 3, "576 INTERPOLATE -> E0 (3)");
    init_cfg(0xF, 0, 1, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_1280x720) == 0 && cellRescGetRegisterCount() == 4, "720 interlaced -> E4 (4)");
    init_cfg(0xF, CELL_RESC_PAL_60_INTERPOLATE, 1, 0);
    CHECK(cellRescSetDisplayMode(CELL_RESC_720x576) == 0 && cellRescGetRegisterCount() == 4, "576 INTERPOLATE interlaced -> E8 (4)");

    /* Set patches the current program only: mark all four with distinct values. */
    static const struct { u32 mode, pal, interlace; int prog; } route[] = {
        {CELL_RESC_1280x720, 0, 0, DC},
        {CELL_RESC_720x576, CELL_RESC_PAL_60_INTERPOLATE, 0, E0},
        {CELL_RESC_1280x720, 0, 1, E4},
        {CELL_RESC_720x576, CELL_RESC_PAL_60_INTERPOLATE, 1, E8},
    };
    for (int p = 0; p < 4; ++p) {
        init_cfg(0xF, route[p].pal, route[p].interlace, 0);
        CHECK(cellRescSetDisplayMode(route[p].mode) == 0, "route %d", p);
        cellRescSetRegisterCount((u8)(0x10 + p));
        CHECK(cellRescGetRegisterCount() == 0x10 + p, "route %d round trip", p);
    }
    for (int p = 0; p < 4; ++p) {
        init_cfg(0xF, route[p].pal, route[p].interlace, 0);
        CHECK(cellRescSetDisplayMode(route[p].mode) == 0, "route %d (again)", p);
        CHECK(cellRescGetRegisterCount() == 0x10 + p, "program %d kept its own value across Init, got 0x%X",
              p, (u32)cellRescGetRegisterCount());
        CHECK(cellResc_convert_register_count() == (u32)(0x10 + p), "accessor program %d", p);
    }
    /* Init selects DC again whatever the previous mode was. */
    init_cfg(0xF, 0, 0, 0);
    CHECK(cellRescGetRegisterCount() == 0x10, "Init re-selects DC");
    /* The argument is a byte; Get zero-extends it. */
    cellRescSetRegisterCount(0xFF);
    CHECK(cellRescGetRegisterCount() == 255, "0xFF reads back as 255, got %d", cellRescGetRegisterCount());
    cellRescSetRegisterCount(0);
    CHECK(cellRescGetRegisterCount() == 0, "0 reads back as 0");
    cellRescSetRegisterCount(0x10);

    /* The SetDisplayMode matrix: returns and selection. A failing call keeps the
     * selection Init made (DC, marked 0x10; the other programs are 0x11..0x13). */
    static const u32 supports[] = {0xF, 0x4, 0x2, 0x8};
    for (unsigned s = 0; s < 4; ++s)
    for (u32 mode = 0; mode <= 16; ++mode)
    for (u32 pal = 0; pal <= 6; ++pal)
    for (u32 il = 0; il <= 1; ++il)
    for (u32 flip = 0; flip <= 2; ++flip) {
        init_cfg(supports[s], pal, il, flip);
        u32 err;
        int prog = expected(mode, pal, il, flip, supports[s], &err);
        s32 r = cellRescSetDisplayMode(mode);
        if (prog < 0 && err != 0x80210307u) {
            CHECK((u32)r == err, "mode %u sup 0x%X pal %u: returned 0x%08X want 0x%08X", mode, supports[s], pal, (u32)r, err);
            CHECK(cellRescGetRegisterCount() == 0x10, "failed call changed the selection (mode %u)", mode);
            continue;
        }
        if (prog < 0) {   /* BAD_COMBINATION: the mode is stored, the selection untouched */
            CHECK((u32)r == err, "mode %u pal %u flip %u: returned 0x%08X want 0x%08X", mode, pal, flip, (u32)r, err);
            CHECK(cellRescGetRegisterCount() == 0x10, "BAD_COMBINATION changed the selection (mode %u)", mode);
            gw32(0x2000, 0);
            CHECK(cellRescGetDisplayMode((u32*)(uintptr_t)0x2000) == 0, "GetDisplayMode");
            CHECK(gr32(0x2000) == mode, "BAD_COMBINATION stores the mode: %u, want %u", gr32(0x2000), mode);
            continue;
        }
        CHECK(r == 0, "mode %u sup 0x%X pal %u il %u: returned 0x%08X", mode, supports[s], pal, il, (u32)r);
        static const u8 want[] = {0x10, 0x11, 0x12, 0x13};
        CHECK(cellRescGetRegisterCount() == want[prog],
              "mode %u pal %u il %u -> program %d, read %d", mode, pal, il, prog, cellRescGetRegisterCount());
    }

    /* Stops before Init and after Exit. */
    check_stop(argv[0], "uninit-set", "cellRescSetRegisterCount");
    check_stop(argv[0], "uninit-get", "cellRescGetRegisterCount");
    check_stop(argv[0], "exit-set", "cellRescSetRegisterCount");
    check_stop(argv[0], "exit-get", "cellRescGetRegisterCount");

    free(vm_base);
    if (g_fail) { printf("resc_register_count: FAILED\n"); return 1; }
    printf("resc_register_count: all checks passed\n");
    return 0;
}
