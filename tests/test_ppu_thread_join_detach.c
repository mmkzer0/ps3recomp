/* PPU thread join / detach / exit lifecycle through the real sys_ppu_thread.c.
 *
 * lv2 (RPCS3 ppu_join_status): join -> EDEADLK on itself, EINVAL on a detached thread or one a join is already
 * waiting for, ESRCH on an id that no longer names a thread, a zombie is reaped at once; detach -> EINVAL on a
 * detached thread, EBUSY while a join waits, a zombie is reaped at once. Slot 41 is _sys_ppu_thread_exit and slot
 * 42 is a stock ENOSYS slot, dispatched through the table sys_ppu_thread_init builds.
 *
 * The races are ordered with ppu_thread_set_epilogue_hook: the hook runs on the worker after it has published its
 * end and released the table lock, and may park it there, so the test thread can join or detach in exactly the window
 * that used to be a use-after-destroy. Where a test must know a worker is completely gone it waits for a pthread key
 * destructor, which runs after the thread procedure has returned. Nothing here sleeps to order two threads.
 */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include "../runtime/syscalls/lv2_enosys_bitmap.h"
#include <assert.h>
#include <sys/mman.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

void ppu_ctx_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
/* Only sys_ppu_thread_yield's opt-in diagnostics read it; no case here enables one. */
uint32_t vm_read32(uint64_t a) { uint32_t v; memcpy(&v, vm_base + (uint32_t)a, 4); return v; }

#define TEST_INITIAL_VSCR 0x00010000u
uint32_t ps3_ppu_initial_vscr(void) { return TEST_INITIAL_VSCR; }
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields,
                        const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)fields; (void)nfields; (void)site;
    fprintf(stderr, "ps3_stop called in test: kind=%d\n", (int)kind);
    abort();
}

static int g_fails;
#define EXPECT_EQ(got, want) do { \
    long long g_ = (long long)(got), w_ = (long long)(want); \
    if (g_ != w_) { \
        g_fails++; \
        fprintf(stderr, "FAIL line %d: %s = %lld (0x%llX), want %lld (0x%llX)\n", __LINE__, #got, g_, \
                (unsigned long long)g_, w_, (unsigned long long)w_); \
    } \
} while (0)

#define E_OK       0
#define E(code)    ((long long)(int32_t)(code))

/* ---------------------------------------------------------------------------
 * Latches and the epilogue hook
 * -----------------------------------------------------------------------*/
typedef struct { pthread_mutex_t m; pthread_cond_t c; int open; } latch;

static void latch_init(latch* l) { pthread_mutex_init(&l->m, NULL); pthread_cond_init(&l->c, NULL); l->open = 0; }
static void latch_wait(latch* l)
{
    pthread_mutex_lock(&l->m);
    while (!l->open) pthread_cond_wait(&l->c, &l->m);
    pthread_mutex_unlock(&l->m);
}
static void latch_open(latch* l)
{
    pthread_mutex_lock(&l->m);
    l->open = 1;
    pthread_cond_broadcast(&l->c);
    pthread_mutex_unlock(&l->m);
}

/* The guest entry of thread `arg` waits at g_gate[arg] if the test closed it. Its exit status is STATUS_BASE + arg. */
#define NGATE        16
#define STATUS_BASE  0xC0DE0000ull
#define ARG_SELFJOIN 7
#define ARG_EXIT     9
#define ARG_SC41      12
#define ARG_SC42      13
#define ARG_SPURIOUS  14
#define EXIT_CODE_41  0x4C1ull
#define JOINABLE      1u
static latch g_gate[NGATE];
static int   g_gate_used[NGATE];          /* written before the thread is created */
static int   g_self_join_rc;              /* the worker's, read after the join that orders it */
static lv2_syscall_table tbl;
static int      g_returned41;             /* set only if sc 41 came back, which it never does */
static uint64_t g_rc42;                   /* r3 after sc 42 */
static int      g_returned42;
static int      g_would_stop;             /* the loader's last arm: no handler and no ENOSYS slot */

static void gate_close(int arg) { latch_init(&g_gate[arg]); g_gate_used[arg] = 1; }
static void gate_open(int arg)  { latch_open(&g_gate[arg]); }

/* ppu_loader.cpp's default path for a number its switch does not answer: a
 * registered handler runs, else a stock-ENOSYS slot answers CELL_ENOSYS, else
 * the process stops. The last arm is recorded instead of taken. */
static void guest_syscall(ppu_context* ctx)
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
    g_would_stop = 1;
}

static void guest_entry(ppu_context* ctx)
{
    const uint64_t arg = ctx->gpr[3];
    if (arg < NGATE && g_gate_used[arg]) latch_wait(&g_gate[arg]);
    if (arg == ARG_SELFJOIN) {
        ppu_context c;
        memset(&c, 0, sizeof c);
        c.thread_id = ctx->thread_id;
        c.gpr[3] = ctx->thread_id;
        g_self_join_rc = (int)sys_ppu_thread_join(&c);
    }
    if (arg == ARG_EXIT) {
        ctx->gpr[3] = 0x77;
        sys_ppu_thread_exit(ctx);   /* unwinds to the epilogue, which reads r3 */
        ctx->gpr[3] = 0xBAD;        /* reached only if the unwind failed */
        return;
    }
    if (arg == ARG_SC41) {
        /* _sys_ppu_thread_exit(status): r11 = 41, r3 = the status. r4..r9 are whatever the guest left there; on
         * the old table they were a valid sys_ppu_thread_create and started a foreign thread. */
        ctx->gpr[11] = 41;
        ctx->gpr[3]  = EXIT_CODE_41;
        ctx->gpr[4]  = 0x10000;
        ctx->gpr[5]  = ARG_SPURIOUS;
        ctx->gpr[6]  = 1000;
        ctx->gpr[7]  = VM_PPU_STACK_SIZE;
        ctx->gpr[8]  = JOINABLE;
        ctx->gpr[9]  = 0;
        guest_syscall(ctx);
        g_returned41 = 1;
        ctx->gpr[3] = 0xBAD41;
        return;
    }
    if (arg == ARG_SPURIOUS) {
        ctx->gpr[3] = 0;
        return;
    }
    if (arg == ARG_SC42) {
        /* Stock 42 is the shared ENOSYS stub: the call answers CELL_ENOSYS and the thread runs on. */
        ctx->gpr[11] = 42;
        ctx->gpr[3]  = 0x77;
        guest_syscall(ctx);
        g_rc42 = ctx->gpr[3];
        g_returned42 = 1;
        ctx->gpr[3] = 0x42;
        return;
    }
    ctx->gpr[3] = STATUS_BASE + arg;
}

typedef struct { uint64_t tid; int joiner; } hook_rec;
#define HOOK_MAX 256
static struct {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             park;              /* workers park in the hook while set */
    hook_rec        published[HOOK_MAX];
    int             npublished;
    uint64_t        exited[HOOK_MAX];
    int             nexited;
} hk = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, {{0, 0}}, 0, {0}, 0 };
static pthread_key_t g_exit_key;

/* Runs when the worker's thread procedure has returned: nothing of the worker is left. */
static void exit_dtor(void* p)
{
    pthread_mutex_lock(&hk.m);
    assert(hk.nexited < HOOK_MAX);
    hk.exited[hk.nexited++] = *(uint64_t*)p;
    pthread_cond_broadcast(&hk.c);
    pthread_mutex_unlock(&hk.m);
    free(p);
}

static void epilogue_hook(uint64_t tid, int joiner)
{
    uint64_t* cell = (uint64_t*)malloc(sizeof *cell);
    *cell = tid;
    pthread_setspecific(g_exit_key, cell);
    pthread_mutex_lock(&hk.m);
    assert(hk.npublished < HOOK_MAX);
    hk.published[hk.npublished].tid = tid;
    hk.published[hk.npublished].joiner = joiner;
    hk.npublished++;
    pthread_cond_broadcast(&hk.c);
    while (hk.park) pthread_cond_wait(&hk.c, &hk.m);
    pthread_mutex_unlock(&hk.m);
}

static void hook_park(int on)
{
    pthread_mutex_lock(&hk.m);
    hk.park = on;
    pthread_cond_broadcast(&hk.c);
    pthread_mutex_unlock(&hk.m);
}

/* Blocks until the epilogue of `tid` is past the table lock; returns the joiner state it published. */
static int hook_wait_published(uint64_t tid)
{
    pthread_mutex_lock(&hk.m);
    for (;;) {
        for (int i = 0; i < hk.npublished; i++)
            if (hk.published[i].tid == tid) {
                const int j = hk.published[i].joiner;
                pthread_mutex_unlock(&hk.m);
                return j;
            }
        pthread_cond_wait(&hk.c, &hk.m);
    }
}

static void hook_wait_exited(uint64_t tid)
{
    pthread_mutex_lock(&hk.m);
    for (;;) {
        for (int i = 0; i < hk.nexited; i++)
            if (hk.exited[i] == tid) { pthread_mutex_unlock(&hk.m); return; }
        pthread_cond_wait(&hk.c, &hk.m);
    }
}

/* ---------------------------------------------------------------------------
 * Syscall helpers
 * -----------------------------------------------------------------------*/
#define MAIN_TID 1ull

static int64_t create_thread(uint64_t flags, uint64_t arg, uint64_t* tid_out)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = MAIN_TID;
    ctx.gpr[3] = 0x100;                 /* tid out */
    ctx.gpr[4] = 0x10000;               /* entry */
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;                  /* priority */
    ctx.gpr[7] = VM_PPU_STACK_SIZE;
    ctx.gpr[8] = flags;
    const int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0) {
        uint64_t be;
        memcpy(&be, vm_base + 0x100, 8);
        *tid_out = __builtin_bswap64(be);
    }
    return rc;
}

static int64_t join_as(uint64_t caller, uint64_t tid, uint32_t status_addr)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = caller;
    ctx.gpr[3] = tid;
    ctx.gpr[4] = status_addr;
    return sys_ppu_thread_join(&ctx);
}

static int64_t detach_as(uint64_t caller, uint64_t tid)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = caller;
    ctx.gpr[3] = tid;
    return sys_ppu_thread_detach(&ctx);
}

static uint64_t status_at(uint32_t addr)
{
    uint64_t be;
    memcpy(&be, vm_base + addr, 8);
    return __builtin_bswap64(be);
}

/* What the table says about `tid`: 0 if it names no thread. */
typedef struct { int found; int state; int joiner; uint64_t joiner_tid; int finished; } peek_t;
static peek_t peek(uint64_t tid)
{
    peek_t p = {0, 0, 0, 0, 0};
    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t) {
        p.found = 1;
        p.state = t->state;
        p.joiner = t->joiner;
        p.joiner_tid = t->joiner_tid;
        pthread_mutex_lock(&t->finish_mutex);
        p.finished = t->finished;
        pthread_mutex_unlock(&t->finish_mutex);
    }
    table_unlock();
    return p;
}

typedef struct { uint32_t free_count; uint32_t next_addr; } stacks_t;
static stacks_t stacks(void)
{
    stacks_t s;
    table_lock();
    s.free_count = g_vm_stack_alloc.free_count;
    s.next_addr = g_vm_stack_alloc.next_addr;
    table_unlock();
    return s;
}

/* Slots the table holds: main plus every thread that has not been reaped. */
static int live_threads(void)
{
    int n = 0;
    table_lock();
    for (int i = 0; i < PPU_THREAD_MAX; i++)
        if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}

/* Poll the table, not the clock: returns once a join is registered on `tid`. */
static void wait_joining(uint64_t tid)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        if (peek(tid).joiner == PPU_JOIN_JOINING) return;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec - t0.tv_sec > 20) {
            g_fails++;
            fprintf(stderr, "FAIL: no join registered on tid %llu\n", (unsigned long long)tid);
            abort();
        }
        sched_yield();
    }
}

typedef struct { uint64_t caller, tid; uint32_t status_addr; int64_t rc; } join_job;
static void* join_main(void* p)
{
    join_job* j = (join_job*)p;
    j->rc = join_as(j->caller, j->tid, j->status_addr);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Cases
 * -----------------------------------------------------------------------*/

/* (a) A joinable thread that is a zombie, its worker parked just past the epilogue's table lock: detach reaps it
 * once, the slot and the stack are reusable at once, and the parked worker, when released, touches neither the
 * destroyed sync objects nor the new owner of the slot. */
static void case_detach_while_worker_in_epilogue(void)
{
    uint64_t t1 = 0, t2 = 0;
    hook_park(1);
    EXPECT_EQ(create_thread(JOINABLE, 0, &t1), E_OK);
    EXPECT_EQ(hook_wait_published(t1), PPU_JOIN_JOINABLE);
    peek_t p = peek(t1);
    EXPECT_EQ(p.state, PPU_THREAD_STATE_ZOMBIE);
    EXPECT_EQ(p.joiner, PPU_JOIN_JOINABLE);
    const stacks_t before = stacks();

    EXPECT_EQ(detach_as(MAIN_TID, t1), E_OK);
    EXPECT_EQ(peek(t1).found, 0);
    const stacks_t after = stacks();
    EXPECT_EQ(after.free_count, before.free_count + 1);   /* freed once */

    gate_close(1);
    EXPECT_EQ(create_thread(JOINABLE, 1, &t2), E_OK);
    EXPECT_EQ(ppu_tid_slot(t2), ppu_tid_slot(t1));        /* the slot was reusable at once... */
    EXPECT_EQ(stacks().free_count, before.free_count);    /* ...and so was the stack */

    hook_park(0);                                         /* the old worker leaves */
    hook_wait_exited(t1);
    p = peek(t2);
    EXPECT_EQ(p.found, 1);
    EXPECT_EQ(p.state, PPU_THREAD_STATE_RUNNING);
    EXPECT_EQ(p.joiner, PPU_JOIN_JOINABLE);
    EXPECT_EQ(p.finished, 0);                             /* it did not signal the new thread's joiner */

    gate_open(1);
    EXPECT_EQ(join_as(MAIN_TID, t2, 0x200), E_OK);
    EXPECT_EQ(status_at(0x200), STATUS_BASE + 1);         /* its own status */
    EXPECT_EQ(peek(t2).found, 0);
}

/* (b)+(c) A join waits: detach says EBUSY, another join EINVAL, and the waiting join still returns the status. */
static void case_join_then_detach_and_second_join(void)
{
    uint64_t t = 0;
    gate_close(2);
    EXPECT_EQ(create_thread(JOINABLE, 2, &t), E_OK);

    join_job job = { MAIN_TID, t, 0x208, -1 };
    pthread_t jt;
    assert(pthread_create(&jt, NULL, join_main, &job) == 0);
    wait_joining(t);
    const peek_t p = peek(t);
    EXPECT_EQ(p.joiner, PPU_JOIN_JOINING);
    EXPECT_EQ(p.joiner_tid, MAIN_TID);

    EXPECT_EQ(detach_as(MAIN_TID + 1, t), E(CELL_EBUSY));
    EXPECT_EQ(join_as(MAIN_TID + 1, t, 0), E(CELL_EINVAL));
    EXPECT_EQ(peek(t).joiner, PPU_JOIN_JOINING);          /* both refusals left the join alone */

    gate_open(2);
    pthread_join(jt, NULL);
    EXPECT_EQ(job.rc, E_OK);
    EXPECT_EQ(status_at(0x208), STATUS_BASE + 2);
    EXPECT_EQ(peek(t).found, 0);
    EXPECT_EQ(join_as(MAIN_TID, t, 0), E(CELL_ESRCH));
    EXPECT_EQ(detach_as(MAIN_TID, t), E(CELL_ESRCH));
}

/* (d) A thread joining itself is told so, and stays joinable. */
static void case_self_join(void)
{
    uint64_t t = 0;
    EXPECT_EQ(create_thread(JOINABLE, ARG_SELFJOIN, &t), E_OK);
    EXPECT_EQ(join_as(MAIN_TID, t, 0x210), E_OK);
    EXPECT_EQ(g_self_join_rc, (int)CELL_EDEADLK);
    EXPECT_EQ(status_at(0x210), STATUS_BASE + ARG_SELFJOIN);
}

/* (e) A second detach is EINVAL, and so is a join of a detached thread; a thread that was never joinable is the same
 * from the start. Both free their slot on their own, after which the id is unknown. */
static void case_second_detach(void)
{
    uint64_t t = 0, u = 0;
    gate_close(3);
    EXPECT_EQ(create_thread(JOINABLE, 3, &t), E_OK);
    EXPECT_EQ(detach_as(MAIN_TID, t), E_OK);
    EXPECT_EQ(detach_as(MAIN_TID, t), E(CELL_EINVAL));
    EXPECT_EQ(join_as(MAIN_TID, t, 0), E(CELL_EINVAL));
    EXPECT_EQ(peek(t).joiner, PPU_JOIN_DETACHED);
    gate_open(3);
    EXPECT_EQ(hook_wait_published(t), PPU_JOIN_DETACHED);   /* it freed its own slot */
    EXPECT_EQ(peek(t).found, 0);
    hook_wait_exited(t);
    EXPECT_EQ(detach_as(MAIN_TID, t), E(CELL_ESRCH));
    EXPECT_EQ(join_as(MAIN_TID, t, 0), E(CELL_ESRCH));

    gate_close(4);
    EXPECT_EQ(create_thread(0, 4, &u), E_OK);
    EXPECT_EQ(detach_as(MAIN_TID, u), E(CELL_EINVAL));
    EXPECT_EQ(join_as(MAIN_TID, u, 0), E(CELL_EINVAL));
    gate_open(4);
    EXPECT_EQ(hook_wait_published(u), PPU_JOIN_DETACHED);
    hook_wait_exited(u);
    EXPECT_EQ(peek(u).found, 0);
}

/* (f) detach of a zombie reaps it: the slot and the stack come back. */
static void case_detach_zombie(void)
{
    uint64_t t = 0, t2 = 0;
    EXPECT_EQ(create_thread(JOINABLE, 5, &t), E_OK);
    EXPECT_EQ(hook_wait_published(t), PPU_JOIN_JOINABLE);
    EXPECT_EQ(peek(t).state, PPU_THREAD_STATE_ZOMBIE);
    const stacks_t before = stacks();

    EXPECT_EQ(detach_as(MAIN_TID, t), E_OK);
    EXPECT_EQ(peek(t).found, 0);
    EXPECT_EQ(stacks().free_count, before.free_count + 1);
    hook_wait_exited(t);

    gate_close(6);
    EXPECT_EQ(create_thread(JOINABLE, 6, &t2), E_OK);
    EXPECT_EQ(ppu_tid_slot(t2), ppu_tid_slot(t));
    EXPECT_EQ(stacks().next_addr, before.next_addr);        /* the stack came from the free list */
    EXPECT_EQ(stacks().free_count, before.free_count);
    gate_open(6);
    EXPECT_EQ(join_as(MAIN_TID, t2, 0x218), E_OK);
    EXPECT_EQ(status_at(0x218), STATUS_BASE + 6);
}

/* (g) join of a zombie returns at once with the status, and the id is unknown afterwards. */
static void case_join_zombie(void)
{
    uint64_t t = 0;
    EXPECT_EQ(create_thread(JOINABLE, 8, &t), E_OK);
    EXPECT_EQ(hook_wait_published(t), PPU_JOIN_JOINABLE);
    EXPECT_EQ(peek(t).state, PPU_THREAD_STATE_ZOMBIE);
    const stacks_t before = stacks();
    EXPECT_EQ(join_as(MAIN_TID, t, 0x220), E_OK);
    EXPECT_EQ(status_at(0x220), STATUS_BASE + 8);
    EXPECT_EQ(peek(t).found, 0);
    EXPECT_EQ(stacks().free_count, before.free_count + 1);
    EXPECT_EQ(join_as(MAIN_TID, t, 0), E(CELL_ESRCH));
}

/* (h) The status given to sys_ppu_thread_exit is the join's, and the join waits for the epilogue, not for the exit
 * call: the thread is a zombie only after it has unwound. */
static void case_exit_status(void)
{
    uint64_t t = 0;
    gate_close(ARG_EXIT);
    EXPECT_EQ(create_thread(JOINABLE, ARG_EXIT, &t), E_OK);
    join_job job = { MAIN_TID, t, 0x228, -1 };
    pthread_t jt;
    assert(pthread_create(&jt, NULL, join_main, &job) == 0);
    wait_joining(t);
    gate_open(ARG_EXIT);
    pthread_join(jt, NULL);
    EXPECT_EQ(job.rc, E_OK);
    EXPECT_EQ(status_at(0x228), 0x77);
}

/* (i) A detached thread frees its own slot; the worker, parked right before its return, must not disturb the thread
 * that has taken the slot and the stack over. */
static void case_detached_worker_parked_after_free(void)
{
    uint64_t t1 = 0, t2 = 0;
    hook_park(1);
    EXPECT_EQ(create_thread(0, 10, &t1), E_OK);
    EXPECT_EQ(hook_wait_published(t1), PPU_JOIN_DETACHED);
    EXPECT_EQ(peek(t1).found, 0);                         /* freed before the worker's last statement */
    const stacks_t before = stacks();

    gate_close(11);
    EXPECT_EQ(create_thread(JOINABLE, 11, &t2), E_OK);
    EXPECT_EQ(ppu_tid_slot(t2), ppu_tid_slot(t1));
    EXPECT_EQ(stacks().free_count, before.free_count - 1);   /* the new thread took the freed stack */

    hook_park(0);
    hook_wait_exited(t1);
    peek_t p = peek(t2);
    EXPECT_EQ(p.found, 1);
    EXPECT_EQ(p.state, PPU_THREAD_STATE_RUNNING);
    EXPECT_EQ(p.joiner, PPU_JOIN_JOINABLE);
    EXPECT_EQ(p.finished, 0);
    gate_open(11);
    EXPECT_EQ(join_as(MAIN_TID, t2, 0x230), E_OK);
    EXPECT_EQ(status_at(0x230), STATUS_BASE + 11);
}

/* (j) Slot 41 is _sys_ppu_thread_exit: the status in r3 ends the caller even though r4..r9 still hold a valid
 * sys_ppu_thread_create, which must start nothing. Slot 42 is the stock ENOSYS stub: it answers CELL_ENOSYS and
 * the thread runs on. */
static void case_exit_and_enosys_slots(void)
{
    uint64_t t = 0;
    EXPECT_EQ(live_threads(), 1);                         /* main */

    EXPECT_EQ(create_thread(JOINABLE, ARG_SC41, &t), E_OK);
    EXPECT_EQ(join_as(MAIN_TID, t, 0x248), E_OK);
    EXPECT_EQ(status_at(0x248), EXIT_CODE_41);            /* the status in r3, not a CELL_OK from create */
    EXPECT_EQ(g_returned41, 0);                           /* sc 41 does not return to the guest */
    EXPECT_EQ(live_threads(), 1);                         /* ...and the stale registers started no thread */

    EXPECT_EQ(create_thread(JOINABLE, ARG_SC42, &t), E_OK);
    EXPECT_EQ(join_as(MAIN_TID, t, 0x250), E_OK);
    EXPECT_EQ(g_returned42, 1);
    EXPECT_EQ(g_rc42, E(CELL_ENOSYS));                    /* 0xFFFFFFFF80010003 */
    EXPECT_EQ(status_at(0x250), 0x42);                    /* the thread ran on */
    EXPECT_EQ(g_would_stop, 0);
}

int main(void)
{
    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xE0000000u;
    lv2_syscall_table_init(&tbl);
    sys_ppu_thread_init(&tbl);                /* clears the table and initialises the stack allocator */
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();
    assert(pthread_key_create(&g_exit_key, exit_dtor) == 0);
    ppu_thread_set_epilogue_hook(epilogue_hook);

    case_detached_worker_parked_after_free();

    case_join_zombie();
    case_detach_zombie();
    case_detach_while_worker_in_epilogue();
    case_join_then_detach_and_second_join();
    case_self_join();
    case_second_detach();
    case_exit_status();
    case_exit_and_enosys_slots();

    if (g_fails == 0) {
        printf("PASS: PPU thread join/detach follow the lv2 state machine; slot 41 exits and 42 is ENOSYS; the "
               "epilogue never touches a reaped slot\n");
        return 0;
    }
    fprintf(stderr, "FAILED with %d error(s)\n", g_fails);
    return 1;
}
