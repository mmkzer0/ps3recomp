/* lv2 slot 41 is _sys_ppu_thread_exit and slot 42 is the shared ENOSYS stub (FW 4.93 stock syscall table; RPCS3 at
 * pin d00d8023, lv2.cpp, binds _sys_ppu_thread_exit at 41 and uns_func at 42). Thread creation is 52/53 on lv2 with
 * another ABI, so it is not a table entry here: it is reached by NID.
 *
 * Runs the table sys_ppu_thread_init builds through the path a guest `sc` takes in ppu_loader.cpp's default branch:
 * a registered handler runs; else a slot the firmware's own table leaves on the ENOSYS stub (lv2_enosys_default_answer,
 * the generated FW 4.93 bitmap) answers CELL_ENOSYS; else the process stops (unimplemented-syscall). The last arm is
 * recorded here instead of taken. A guest that issues `sc` 41 with the firmware ABI (r3 = the status) must end there,
 * even when r4..r9 still hold what happens to be a valid sys_ppu_thread_create from an earlier call: on the old table
 * slot 41 was create, so that `sc` started a foreign thread from the stale registers and returned CELL_OK to a caller
 * that meant to exit. `sc` 42 must answer CELL_ENOSYS and leave the caller running, and it does so from the bitmap:
 * slot 42 holds no handler in this runtime (the table keeps lv2_syscall_unimplemented there, so a [[lv2.stub]]
 * allow-list entry still wins over the answer). `sc` 53 is a real firmware syscall this runtime does not implement:
 * neither a handler nor an ENOSYS slot, so the loader stops on it. */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include "../runtime/syscalls/lv2_enosys_bitmap.h"
#include <assert.h>
#include <sys/mman.h>
#include <time.h>
int64_t lv2_syscall_unimplemented(ppu_context* ctx) { (void)ctx; return (int64_t)(int32_t)CELL_ENOSYS; }   /* lv2_register.c */

uint8_t* vm_base;
uint32_t ppu_vm_size;

void ppu_ctx_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
/* Only sys_ppu_thread_yield's opt-in diagnostics read it, and sys_ppu_thread_init registers yield; no case here
 * enables one. */
uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, vm_base + (uint32_t)a, 4); return v; }

#define TEST_INITIAL_VSCR 0x00010000u
uint32_t ps3_ppu_initial_vscr(void) { return TEST_INITIAL_VSCR; }
#include "ppu_thread_config_stub.h"   /* ppu.host_stack_mb */
/* The stop runtime is not linked: a stop here is a failure of the test. ps3_stop_set_mirror, which
 * ps3_log_configure would call, comes with the log (tests/ps3_log_mirror_stub.c). */
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields,
                        const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)fields; (void)nfields; (void)site;
    fprintf(stderr, "ps3_stop called in test: kind=%d\n", (int)kind);
    abort();
}

#define ARG_SC41      1
#define ARG_SC42      2
#define ARG_SC53      3
#define ARG_SPURIOUS  4
#define EXIT_CODE_41  0x4C1ull
#define JOINABLE      1u

static int      g_returned41;             /* set only if sc 41 came back, which it never does */
static int      g_spurious_ran;           /* a thread started from the stale registers */
static int      g_returned42;
static uint64_t g_rc42;                   /* r3 after sc 42 */
static int      g_returned53;
static uint64_t g_r3_53;                  /* r3 after sc 53: untouched, the loader stops before it answers */
static int      g_would_stop;             /* the loader's last arm: no handler and no ENOSYS slot */
static lv2_syscall_table tbl;

/* ppu_loader.cpp's default path for a number its switch does not answer, up to the stop. */
static void guest_sc(ppu_context* ctx)
{
    const uint32_t num = (uint32_t)ctx->gpr[11];
    if (tbl.handlers[num] != lv2_syscall_unimplemented) {
        ctx->gpr[3] = (uint64_t)tbl.handlers[num](ctx);
        return;
    }
    if (lv2_enosys_default_answer(num)) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)CELL_ENOSYS;
        return;
    }
    __atomic_store_n(&g_would_stop, 1, __ATOMIC_RELEASE);
}

static void guest_entry(ppu_context* ctx)
{
    const uint64_t arg = ctx->gpr[3];
    if (arg == ARG_SC41) {
        /* _sys_ppu_thread_exit(status): r11 = 41, r3 = the status. r4..r9 are valid sys_ppu_thread_create arguments
         * (entry, argument, priority, stack size, flags, name), as if the previous call had been one. */
        ctx->gpr[11] = 41;
        ctx->gpr[3]  = EXIT_CODE_41;
        ctx->gpr[4]  = 0x10000;
        ctx->gpr[5]  = ARG_SPURIOUS;
        ctx->gpr[6]  = 1000;
        ctx->gpr[7]  = VM_PPU_STACK_SIZE;
        ctx->gpr[8]  = JOINABLE;
        ctx->gpr[9]  = 0;
        guest_sc(ctx);
        g_returned41 = 1;
        ctx->gpr[3] = 0xBAD41;
        return;
    }
    if (arg == ARG_SPURIOUS) {
        __atomic_store_n(&g_spurious_ran, 1, __ATOMIC_RELEASE);
        ctx->gpr[3] = 0;
        return;
    }
    if (arg == ARG_SC42) {
        ctx->gpr[11] = 42;
        ctx->gpr[3]  = 0x77;
        guest_sc(ctx);
        g_rc42 = ctx->gpr[3];
        g_returned42 = 1;
        ctx->gpr[3] = 0x42;               /* the thread runs on and ends with this status */
        return;
    }
    if (arg == ARG_SC53) {
        ctx->gpr[11] = 53;                /* a firmware syscall with no handler and no ENOSYS slot here */
        ctx->gpr[3]  = 0x77;
        guest_sc(ctx);
        g_r3_53 = ctx->gpr[3];
        g_returned53 = 1;
        ctx->gpr[3] = 0x53;               /* the test thread goes on so it can be joined */
        return;
    }
    ctx->gpr[3] = 0;
}

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = 0x%llX, want 0x%llX\n", __LINE__, #got, (unsigned long long)g_, \
            (unsigned long long)w_); } } while (0)

#define MAIN_TID 1ull

static int64_t create_thread(uint64_t arg, uint64_t* tid_out)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = MAIN_TID;
    ctx.gpr[3] = 0x100;            /* tid out */
    ctx.gpr[4] = 0x10000;          /* entry */
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;             /* priority */
    ctx.gpr[7] = VM_PPU_STACK_SIZE;
    ctx.gpr[8] = JOINABLE;
    ctx.gpr[9] = 0;                /* name */
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0) { uint64_t be; memcpy(&be, vm_base + 0x100, 8); *tid_out = __builtin_bswap64(be); }
    return rc;
}

static int64_t join_thread(uint64_t tid, uint64_t* status)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = MAIN_TID;
    ctx.gpr[3] = tid;
    ctx.gpr[4] = 0x200;
    int64_t rc = sys_ppu_thread_join(&ctx);
    if (rc == 0) { uint64_t be; memcpy(&be, vm_base + 0x200, 8); *status = __builtin_bswap64(be); }
    return rc;
}

/* Slots other than main that are not FREE: a thread nobody has joined keeps its slot. */
static int used_slots(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}

int main(void)
{
    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xE0000000u;
    lv2_syscall_table_init(&tbl);
    sys_ppu_thread_init(&tbl);                 /* clears the thread table, registers the handlers, sets up the stacks */
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    /* The table itself: exit at 41; no handler at 42, which the bitmap answers; no create at 41 or at lv2's 52/53;
     * and the bitmap says what the firmware's table says: 42 is a stock ENOSYS slot, 53 is not. */
    EXPECT_EQ(tbl.handlers[41] == sys_ppu_thread_exit, 1);
    EXPECT_EQ(tbl.handlers[42] == lv2_syscall_unimplemented, 1);
    EXPECT_EQ(tbl.handlers[52] == lv2_syscall_unimplemented, 1);
    EXPECT_EQ(tbl.handlers[53] == lv2_syscall_unimplemented, 1);
    EXPECT_EQ(lv2_enosys_default_answer(42), 1);
    EXPECT_EQ(lv2_enosys_default_answer(53), 0);

    /* sc 41 with the firmware ABI ends the caller with its status, never returns, and starts no thread. */
    uint64_t t = 0, status = 0;
    EXPECT_EQ(create_thread(ARG_SC41, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(status, EXIT_CODE_41);           /* the status in r3, not a CELL_OK from a create */
    EXPECT_EQ(g_returned41, 0);
    for (int i = 0; i < 20; i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    EXPECT_EQ(__atomic_load_n(&g_spurious_ran, __ATOMIC_ACQUIRE), 0);
    EXPECT_EQ(used_slots(), 0);                /* the joined thread is gone and no other took a slot */

    /* sc 42 is the ENOSYS stub: the loader's default path answers CELL_ENOSYS and the thread runs on. */
    EXPECT_EQ(create_thread(ARG_SC42, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(g_returned42, 1);
    EXPECT_EQ(g_rc42, (uint64_t)(int64_t)(int32_t)CELL_ENOSYS);   /* 0xFFFFFFFF80010003, not a CELL_OK */
    EXPECT_EQ(status, 0x42);
    EXPECT_EQ(__atomic_load_n(&g_would_stop, __ATOMIC_ACQUIRE), 0);

    /* sc 53 has no handler and is no ENOSYS slot: the loader's last arm is the unimplemented-syscall stop, and
     * nothing answers CELL_OK for it. */
    EXPECT_EQ(create_thread(ARG_SC53, &t), 0);
    EXPECT_EQ(join_thread(t, &status), 0);
    EXPECT_EQ(g_returned53, 1);
    EXPECT_EQ(__atomic_load_n(&g_would_stop, __ATOMIC_ACQUIRE), 1);
    EXPECT_EQ(g_r3_53, 0x77);                  /* no answer was written */
    EXPECT_EQ(status, 0x53);

    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: sc 41 is _sys_ppu_thread_exit and ends the caller; sc 42 answers CELL_ENOSYS from the FW 4.93 bitmap; "
         "sc 53 reaches the unimplemented-syscall stop");
    return 0;
}
