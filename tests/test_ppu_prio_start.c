/* PS3_PRIO_START: sys_ppu_thread_create lets a thread made at a higher priority than its creator run until it
 * blocks, as lv2's scheduler does. Includes the real implementation and runs real host threads through it; the guest
 * entry is a trampoline whose behaviour the thread argument picks. tools/test_ppu_prio_start.py runs the binary twice,
 * with PS3_PRIO_START=1 and without, and the expectations follow the variable.
 *
 * Nothing here is ordered by a sleep. A created thread starts at a gate, which stays shut until either the creator's
 * wait has begun (ppu_thread_set_prio_start_hook: the hook opens it) or the case is over (finish_case opens it). So
 * "the creator read the word before the thread wrote it" cannot be a matter of how fast a loaded host runs, and
 * whether the creator waited is counted by the hook, not inferred from how long create took. The one elapsed-time
 * check is a lower bound (case 4), which a slow host cannot violate. */
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <time.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;   /* main(): the arena covers the stack region the allocator hands out */
/* The few runtime services the thread proc and create call; nothing here replaces the code under test. */
void ppu_ctx_register(ppu_context* c) { (void)c; }
/* ppu.vscr_nj comes from the runtime's config module and ps3_stop from the stop module; neither is linked here. */
uint32_t ps3_ppu_initial_vscr(void) { return 0; }
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields,
                        const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)fields; (void)nfields; (void)site;
    fprintf(stderr, "ps3_stop called in test: kind=%d\n", (int)kind);
    abort();
}
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }

enum { BODY_WRITE_THEN_BLOCK = 1, BODY_WRITE_THEN_RETURN, BODY_SPIN_UNTIL_RELEASED };

static int g_published;   /* what a thread writes before it blocks (stands in for the event queue key) */
static int g_release;     /* lets a BODY_SPIN_UNTIL_RELEASED thread end */
static int g_waits;       /* hook calls: creates that began a PS3_PRIO_START wait */
static int g_gave_up;     /* hook calls: waits that ran into the cap */

/* The gate a thread passes before it publishes. */
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gate_cv = PTHREAD_COND_INITIALIZER;
static int             g_gate_open;

static void gate_wait(void)
{
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate_open) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
}
static void gate_set(int open)
{
    pthread_mutex_lock(&g_gate_mu);
    g_gate_open = open;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
}

/* Runs on the creator, inside sys_ppu_thread_create, when it waits for the thread it just made. */
static void prio_start_hook(uint64_t tid, int gave_up)
{
    (void)tid;
    if (gave_up) { __atomic_fetch_add(&g_gave_up, 1, __ATOMIC_RELAXED); return; }
    __atomic_fetch_add(&g_waits, 1, __ATOMIC_RELAXED);
    gate_set(1);   /* the creator is now waiting: the thread may run */
}

static void sleep_ms(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static long long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000; }

static void guest_entry(ppu_context* ctx)
{
    switch (ctx->gpr[3]) {
    case BODY_WRITE_THEN_BLOCK:    /* the SurMixer shape: set up, publish, then wait for work */
        gate_wait();
        __atomic_store_n(&g_published, 1, __ATOMIC_RELAXED);
        ppu_thread_note_block(ctx->thread_id);   /* what every wait syscall does on entry */
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sleep_ms(1);
        break;
    case BODY_WRITE_THEN_RETURN:   /* ends without ever reaching a wait */
        gate_wait();
        __atomic_store_n(&g_published, 1, __ATOMIC_RELAXED);
        break;
    case BODY_SPIN_UNTIL_RELEASED: /* never reaches a wait the runtime sees */
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sleep_ms(1);
        break;
    }
    ctx->gpr[3] = 0;
}

static int g_fails;
#define EXPECT(cond) do { if (!(cond)) { g_fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static int published(void) { return __atomic_load_n(&g_published, __ATOMIC_RELAXED); }
static int waits(void)     { return __atomic_load_n(&g_waits, __ATOMIC_RELAXED); }
static int gave_up(void)   { return __atomic_load_n(&g_gave_up, __ATOMIC_RELAXED); }

/* Creates a non-joinable thread from main; returns how long the create took, in ms. */
static long long create(int32_t priority, uint64_t body)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.thread_id = 1;             /* main */
    ctx.gpr[3] = 0x100;            /* tid out */
    ctx.gpr[4] = 0x10000;          /* entry */
    ctx.gpr[5] = body;             /* arg */
    ctx.gpr[6] = (uint64_t)(int64_t)priority;
    ctx.gpr[7] = 0x10000;          /* stack size */
    ctx.gpr[8] = 0;                /* flags: not joinable */
    ctx.gpr[9] = 0;                /* name */
    const long long t0 = now_ms();
    EXPECT(sys_ppu_thread_create(&ctx) == 0);
    return now_ms() - t0;
}

static void set_main_priority(int32_t priority)
{
    ppu_context ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.gpr[3] = 1;
    ctx.gpr[4] = (uint64_t)(int64_t)priority;
    EXPECT(sys_ppu_thread_set_priority(&ctx) == 0);
}

/* Ends every live created thread and resets the shared words for the next case. */
static void finish_case(void)
{
    gate_set(1);
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    int live = 0;
    for (int i = 0; i < 20000; i++) {
        live = 0;
        table_lock();
        for (int k = 1; k < PPU_THREAD_MAX; k++) if (g_ppu_threads[k].state != PPU_THREAD_STATE_FREE) live++;
        table_unlock();
        if (!live) break;
        sleep_ms(1);
    }
    EXPECT(live == 0);   /* a thread still running here would see the next case's words */
    gate_set(0);
    __atomic_store_n(&g_release, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_published, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_waits, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_gave_up, 0, __ATOMIC_RELAXED);
}

int main(void)
{
    const int on = getenv("PS3_PRIO_START") != NULL;
    /* A real guest arena that covers the stack region: the stack allocator sets a guard page per thread, and
     * vm_protect refuses any range outside [0, ppu_vm_size). */
    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xA0000000u;
    g_vm_stack_alloc.next_addr = 0x10000000u;   /* the guest stack region the threads bump-allocate from */
    g_vm_stack_alloc.region_end = 0xA0000000u;
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_set_prio_start_hook(prio_start_hook);
    ppu_thread_register_main();                 /* main runs at the default priority, 1000 */

    /* 1. A higher-priority thread publishes, then blocks: with the option the creator sees what it published. The
     * thread cannot publish before the creator's wait has begun, so without the option it has not published when
     * create returns. */
    create(400, BODY_WRITE_THEN_BLOCK);
    EXPECT(published() == on);
    EXPECT(waits() == on);
    EXPECT(gave_up() == 0);
    finish_case();

    /* 2. Equal and lower priority do not preempt the creator: create returns without waiting, and the gate is shut,
     * so the thread has not published. */
    create(1000, BODY_WRITE_THEN_BLOCK);
    EXPECT(published() == 0);
    EXPECT(waits() == 0);
    finish_case();
    create(2000, BODY_WRITE_THEN_BLOCK);
    EXPECT(published() == 0);
    EXPECT(waits() == 0);
    finish_case();

    /* 3. A higher-priority thread that ends without blocking releases its creator when it ends, not at the cap. */
    create(400, BODY_WRITE_THEN_RETURN);
    EXPECT(published() == on);
    EXPECT(waits() == on);
    EXPECT(gave_up() == 0);
    finish_case();

    /* 4. One that never blocks holds its creator up to the cap and no longer: create returns, and only after the
     * cap. Without the option it does not wait at all. */
    {
        const long long ms = create(400, BODY_SPIN_UNTIL_RELEASED);
        EXPECT(waits() == on);
        EXPECT(gave_up() == on);
        if (on) EXPECT(ms >= PRIO_START_TIMEOUT_MS - 5);
        finish_case();
    }

    /* 5. The creator's own priority counts once main has set one: at 300, a 400 thread no longer preempts it. */
    set_main_priority(300);
    create(400, BODY_WRITE_THEN_BLOCK);
    EXPECT(published() == 0);
    EXPECT(waits() == 0);
    finish_case();
    create(200, BODY_WRITE_THEN_BLOCK);
    EXPECT(published() == on);
    EXPECT(waits() == on);
    EXPECT(gave_up() == 0);
    finish_case();

    if (g_fails) { fprintf(stderr, "%d check(s) failed (PS3_PRIO_START %s)\n", g_fails, on ? "on" : "off"); return 1; }
    printf("PASS: PS3_PRIO_START %s\n", on ? "on: a higher-priority thread runs until it blocks"
                                          : "off: sys_ppu_thread_create never waits");
    return 0;
}
