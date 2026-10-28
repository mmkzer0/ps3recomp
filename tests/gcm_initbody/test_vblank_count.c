/*
 * ps3recomp - tests/gcm_initbody/test_vblank_count.c
 *
 * cellGcmGetVBlankCount returns the vblank counter and does not move it. RPCS3 (pinned
 * d00d80231af3f9b0286f38fd72f7513583f1a9d4) rpcs3/Emu/Cell/Modules/cellGcmSys.cpp:889-894 returns its
 * counter unchanged; only a raised vblank advances it. The HLE's getter used to be `return s_vblank_count++`,
 * so a caller pacing itself by the count (the sceNpTrophy status sequence) saw a vblank per query.
 *
 * Checked, each against the count read before it, not against a constant the code under test could share:
 *   - any number of getter calls with no vblank between them return one value;
 *   - each raised vblank adds exactly 1, through every entry point that raises one
 *     (cellGcmTickVBlank, cellGcm_vblank_signal, cellGcm_request_tick), and the getter reads between them
 *     change nothing;
 *   - the vblank handler's argument is the count, its last call of a pump the current one, consecutive;
 *   - a vblank thread raising vblanks while another thread polls the getter: the polled values never go back,
 *     and the final count is the base plus the vblanks raised, so a polling reader advanced nothing
 *     (run under ThreadSanitizer by the `tsan` label).
 *
 * Synthetic configuration, nothing recorded.
 *
 *   ctest --test-dir build/dev -R gcm_vblank_count --output-on-failure
 */
#include "cellGcmSys.h"
#include "ps3emu/guest_call.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern uint32_t ppu_vm_size;
extern uint32_t ppu_hle_inject_base;
extern void vm_write32(uint64_t addr, uint32_t v);
extern void cellGcm_vblank_signal(void);
extern void cellGcm_request_tick(void);
extern void cellGcmTickVBlank(void);
extern void ppu_gcm_pump(void);

uint8_t* vm_base = NULL;

#define IO_ADDR    0x00100000u
#define IO_SIZE    0x00400000u
#define HEAP       0x00800000u
#define OPD_VBLANK 0x00020010u
#define CODE_VBLANK 0x00030010u

static int g_fail;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        if (!(c)) {                                                            \
            g_fail++;                                                          \
            printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static uint32_t g_heap = HEAP;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void host_w32(u32 ea, u32 v) { vm_write32(ea, v); }

static void scene_gcm(void)
{
    cellGcmTerminate();
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, 0x10000u, IO_SIZE, IO_ADDR, host_alloc, host_w32);
}

static unsigned g_handler_calls;
static uint64_t g_handler_arg[16];
static void fake_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                        uint64_t a6, uint64_t a7)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    if (opd == OPD_VBLANK && g_handler_calls < 16) g_handler_arg[g_handler_calls++] = a0;
}

static void test_getter_is_pure(void)
{
    scene_gcm();
    const uint32_t a = cellGcmGetVBlankCount();
    const uint32_t b = cellGcmGetVBlankCount();
    CHECK(a == b, "two queries with no vblank between them: %u then %u", a, b);
    for (int i = 0; i < 1000; i++) (void)cellGcmGetVBlankCount();
    CHECK(cellGcmGetVBlankCount() == a, "1000 more queries moved the count: %u, was %u", cellGcmGetVBlankCount(), a);
}

static void test_one_per_vblank(void)
{
    scene_gcm();
    uint32_t c = cellGcmGetVBlankCount();

    cellGcmTickVBlank();
    CHECK(cellGcmGetVBlankCount() == c + 1, "cellGcmTickVBlank: %u, want %u", cellGcmGetVBlankCount(), c + 1);
    c++;
    CHECK(cellGcmGetVBlankCount() == c, "a second query after the vblank moved the count");

    cellGcm_vblank_signal();
    CHECK(cellGcmGetVBlankCount() == c + 1, "cellGcm_vblank_signal: %u, want %u", cellGcmGetVBlankCount(), c + 1);
    c++;

    cellGcm_request_tick();
    CHECK(cellGcmGetVBlankCount() == c + 1, "cellGcm_request_tick: %u, want %u", cellGcmGetVBlankCount(), c + 1);
    c++;

    for (int i = 0; i < 7; i++) cellGcmTickVBlank();
    CHECK(cellGcmGetVBlankCount() == c + 7, "7 vblanks: %u, want %u", cellGcmGetVBlankCount(), c + 7);
}

static void test_handler_argument(void)
{
    scene_gcm();
    g_ps3_guest_caller = fake_caller;
    vm_write32(OPD_VBLANK, CODE_VBLANK);
    vm_write32(OPD_VBLANK + 4, 0x00029000u);
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)(uintptr_t)OPD_VBLANK);
    ppu_gcm_pump();                       /* nothing pending from the setup */
    g_handler_calls = 0;

    const uint32_t base = cellGcmGetVBlankCount();
    cellGcmTickVBlank();
    cellGcmTickVBlank();
    cellGcmTickVBlank();
    (void)cellGcmGetVBlankCount();        /* a query between the raise and the delivery changes nothing */
    ppu_gcm_pump();
    CHECK(g_handler_calls == 3, "one handler call per vblank raised: %u, want 3", g_handler_calls);
    CHECK(g_handler_calls == 3 && g_handler_arg[0] == base + 1 && g_handler_arg[1] == base + 2 &&
          g_handler_arg[2] == base + 3,
          "handler arguments %llu %llu %llu, want %u %u %u", (unsigned long long)g_handler_arg[0],
          (unsigned long long)g_handler_arg[1], (unsigned long long)g_handler_arg[2], base + 1, base + 2, base + 3);
    CHECK(cellGcmGetVBlankCount() == base + 3, "the delivery moved the count: %u, want %u", cellGcmGetVBlankCount(),
          base + 3);
    g_ps3_guest_caller = NULL;
}

#define RAISED 20000u
static void* vblank_thread(void* arg)
{
    (void)arg;
    for (unsigned i = 0; i < RAISED; i++) cellGcm_vblank_signal();
    return NULL;
}

static void test_threads(void)
{
    scene_gcm();
    const uint32_t base = cellGcmGetVBlankCount();
    pthread_t t;
    if (pthread_create(&t, NULL, vblank_thread, NULL)) { CHECK(0, "pthread_create"); return; }
    uint32_t last = base, polls = 0, backwards = 0;
    while (cellGcmGetVBlankCount() < base + RAISED) {
        const uint32_t v = cellGcmGetVBlankCount();
        if (v < last) backwards++;
        last = v;
        polls++;
    }
    pthread_join(t, NULL);
    CHECK(backwards == 0, "the polled count went backwards %u times", backwards);
    CHECK(cellGcmGetVBlankCount() == base + RAISED, "after %u vblanks raised and %u polls: %u, want %u", RAISED,
          polls, cellGcmGetVBlankCount(), base + RAISED);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x40000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { printf("[FAIL] guest VM alloc\n"); return 1; }
    ppu_vm_size = vm_size;

    test_getter_is_pure();
    test_one_per_vblank();
    test_handler_argument();
    test_threads();

    if (g_fail) { printf("%d check(s) failed\n", g_fail); return 1; }
    printf("GCM vblank count checks passed\n");
    return 0;
}
