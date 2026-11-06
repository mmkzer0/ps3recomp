/*
 * ps3recomp - tests/spu_kernel_range
 *
 * The HLE SPURS kernel's selectWorkload service (a policy module's indirect branch to
 * SPURS_PM_SELECT_WORKLOAD_LS, spu_channels.c spu_indirect_branch) copies the SPURS instance's first 0x80
 * bytes into the kernel context's tempArea at LS 0x100. The instance's EA is read from LS 0x1C4, which the
 * policy module and the jobs it ran write, so it is a guest span the title chose: one that is not inside
 * guest memory is the named stop hle-guest-range, not a host read past vm_base.
 *
 *   test_spu_kernel_range copy       an instance inside the arena is copied, the service returns to lr
 *   test_spu_kernel_range straddle   the 0x80 bytes run past the arena's end: stops
 *   test_spu_kernel_range far        an EA far outside the arena: stops
 *
 * The stop ends the process (exit 70, machine line on stderr), so ctest matches the line (CMakeLists.txt).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu_context.h"
#include "ppu_memory.h"

/* Guest memory, and what the SPU channel layer expects of its host (none of it is on the path driven here). */
uint8_t* vm_base;
uint32_t ppu_vm_size = 0;
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0, g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0, g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

void spu_indirect_branch(spu_context* ctx);

#define ARENA   0x10000u
#define LR      0x1230u

static int g_fail;
#define CHECK(c, ...) do { if (c) printf("[PASS] " __VA_ARGS__); else { g_fail++; printf("[FAIL] " __VA_ARGS__); } \
                           printf("\n"); } while (0)

/* The policy module's call: pc at the selectWorkload address, the instance EA in the kernel context, lr in r0. */
static spu_context* select_workload(uint32_t instance_ea)
{
    spu_context* c = (spu_context*)calloc(1, sizeof *c);   /* 256 KB LS: not on the stack */
    if (!c) abort();
    spu_context_init(c, 0);
    c->policy_mode = 1;
    c->pc = SPURS_PM_SELECT_WORKLOAD_LS;
    c->gpr[0]._u32[0] = LR;
    c->ls[0x1C4] = (uint8_t)(instance_ea >> 24); c->ls[0x1C5] = (uint8_t)(instance_ea >> 16);
    c->ls[0x1C6] = (uint8_t)(instance_ea >> 8);  c->ls[0x1C7] = (uint8_t)instance_ea;
    spu_indirect_branch(c);
    return c;
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s copy|straddle|far\n", argv[0]); return 2; }
    static uint8_t arena[ARENA];
    vm_base = arena;
    ppu_vm_size = ARENA;
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!strcmp(argv[1], "copy")) {
        const uint32_t ea = ARENA - 0x80u;        /* the last line of the arena: fits exactly */
        for (uint32_t i = 0; i < 0x80; i++) arena[ea + i] = (uint8_t)(0x40 + i);
        spu_context* c = select_workload(ea);
        CHECK(memcmp(c->ls + 0x100, arena + ea, 0x80) == 0, "the instance's first line is copied to tempArea");
        CHECK(c->pc == LR, "the service returns to lr (pc 0x%X)", c->pc);
        free(c);
        return g_fail ? 1 : 0;
    }
    if (!strcmp(argv[1], "straddle")) {
        select_workload(ARENA - 0x40u);
        printf("[FAIL] a line running past the arena's end was copied\n");
        return 1;
    }
    if (!strcmp(argv[1], "far")) {
        select_workload(0xFFFFFF00u);
        printf("[FAIL] a line far outside the arena was copied\n");
        return 1;
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
