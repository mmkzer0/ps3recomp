/*
 * Test fixture for sys_crashdump:
 * - sys_crash_dump_set_user_log_area (NID: 0xB20F87B3)
 * - sys_crash_dump_get_user_log_area (NID: 0x52696620)
 */

#include "../sys_crashdump.c"
#include "ppu_context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern void ppu_hle_init(void);
extern void ps3_hle_call(uint32_t nid, ppu_context* ctx);

static uint8_t s_mem[0x10000];
uint8_t* vm_base = s_mem;
uint32_t ppu_vm_size = sizeof(s_mem);

uint32_t g_ww_lo = 0, g_ww_hi = 0;
void* g_ps3_guest_caller = NULL;
uint32_t g_ydkj_real_spurs_ea = 0, g_ydkj_real_taskset_ea = 0;

uint32_t prx_resolve_export(uint32_t x) { (void)x; return 0; }
void ps3_indirect_call(ppu_context* ctx) { (void)ctx; abort(); }
uint64_t ppu_guest_call(uint32_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                        uint64_t f, uint64_t g, uint64_t h, uint64_t i) {
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;
    abort();
}
void ppu_prof_stamp(void* a, unsigned b) { (void)a; (void)b; }
void ppu_gcm_pump(void) {}
uint32_t ppu_prof_resolve_host(void* a) { (void)a; return 0; }
void ps3_msf(const char* fmt, ...) { (void)fmt; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void ppu_resv_break_store(uint64_t a) { (void)a; }
void ps3_ww_report_inline(uint32_t a, uint64_t b, int c) { (void)a; (void)b; (void)c; }
void ppu_dump_guest_stack(ppu_context* a, const char* b) { (void)a; (void)b; }
void ppu_dump_bctrl_ring(uint32_t a, const char* b) { (void)a; (void)b; }
void ppu_guest_callstack(const char* a) { (void)a; }

static void test_range_stop(void)
{
    int pipes[2];
    assert(pipe(pipes) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);

        ppu_context ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.gpr[3] = 0;
        ctx.gpr[4] = 0x20000; /* beyond ppu_vm_size = 0x10000 */
        ps3_hle_call(0xB20F87B3u, &ctx);
        _exit(1);
    }
    close(pipes[1]);
    char out[1024] = {0};
    char buf[256];
    ssize_t n;
    while ((n = read(pipes[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        strncat(out, buf, sizeof(out) - strlen(out) - 1);
    }
    close(pipes[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 70);
    assert(strstr(out, "kind=hle-guest-range") != NULL);
    assert(strstr(out, "arg=new_entry") != NULL);
}

int main(void)
{
    ppu_hle_init();

    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);

    const uint32_t NID_SET = 0xB20F87B3u;
    const uint32_t NID_GET = 0x52696620u;

    const uint32_t entry_ea = 0x1000;
    const uint32_t out_ea   = 0x2000;

    /* Setup a valid log area descriptor: "TestLog", addr 0x3000, size 512 */
    memset(s_mem, 0, sizeof s_mem);
    strncpy((char*)s_mem + entry_ea, "TestLog", 16);
    vm_write32(entry_ea + 16, 0x3000);
    vm_write32(entry_ea + 20, 512);

    /* 1. Valid registration at index 127 */
    ctx.gpr[3] = 127;
    ctx.gpr[4] = entry_ea;
    ps3_hle_call(NID_SET, &ctx);
    assert(ctx.gpr[3] == 0); /* CELL_OK */

    /* 2. Retrieve the registered log area via GET */
    ctx.gpr[3] = 127;
    ctx.gpr[4] = out_ea;
    ps3_hle_call(NID_GET, &ctx);
    assert(ctx.gpr[3] == 0); /* CELL_OK */
    assert(strcmp((char*)s_mem + out_ea, "TestLog") == 0);
    assert(vm_read32(out_ea + 16) == 0x3000);
    assert(vm_read32(out_ea + 20) == 512);

    /* 3. Retrieve unpopulated slot (e.g. index 50) -> CELL_OK with zeroed fields (oracle) */
    memset(s_mem + out_ea, 0xAA, 24);
    ctx.gpr[3] = 50;
    ctx.gpr[4] = out_ea;
    ps3_hle_call(NID_GET, &ctx);
    assert(ctx.gpr[3] == 0);
    assert(s_mem[out_ea] == '\0');
    assert(vm_read32(out_ea + 16) == 0);
    assert(vm_read32(out_ea + 20) == 0);

    /* 4. Out-of-bounds index 128 -> CELL_EINVAL (0x80010002 sign-extended) */
    ctx.gpr[3] = 128;
    ctx.gpr[4] = entry_ea;
    ps3_hle_call(NID_SET, &ctx);
    assert(ctx.gpr[3] == 0xFFFFFFFF80010002ULL);

    /* 5. Out-of-bounds index 255 -> CELL_EINVAL */
    ctx.gpr[3] = 255;
    ctx.gpr[4] = entry_ea;
    ps3_hle_call(NID_SET, &ctx);
    assert(ctx.gpr[3] == 0xFFFFFFFF80010002ULL);

    /* 6. NULL pointer -> CELL_EINVAL */
    ctx.gpr[3] = 0;
    ctx.gpr[4] = 0;
    ps3_hle_call(NID_SET, &ctx);
    assert(ctx.gpr[3] == 0xFFFFFFFF80010002ULL);

    ctx.gpr[3] = 0;
    ctx.gpr[4] = 0;
    ps3_hle_call(NID_GET, &ctx);
    assert(ctx.gpr[3] == 0xFFFFFFFF80010002ULL);

    /* 7. GET with out-of-bounds index -> CELL_EINVAL */
    ctx.gpr[3] = 128;
    ctx.gpr[4] = out_ea;
    ps3_hle_call(NID_GET, &ctx);
    assert(ctx.gpr[3] == 0xFFFFFFFF80010002ULL);

    /* 8. Out-of-range address fails closed with hle-guest-range in subprocess */
    test_range_stop();

    printf("PASS: sys_crashdump test\n");
    return 0;
}
