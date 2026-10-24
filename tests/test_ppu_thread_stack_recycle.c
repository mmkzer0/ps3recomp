/* PPU thread stack recycling (M2 R3 / Issue #18):
 * 1. Churn of 300 creates/joins with 1 MB stacks: no ENOMEM, and the next_addr
 *    high-water mark stays bounded (at most concurrent maximum * stack size).
 * 2. Reused stack still has its guard page intact (base - page is PROT_NONE).
 * 3. Detached/non-joinable threads return their stack to the allocator upon exit.
 */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

void ppu_ctx_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

#define TEST_INITIAL_VSCR 0x00010000u
uint32_t ps3_ppu_initial_vscr(void) { return TEST_INITIAL_VSCR; }
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields,
                        const ps3_stop_site* site)
{
    (void)kind; (void)what; (void)remedy; (void)fields; (void)nfields; (void)site;
    fprintf(stderr, "ps3_stop called in test: kind=%d\n", (int)kind);
    abort();
}

static int g_fails;
#define EXPECT_EQ(got, want) do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) { \
        g_fails++; \
        fprintf(stderr, "FAIL line %d: %s = %lld, want %lld\n", __LINE__, #got, g_, w_); \
    } \
} while (0)

static void guest_entry(ppu_context* ctx)
{
    ctx->gpr[3] = 0;
}

static int64_t create_thread(uint64_t flags, uint32_t stack_size, uint64_t* tid_out)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 0x100;                 /* tid out */
    ctx.gpr[4] = 0x10000;               /* entry */
    ctx.gpr[5] = 0;                     /* arg */
    ctx.gpr[6] = 1000;                  /* priority */
    ctx.gpr[7] = stack_size;
    ctx.gpr[8] = flags;
    ctx.gpr[9] = 0;                     /* name */
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid_out) {
        uint64_t be;
        memcpy(&be, vm_base + 0x100, 8);
        *tid_out = __builtin_bswap64(be);
    }
    return rc;
}

static int64_t join_thread(uint64_t tid)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = tid;
    ctx.gpr[4] = 0;                     /* status out */
    return sys_ppu_thread_join(&ctx);
}

static void settle(void)
{
    for (int i = 0; i < 5000; i++) {
        int running = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; k++) {
            if (g_ppu_threads[k].state == PPU_THREAD_STATE_RUNNING ||
                g_ppu_threads[k].state == PPU_THREAD_STATE_DETACHED)
                running++;
        }
        table_unlock();
        if (!running)
            return;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }
}

static int check_guard_fault(uint32_t usable_base)
{
    const uint32_t pg = vm_host_page_size();
    int fds[2];
    if (pipe(fds) != 0)
        return 0;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        /* Verify usable stack is writeable */
        volatile uint8_t* stack = (volatile uint8_t*)vm_to_host(usable_base);
        *stack = 0x5A;
        /* Touch the guard page: must fault! */
        volatile uint8_t* guard = (volatile uint8_t*)vm_to_host(usable_base - pg);
        *guard = 0xAA;
        uint8_t ok = 1;
        write(fds[1], &ok, 1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    uint8_t ok = 0;
    ssize_t n = read(fds[0], &ok, 1);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFSIGNALED(status) && n == 0);
}

int main(void)
{
    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xE0000000u;
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    const uint32_t pg = vm_host_page_size();

    /* -----------------------------------------------------------------------
     * 1. Reused stack is handed out zeroed and retains its guard page intact.
     *    Allocator metadata is purely host-side (never stored in guest memory).
     *    Tested in single-threaded state before thread spawning.
     * -----------------------------------------------------------------------*/
    {
        vm_stack_alloc sa;
        vm_stack_alloc_init(&sa);
        uint32_t base1 = 0;
        int32_t rc1 = vm_stack_allocate(&sa, VM_PPU_STACK_SIZE, &base1);
        EXPECT_EQ(rc1, CELL_OK);
        EXPECT_EQ(check_guard_fault(base1), 1);

        /* Fill usable range with non-zero pattern */
        memset(vm_to_host(base1), 0xAA, VM_PPU_STACK_SIZE);
        EXPECT_EQ(((uint8_t*)vm_to_host(base1))[0], 0xAA);
        EXPECT_EQ(((uint8_t*)vm_to_host(base1))[VM_PPU_STACK_SIZE - 1], 0xAA);

        /* Free the stack: metadata must stay host-side; guest memory unchanged */
        vm_stack_free(&sa, base1, VM_PPU_STACK_SIZE);
        EXPECT_EQ(sa.free_count, 1);
        EXPECT_EQ(sa.free_entries[0].base, base1);
        EXPECT_EQ(((uint8_t*)vm_to_host(base1))[0], 0xAA);

        /* Reallocate: must return the exact same stack base, completely zeroed */
        uint32_t base2 = 0;
        int32_t rc2 = vm_stack_allocate(&sa, VM_PPU_STACK_SIZE, &base2);
        EXPECT_EQ(rc2, CELL_OK);
        EXPECT_EQ(base2, base1);
        EXPECT_EQ(sa.free_count, 0);

        /* Verify entire usable range is zeroed */
        int non_zero = 0;
        const uint8_t* bytes = (const uint8_t*)vm_to_host(base2);
        for (uint32_t i = 0; i < VM_PPU_STACK_SIZE; i++) {
            if (bytes[i] != 0) {
                non_zero++;
                break;
            }
        }
        EXPECT_EQ(non_zero, 0);

        /* Verify guard page at base2 - pg remains PROT_NONE across reuse */
        EXPECT_EQ(check_guard_fault(base2), 1);
    }

    /* -----------------------------------------------------------------------
     * 2. Churn of 300 creates/joins with 1 MB stacks:
     *    - No ENOMEM (stack region would be spent after ~220 without recycling)
     *    - next_addr high-water mark stays bounded at concurrent_max * stack_size
     * -----------------------------------------------------------------------*/
    {
        const uint32_t init_next = g_vm_stack_alloc.next_addr;
        for (int i = 0; i < 300; i++) {
            uint64_t tid = 0;
            int64_t rc = create_thread(1, VM_PPU_STACK_SIZE, &tid);
            if (rc != 0) {
                EXPECT_EQ(rc, 0);
                fprintf(stderr, "Create failed at iteration %d\n", i);
                break;
            }
            rc = join_thread(tid);
            EXPECT_EQ(rc, 0);
        }

        /* Sequential create/join: concurrent maximum is 1 */
        const uint32_t max_allowed_next = init_next + VM_PPU_STACK_SIZE + pg;
        EXPECT_EQ(g_vm_stack_alloc.next_addr <= max_allowed_next, 1);
    }

    /* -----------------------------------------------------------------------
     * 3. Detached / non-joinable threads return their stack upon exit.
     * -----------------------------------------------------------------------*/
    {
        /* 3A: Thread created with flags = 0 (non-joinable) */
        const uint32_t next_before_nj = g_vm_stack_alloc.next_addr;
        uint64_t tid_nj = 0;
        EXPECT_EQ(create_thread(0, VM_PPU_STACK_SIZE, &tid_nj), 0);
        settle();

        /* A new thread of the same size must reuse the stack freed by the non-joinable thread */
        uint64_t tid_reuse1 = 0;
        EXPECT_EQ(create_thread(1, VM_PPU_STACK_SIZE, &tid_reuse1), 0);
        EXPECT_EQ(g_vm_stack_alloc.next_addr, next_before_nj);
        EXPECT_EQ(join_thread(tid_reuse1), 0);

        /* 3B: Thread created joinable, then detached via sys_ppu_thread_detach */
        const uint32_t next_before_det = g_vm_stack_alloc.next_addr;
        uint64_t tid_det = 0;
        EXPECT_EQ(create_thread(1, VM_PPU_STACK_SIZE, &tid_det), 0);

        ppu_context det_ctx;
        memset(&det_ctx, 0, sizeof(det_ctx));
        det_ctx.gpr[3] = tid_det;
        EXPECT_EQ(sys_ppu_thread_detach(&det_ctx), CELL_OK);
        settle();

        /* A new thread of the same size must reuse the stack freed by the detached thread */
        uint64_t tid_reuse2 = 0;
        EXPECT_EQ(create_thread(1, VM_PPU_STACK_SIZE, &tid_reuse2), 0);
        EXPECT_EQ(g_vm_stack_alloc.next_addr, next_before_det);
        EXPECT_EQ(join_thread(tid_reuse2), 0);
    }

    /* -----------------------------------------------------------------------
     * 4. Concurrent threads reuse freed stacks without advancing next_addr.
     * -----------------------------------------------------------------------*/
    {
#define CONCURRENT_TEST 8
        uint64_t tids[CONCURRENT_TEST];

        for (int i = 0; i < CONCURRENT_TEST; i++)
            EXPECT_EQ(create_thread(1, VM_PPU_STACK_SIZE, &tids[i]), 0);
        const uint32_t high_water = g_vm_stack_alloc.next_addr;
        for (int i = 0; i < CONCURRENT_TEST; i++)
            EXPECT_EQ(join_thread(tids[i]), 0);

        /* Spawning CONCURRENT threads again must fit within the high-water mark */
        for (int i = 0; i < CONCURRENT_TEST; i++)
            EXPECT_EQ(create_thread(1, VM_PPU_STACK_SIZE, &tids[i]), 0);
        EXPECT_EQ(g_vm_stack_alloc.next_addr, high_water);
        for (int i = 0; i < CONCURRENT_TEST; i++)
            EXPECT_EQ(join_thread(tids[i]), 0);
    }

    if (g_fails == 0) {
        printf("PASS: PPU thread stacks are recycled on join, detach, and non-joinable exit; guards preserved\n");
        return 0;
    }
    fprintf(stderr, "FAILED with %d error(s)\n", g_fails);
    return 1;
}
