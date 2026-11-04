/*
 * lv2conf host backend: the conf.h case API over the toolkit's own lv2 HLE.
 *
 * The case sources (tools/hwprobe/probes/lv2conf/cases/) run unchanged on a console, where conf_sc is
 * a real `sc`, and here, where conf_sc fills a ppu_context and enters the syscall table the runtime
 * registers (lv2_init_syscalls). Both write the same rows, so the two runs can be diffed.
 *
 * Guest memory is the toolkit's own 4 GiB-aligned mapping (vm_init), big-endian like the guest's. The
 * scratch arena sits at 0x00100000, below 4 GiB and clear of every window the HLE hands out itself
 * (the lv2 allocator from 0x10000000, the loader TLS at 0x10F00000, the SPU image staging at
 * 0x0D800000). Address 0 stays unmapped: a handler that dereferences a NULL guest pointer faults, and
 * the runner records the crash; it does not silently land the store.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "conf.h"
#include "lv2conf_host.h"

#include "memory/vm.h"
#include "ppu/ppu_context.h"
#include "ps3emu/stop.h"
#include "syscalls/lv2_enosys_bitmap.h"
#include "syscalls/lv2_syscall_table.h"

/* runtime/syscalls/lv2_register.c */
extern void lv2_init_syscalls(void);
extern int lv2_try_syscall(ppu_context *ctx);

/* The runtime defines vm_base in ppu_loader.cpp, which a host without a lifted title does not link. */
uint8_t *vm_base = NULL;

#define SCRATCH_BASE 0x00100000u
#define SCRATCH_SIZE 0x00800000u
/* Guest thread id 1 is the main thread: ppu_tid_make(serial 0, slot 0). */
#define MAIN_THREAD_ID 1u

static struct lv2conf_host_status *g_status;
static FILE *g_rows;
static const char *g_id;
static uint32_t g_scratch_next;

int lv2conf_host_init(void)
{
    if (vm_init() != CELL_OK) {
        fprintf(stderr, "lv2conf: vm_init failed\n");
        return -1;
    }
    void *p = mmap(NULL, sizeof *g_status, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "lv2conf: cannot map the shared status block\n");
        return -1;
    }
    g_status = (struct lv2conf_host_status *)p;
    g_status->sc_num = UINT32_MAX;
    return 0;
}

struct lv2conf_host_status *lv2conf_host_status(void)
{
    return g_status;
}

void lv2conf_host_begin_case(const char *id, FILE *rows)
{
    g_id = id;
    g_rows = rows;
    g_scratch_next = SCRATCH_BASE;
    g_status->finished = 0;
    g_status->sc_num = UINT32_MAX;
    g_status->sc_handler = 0;
    lv2_init_syscalls();
}

/* ---- syscalls ---------------------------------------------------------------------------------- */

int64_t conf_sc(uint32_t num, unsigned nargs, const uint64_t *args)
{
    ppu_context ctx;
    ppu_context_clear(&ctx);
    ctx.thread_id = MAIN_THREAD_ID;
    ctx.gpr[11] = num;
    if (nargs > 8 || (nargs && !args)) {
        fprintf(stderr, "lv2conf: %s: conf_sc(%u) called with %u args\n", g_id, num, nargs);
        _exit(3);
    }
    for (unsigned i = 0; i < nargs; i++)
        ctx.gpr[3 + i] = args[i];

    g_status->sc_num = num;
    g_status->sc_handler = num < LV2_SYSCALL_MAX ? (uintptr_t)g_lv2_syscalls.handlers[num] : 0;

    /* The table lookup by slot number, as lv2_syscall_dispatch does it. lv2_try_syscall is the loader's
     * own entry into the table: it also says whether the slot has a handler at all, which the table
     * cannot tell from here (the "unimplemented" sentinel is a static inline, one copy per unit). */
    if (!lv2_try_syscall(&ctx)) {
        /* The same two outcomes the loader gives a slot with no handler (ppu_loader.cpp, lv2_syscall):
         * the firmware's own ENOSYS stub answers for the slots the stock kernel leaves on it, any other
         * gap is the named stop. */
        if (lv2_enosys_default_answer(num)) {
            ctx.gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOSYS;
        } else {
            char num_s[16];
            snprintf(num_s, sizeof num_s, "%u", num);
            ps3_stop_field f[] = { { "num", num_s }, { "caller_lr", "0x00000000" } };
            ps3_stop_site site = { 0, 0, MAIN_THREAD_ID };
            ps3_stop(PS3_STOP_UNIMPLEMENTED_SYSCALL, "lv2 syscall has no implementation",
                     "Implement the syscall in runtime/syscalls.", f, 2, &site);
        }
    }
    g_status->sc_num = UINT32_MAX;
    return (int64_t)(int32_t)(uint32_t)ctx.gpr[3];
}

/* ---- guest memory ------------------------------------------------------------------------------ */

conf_addr conf_alloc(uint32_t size, uint32_t align)
{
    if (align == 0)
        align = 1;
    if (align & (align - 1)) {
        fprintf(stderr, "lv2conf: %s: conf_alloc alignment %u is not a power of two\n", g_id, align);
        _exit(3);
    }
    uint64_t a = ((uint64_t)g_scratch_next + align - 1u) & ~((uint64_t)align - 1u);
    if (a + size > (uint64_t)SCRATCH_BASE + SCRATCH_SIZE) {
        fprintf(stderr, "lv2conf: %s: scratch arena exhausted (asked for %u bytes)\n", g_id, size);
        _exit(3);
    }
    memset(vm_base + a, 0, size);
    g_scratch_next = (uint32_t)(a + size);
    return (conf_addr)a;
}

uint8_t conf_r8(conf_addr a)
{
    return vm_base[a];
}

uint32_t conf_r32(conf_addr a)
{
    const uint8_t *p = vm_base + a;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint64_t conf_r64(conf_addr a)
{
    return (uint64_t)conf_r32(a) << 32 | conf_r32(a + 4);
}

void conf_w8(conf_addr a, uint8_t v)
{
    vm_base[a] = v;
}

void conf_w32(conf_addr a, uint32_t v)
{
    uint8_t *p = vm_base + a;
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void conf_w64(conf_addr a, uint64_t v)
{
    conf_w32(a, (uint32_t)(v >> 32));
    conf_w32(a + 4, (uint32_t)v);
}

/* ---- result rows ------------------------------------------------------------------------------- */

/* Flushed per row: a case that hangs or crashes leaves what it recorded before that. */
void conf_rec(const char *key, int64_t value)
{
    fprintf(g_rows, "CASE,%s,%s,%lld\n", g_id, key, (long long)value);
    fflush(g_rows);
}

void conf_rec_rc(const char *key, int64_t rc)
{
    fprintf(g_rows, "CASE,%s,%s,0x%08x\n", g_id, key, (unsigned)(uint32_t)rc);
    fflush(g_rows);
}

void conf_rec_time(const char *key, uint64_t ticks)
{
    fprintf(g_rows, "TIME,%s,%s,%llu\n", g_id, key, (unsigned long long)ticks);
    fflush(g_rows);
}

/* ---- timebase ---------------------------------------------------------------------------------- */

uint64_t conf_tbfreq(void)
{
    return LV2CONF_HOST_TBFREQ;
}

uint64_t conf_tb(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* nsec < 1e9 and the rate is below 1e8, so the product fits 64 bits. */
    return (uint64_t)ts.tv_sec * LV2CONF_HOST_TBFREQ + (uint64_t)ts.tv_nsec * LV2CONF_HOST_TBFREQ / 1000000000ull;
}
