/*
 * ps3recomp - tests/video_resolutions: video.resolutions (docs/CONFIG.md) and the three
 * cellVideoOut answers that must agree with it.
 *
 *   test_video_resolutions <mode>      mode is the case; the value under test comes in through
 *                                      PS3RECOMP_VIDEO_RESOLUTIONS, which ctest sets per test
 *   default      unset: no restriction. Every id (0..20) is available on the primary output,
 *                none on the secondary; the device lists 480, 576, 720, 1080; GetState and
 *                GetConfiguration report 720 with pitch 1280*4.
 *   only720      "2":  only id 2 is available; the device lists exactly it; state 720.
 *   only1080     "1":  only id 1; state 1080 and pitch 1920*4 before any Configure.
 *   pair         "4,1": ids 1 and 4; the device lists 480 then 1080; state is the first listed
 *                (480: 720 is not in the set), pitch 720*4.
 *   extended     "13,10": the 1600x1080 and 960x1080 modes are listed and available, in that order.
 *   configure    "1,2": a title that configures 1080 sees GetState/GetConfiguration follow it.
 *
 * In every case availability, the device's mode list and the state's resolution are checked
 * against one another, not only against the literals above.
 *
 * An invalid list is rejected by the config loader (tests/gates/test_config.py, and the ctest
 * `video_resolutions_invalid` here, which expects the config-invalid stop).
 */
#include "cellVideoOut.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base = NULL;

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

#define STATE_EA 0x1000u
#define INFO_EA  0x2000u
#define CFG_EA   0x3000u
#define VM_SIZE  0x10000u

static uint8_t g8(uint32_t ea) { return vm_base[ea]; }
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int read_state(void)
{
    memset(vm_base + STATE_EA, 0xEE, sizeof(CellVideoOutState));
    CHECK(cellVideoOutGetState(0, 0, (CellVideoOutState*)(uintptr_t)STATE_EA) == 0, "GetState");
    return g8(STATE_EA + offsetof(CellVideoOutState, displayMode) + offsetof(CellVideoOutDisplayModeBytes, resolutionId));
}

/* The device's modes into out[]; returns the count. */
static int read_modes(int* out)
{
    memset(vm_base + INFO_EA, 0, sizeof(CellVideoOutDeviceInfo));
    CHECK(cellVideoOutGetDeviceInfo(0, 0, (CellVideoOutDeviceInfo*)(uintptr_t)INFO_EA) == 0, "GetDeviceInfo");
    const int n = g8(INFO_EA + offsetof(CellVideoOutDeviceInfo, availableModeCount));
    for (int i = 0; i < n; ++i)
        out[i] = g8(INFO_EA + offsetof(CellVideoOutDeviceInfo, availableModes) +
                    (uint32_t)i * sizeof(CellVideoOutDisplayMode) +
                    offsetof(CellVideoOutDisplayMode, resolutionId));
    return n;
}

static void read_config(int* res, uint32_t* pitch)
{
    memset(vm_base + CFG_EA, 0xEE, sizeof(CellVideoOutConfiguration));
    CHECK(cellVideoOutGetConfiguration(0, (CellVideoOutConfiguration*)(uintptr_t)CFG_EA, NULL) == 0, "GetConfiguration");
    *res = g8(CFG_EA + offsetof(CellVideoOutConfiguration, resolutionId));
    *pitch = g32(CFG_EA + offsetof(CellVideoOutConfiguration, pitch));
}

static int avail(u32 id) { return cellVideoOutGetResolutionAvailability(0, id, CELL_VIDEO_OUT_ASPECT_16_9, 0); }

static void expect(const char* what, const int* want_modes, int n_want, int want_state, uint32_t want_pitch,
                   int restricted)
{
    int modes[32];
    const int n = read_modes(modes);
    CHECK(n == n_want, "%s: %d modes listed, want %d", what, n, n_want);
    for (int i = 0; i < n && i < n_want; ++i)
        CHECK(modes[i] == want_modes[i], "%s: mode %d is %d, want %d", what, i, modes[i], want_modes[i]);

    const int st = read_state();
    CHECK(st == want_state, "%s: GetState reports %d, want %d", what, st, want_state);
    int cres;
    uint32_t pitch;
    read_config(&cres, &pitch);
    CHECK(cres == want_state && pitch == want_pitch, "%s: GetConfiguration %d pitch %u, want %d / %u", what, cres,
          pitch, want_state, want_pitch);

    /* Agreement: an id is available exactly when the device lists it (restricted), every listed
     * mode is available, the reported state is available, and the secondary output has none. */
    for (u32 id = 0; id <= 20; ++id) {
        int listed = 0;
        for (int i = 0; i < n; ++i)
            listed |= modes[i] == (int)id;
        const int a = avail(id);
        if (restricted)
            CHECK(a == listed, "%s: id %u availability %d but listed %d", what, id, a, listed);
        else
            CHECK(a == 1, "%s: id %u availability %d, want 1 (no restriction)", what, id, a);
        if (listed)
            CHECK(a == 1, "%s: listed id %u not available", what, id);
        CHECK(cellVideoOutGetResolutionAvailability(1, id, 0, 0) == 0, "%s: secondary output reports id %u", what, id);
    }
    CHECK(avail((u32)st) == 1, "%s: the state's resolution %d is not available", what, st);
}

int main(int argc, char** argv)
{
    if (argc != 2) { printf("usage: %s <mode>\n", argv[0]); return 2; }
    vm_base = (uint8_t*)calloc(1, VM_SIZE);
    if (!vm_base) return 1;
    const char* mode = argv[1];

    if (!strcmp(mode, "default")) {
        static const int m[] = {4, 5, 2, 1};
        expect("default", m, 4, 2, 1280u * 4, 0);
    } else if (!strcmp(mode, "only720")) {
        static const int m[] = {2};
        expect("only720", m, 1, 2, 1280u * 4, 1);
    } else if (!strcmp(mode, "only1080")) {
        static const int m[] = {1};
        expect("only1080", m, 1, 1, 1920u * 4, 1);
    } else if (!strcmp(mode, "pair")) {
        static const int m[] = {4, 1};
        expect("pair", m, 2, 4, 720u * 4, 1);
    } else if (!strcmp(mode, "extended")) {
        static const int m[] = {10, 13};
        expect("extended", m, 2, 10, 1600u * 4, 1);
    } else if (!strcmp(mode, "configure")) {
        static const int m[] = {2, 1};
        expect("configure/before", m, 2, 2, 1280u * 4, 1);
        memset(vm_base + CFG_EA, 0, sizeof(CellVideoOutConfiguration));
        vm_base[CFG_EA] = CELL_VIDEO_OUT_RESOLUTION_1080;
        vm_base[CFG_EA + 1] = CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8;
        vm_base[CFG_EA + 2] = CELL_VIDEO_OUT_ASPECT_16_9;
        CHECK(cellVideoOutConfigure(0, (CellVideoOutConfiguration*)(uintptr_t)CFG_EA, NULL, 0) == 0, "Configure");
        expect("configure/after", m, 2, 1, 1920u * 4, 1);
    } else {
        printf("unknown mode %s\n", mode);
        return 2;
    }
    if (g_fail) { printf("video_resolutions %s: FAILED\n", mode); return 1; }
    printf("video_resolutions %s: all checks passed\n", mode);
    return 0;
}
