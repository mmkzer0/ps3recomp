/* PPU thread ids (runtime/syscalls/sys_ppu_thread.c). lv2 never reuses an id, and RPCS3 hands out increasing ones: an
 * id a guest keeps (sys_ppu_thread_get_id, a join) must never name a later thread, whichever table slot backs it.
 *
 *   - create, join, create again: a new id each time, ids only go up;
 *   - an id whose thread has been joined (or ended, non-joinable) is unknown: join / detach / priority / rename say
 *     ESRCH;
 *   - a live thread's id resolves to its own slot in every id-keyed lookup;
 *   - concurrent creates (several host threads) get distinct ids;
 *   - ids stay below the host-callback range (0x7F000000) and a spent range is refused, never wrapped.
 *
 * An id carries its slot (ppu_thread_id.h), so every lookup is one array access plus a check that the slot still
 * holds the id.
 *
 * Includes the real implementation and runs real host threads through it, like test_ppu_thread_slots.c.
 * Built and run by tools/test_ppu_thread_ids.py (PS3RECOMP_SANITIZE_THREAD=1 adds -fsanitize=thread). */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;   /* main(): the arena covers the stack region the allocator hands out */
void ppu_ctx_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
uint32_t ps3_ppu_initial_vscr(void) { return 0; }

static uint64_t g_hold_id;             /* the thread with this id waits in its entry until it is cleared */
static void guest_entry(ppu_context* ctx)
{
    while (__atomic_load_n(&g_hold_id, __ATOMIC_ACQUIRE) == ctx->thread_id) { struct timespec ts = { 0, 200000 }; nanosleep(&ts, NULL); }
    ctx->gpr[3] = (uint64_t)ctx->thread_id;     /* the exit status: its own id */
}

static int g_fails;
#define EXPECT_EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); if (g_ != w_) { g_fails++; \
    fprintf(stderr, "FAIL line %d: %s = %lld, want %lld\n", __LINE__, #got, g_, w_); } } while (0)
#define ESRCH_RC ((int64_t)(int32_t)CELL_ESRCH)

static int64_t create_at(uint64_t flags, uint64_t* tid, uint32_t out_ea)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = out_ea;  ctx.gpr[4] = 0x10000;  ctx.gpr[5] = 0;  ctx.gpr[6] = 1001;  ctx.gpr[7] = 0x10000;
    ctx.gpr[8] = flags;   ctx.gpr[9] = 0;
    int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid) { uint64_t be; memcpy(&be, vm_base + out_ea, 8); *tid = __builtin_bswap64(be); }
    return rc;
}
static int64_t create(uint64_t flags, uint64_t* tid) { return create_at(flags, tid, 0x100); }
static int64_t call1(int64_t (*fn)(ppu_context*), uint64_t a, uint64_t b)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = a; ctx.gpr[4] = b;
    return fn(&ctx);
}
static int64_t join(uint64_t id) { return call1(sys_ppu_thread_join, id, 0); }
static int slots_in_use(void)
{
    int n = 0;
    table_lock();
    for (int i = 1; i < PPU_THREAD_MAX; i++) if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE) n++;
    table_unlock();
    return n;
}
static void wait_no_slots(void)
{
    for (int i = 0; i < 5000 && slots_in_use(); i++) { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
}

static void test_create_join_create(void)
{
    uint64_t prev = 1, ids[300];
    for (int i = 0; i < 300; i++) {
        EXPECT_EQ(create(1, &ids[i]), 0);
        EXPECT_EQ(ids[i] > prev, 1);
        for (int j = 0; j < i; j++) if (ids[j] == ids[i]) { g_fails++; fprintf(stderr, "FAIL: id %llu handed out twice\n", (unsigned long long)ids[i]); }
        prev = ids[i];
        EXPECT_EQ(join(ids[i]), 0);
        /* The slot is free again and will back the next thread; the id is gone. */
        EXPECT_EQ(join(ids[i]), ESRCH_RC);
        EXPECT_EQ(call1(sys_ppu_thread_detach, ids[i], 0), ESRCH_RC);
        EXPECT_EQ(call1(sys_ppu_thread_get_priority, ids[i], 0x200), ESRCH_RC);
        EXPECT_EQ(call1(sys_ppu_thread_set_priority, ids[i], 1000), ESRCH_RC);
        EXPECT_EQ(call1(sys_ppu_thread_rename, ids[i], 0), ESRCH_RC);
    }
}

static void test_live_thread_resolves_to_its_slot(void)
{
    uint64_t id = 0, other = 0;
    EXPECT_EQ(create(1, &other), 0);
    EXPECT_EQ(join(other), 0);                       /* an id that is stale by the time the live one is looked up */
    EXPECT_EQ(create(1, &id), 0);
    __atomic_store_n(&g_hold_id, id, __ATOMIC_RELEASE);   /* (it may already have ended: the hold is best effort) */
    table_lock();
    ppu_thread_info* t = NULL;
    for (int i = 0; i < PPU_THREAD_MAX; i++)
        if (g_ppu_threads[i].state != PPU_THREAD_STATE_FREE && g_ppu_threads[i].ctx.thread_id == id) t = &g_ppu_threads[i];
    table_unlock();
    EXPECT_EQ(t != NULL, 1);
    if (t) {
        EXPECT_EQ(ppu_tid_diag_index(id), (t - g_ppu_threads) + 1);   /* the slot is in the id */
        EXPECT_EQ(call1(sys_ppu_thread_set_priority, id, 777), 0);
        EXPECT_EQ(t->priority, 777);
        EXPECT_EQ(call1(sys_ppu_thread_get_priority, id, 0x200), 0);
        uint32_t be; memcpy(&be, vm_base + 0x200, 4);
        EXPECT_EQ((int32_t)__builtin_bswap32(be), 777);
        EXPECT_EQ(call1(sys_ppu_thread_rename, id, 0), 0);
    }
    __atomic_store_n(&g_hold_id, 0, __ATOMIC_RELEASE);
    EXPECT_EQ(call1(sys_ppu_thread_join, id, 0x300), 0);
    uint64_t be; memcpy(&be, vm_base + 0x300, 8);
    EXPECT_EQ((int64_t)__builtin_bswap64(be), (int64_t)id);   /* the exit status reached the right thread */
    EXPECT_EQ(join(id), ESRCH_RC);
}

static void test_non_joinable_id_is_gone_after_it_ends(void)
{
    uint64_t first = 0, id = 0;
    EXPECT_EQ(create(0, &first), 0);
    wait_no_slots();
    EXPECT_EQ(join(first), ESRCH_RC);                /* it freed its slot when it ended */
    EXPECT_EQ(call1(sys_ppu_thread_get_priority, first, 0x200), ESRCH_RC);
    for (int i = 0; i < 2 * PPU_THREAD_MAX; i++) {   /* the slot is used again and again, never under the old id */
        EXPECT_EQ(create(0, &id), 0);
        EXPECT_EQ(id != first, 1);
        wait_no_slots();
    }
    EXPECT_EQ(join(first), ESRCH_RC);
}

static void test_host_callback_range_is_not_a_thread(void)
{
    /* A host-callback context (ids from 0x7F000000) is in no table; it still asks for its own priority. */
    memset(vm_base + 0x200, 0, 4);
    EXPECT_EQ(call1(sys_ppu_thread_get_priority, 0x7F000001ull, 0x200), 0);
    uint32_t be; memcpy(&be, vm_base + 0x200, 4);
    EXPECT_EQ((int32_t)__builtin_bswap32(be), 1000);
    EXPECT_EQ(call1(sys_ppu_thread_get_priority, 0, 0x200), ESRCH_RC);
    EXPECT_EQ(join(0x7F000001ull), ESRCH_RC);
}

#define NTHREADS 8
#define PER_THREAD 150
static uint64_t g_conc_ids[NTHREADS][PER_THREAD];
static void* creator(void* arg)
{
    const int k = (int)(intptr_t)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        int64_t rc;
        /* each caller has a result cell of its own (the id is written through it) */
        while ((rc = create_at(0, &g_conc_ids[k][i], 0x400 + 8u * (uint32_t)k)) == (int64_t)(int32_t)CELL_EAGAIN)
            sched_yield();
        if (rc != 0) g_conc_ids[k][i] = 0;
    }
    return NULL;
}
static int cmp_u64(const void* a, const void* b)
{
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}
static void test_concurrent_creates_get_distinct_ids(void)
{
    pthread_t th[NTHREADS];
    memset(g_conc_ids, 0, sizeof g_conc_ids);
    for (int k = 0; k < NTHREADS; k++) pthread_create(&th[k], NULL, creator, (void*)(intptr_t)k);
    for (int k = 0; k < NTHREADS; k++) pthread_join(th[k], NULL);
    static uint64_t all[NTHREADS * PER_THREAD];
    int n = 0;
    for (int k = 0; k < NTHREADS; k++) for (int i = 0; i < PER_THREAD; i++) all[n++] = g_conc_ids[k][i];
    qsort(all, (size_t)n, sizeof all[0], cmp_u64);
    int zero = 0, dup = 0, high = 0;
    for (int i = 0; i < n; i++) {
        if (all[i] < 2) zero++;
        if (all[i] >= PPU_GUEST_TID_END) high++;
        if (i && all[i] == all[i - 1]) dup++;
    }
    EXPECT_EQ(zero, 0);
    EXPECT_EQ(dup, 0);
    EXPECT_EQ(high, 0);
    wait_no_slots();
}

static void test_the_range_is_refused_when_spent_not_wrapped(void)
{
    uint64_t id = 0;
    table_lock(); s_next_serial = PPU_GUEST_TID_END / PPU_THREAD_SLOTS - 1; table_unlock();
    EXPECT_EQ(create(0, &id), 0);
    EXPECT_EQ(id < PPU_GUEST_TID_END && id >= PPU_GUEST_TID_END - PPU_THREAD_SLOTS, 1);
    EXPECT_EQ(create(0, &id), (int64_t)(int32_t)CELL_EAGAIN);
    EXPECT_EQ(create(1, &id), (int64_t)(int32_t)CELL_EAGAIN);
    wait_no_slots();
    EXPECT_EQ(create(0, &id), (int64_t)(int32_t)CELL_EAGAIN);   /* a freed slot does not bring the range back */
}

int main(void)
{
    /* A real guest arena that covers the stack region: the stack allocator sets a guard page per thread, and
     * vm_protect refuses any range outside [0, ppu_vm_size). */
    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xA0000000u;
    g_vm_stack_alloc.next_addr = 0x10000000u;
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();

    test_create_join_create();
    test_live_thread_resolves_to_its_slot();
    test_non_joinable_id_is_gone_after_it_ends();
    test_host_callback_range_is_not_a_thread();
    test_concurrent_creates_get_distinct_ids();
    test_the_range_is_refused_when_spent_not_wrapped();

    if (g_fails) { fprintf(stderr, "%d check(s) failed\n", g_fails); return 1; }
    puts("PASS: PPU thread ids are monotonic and never reused; stale ids are ESRCH");
    return 0;
}
    munmap(vm_base, (size_t)VM_TOTAL_SIZE);
