/* The host stack of a created guest PPU thread (ppu.host_stack_mb), through the real sys_ppu_thread.c:
 *
 *   on   PS3_HOST_STACK_HWM=1: the high-water mark a thread reports at its exit (and, for a thread still running,
 *        in the process-exit summary) is the depth its call chain reached, to within one host page; a thread that
 *        recursed nowhere reports little. The threads recurse over frames whose addresses the test reads, so the
 *        expected figure is computed, not remembered. No sleep: threads park on flags and the test orders itself
 *        by join and by those flags.
 *   off  the variable unset: a thread records nothing and the report hook is never called (the default run pays
 *        nothing).
 *
 * The last step of "on" asks the host for a stack it cannot give (the stub config is not validated, so the figure
 * is as absurd as a caller can make it) and checks that the create ends in the stop host-thread-stack with the
 * thread's id, the bytes asked and the host's errno, instead of starting the thread on the default stack. */
#include <pthread.h>
#include <errno.h>

/* The pthread attribute calls a thread create makes, counted and made to fail on demand. Defined before the macros
 * that redirect them, and pthread.h is already in, so the redirect touches only the code under test. */
static int g_attr_init_fail, g_setstack_fail, g_init_calls, g_setstack_calls, g_destroy_calls;
static int test_attr_init(pthread_attr_t* a)
{
    g_init_calls++;
    return g_attr_init_fail ? g_attr_init_fail : pthread_attr_init(a);
}
static int test_attr_setstacksize(pthread_attr_t* a, size_t n)
{
    g_setstack_calls++;
    return g_setstack_fail ? g_setstack_fail : pthread_attr_setstacksize(a, n);
}
static int test_attr_destroy(pthread_attr_t* a)
{
    g_destroy_calls++;
    return pthread_attr_destroy(a);
}
#define PS3_HOST_THREAD_ATTR_INIT test_attr_init
#define pthread_attr_setstacksize test_attr_setstacksize
#define pthread_attr_destroy test_attr_destroy
static void reuse_refused_slot(void);
#define PS3_PPU_STACK_REFUSAL_HOOK reuse_refused_slot
#include "../runtime/syscalls/sys_ppu_thread.c"
#include <assert.h>
#include <sched.h>
#include <fcntl.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;

void ppu_ctx_register(ppu_context* c) { (void)c; }
int ps3_intr_is_handler(unsigned long long tid) { (void)tid; return 0; }
int ps3_intr_wait(unsigned long long tid) { (void)tid; return 0; }
uint32_t ps3_ppu_initial_vscr(void) { return 0; }
#include "ppu_thread_config_stub.h"   /* ppu.host_stack_mb */

/* The stop, recorded and escaped with a longjmp (sys_ppu_thread_create has released the table lock by then). */
static jmp_buf g_stop_jmp;
static volatile int g_stop_armed;
static struct { int kind; char role[24], tid[24], bytes[24], err[24]; } g_stop;
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields, const ps3_stop_site* site)
{
    (void)what; (void)remedy; (void)site;
    if (!g_stop_armed) {
        fprintf(stderr, "unexpected ps3_stop: %s\n", ps3_stop_kind_id(kind));
        abort();
    }
    g_stop.kind = (int)kind;
    for (size_t i = 0; i < nfields; i++) {
        char* dst = !strcmp(fields[i].key, "role") ? g_stop.role : !strcmp(fields[i].key, "guest_tid") ? g_stop.tid
                  : !strcmp(fields[i].key, "bytes") ? g_stop.bytes : !strcmp(fields[i].key, "errno") ? g_stop.err : NULL;
        if (dst) snprintf(dst, 24, "%s", fields[i].value);
    }
    longjmp(g_stop_jmp, 1);
}

/* The run log's mirror hook: ps3_log_configure installs it. These fixtures link the log for its
 * emitters and never configure a log file. */
void ps3_stop_set_mirror(ps3_stop_quiesce_fn quiesce, ps3_stop_mirror_fn mirror)
{ (void)quiesce; (void)mirror; }
const char* ps3_stop_kind_id(ps3_stop_kind kind)
{
    return kind == PS3_STOP_HOST_THREAD_STACK ? "host-thread-stack" : "other";
}

static int g_fails;
#define CHECK(cond) do { if (!(cond)) { g_fails++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
/* A check whose failure should carry the numbers: a failing CI run has nothing else to go on. */
#define CHECK_V(cond, ...) do { if (!(cond)) { g_fails++; fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #cond); \
                                               fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* A run that measures stacks takes the process out of transparent huge pages before it creates a thread (host_stack.h
 * host_stack_measure_prepare): with THP=always a thread stack outside MAP_STACK is backed by 2 MiB pages and the mark
 * comes out in 2 MiB steps. A run that does not measure leaves the process as it was. A host whose prctl does not
 * answer (qemu-user) skips the probe, visibly. */
static void check_thp_off(int want)
{
#if defined(__linux__) && defined(PR_GET_THP_DISABLE)
    const int now = prctl(PR_GET_THP_DISABLE, 0, 0, 0, 0);
    if (now < 0) { fprintf(stderr, "SKIP thp probe: PR_GET_THP_DISABLE errno=%d\n", errno); return; }
    CHECK_V(now == want, "PR_GET_THP_DISABLE is %d, wanted %d", now, want);
#else
    (void)want;
#endif
}

static size_t page_size(void) { return (size_t)sysconf(_SC_PAGESIZE); }

/* One line on a stack's residency, read independently of host_stack_high_water: the pages mincore holds resident as
 * runs (offset from the stack's low end in KiB, length in KiB), and on Linux the smaps entry of the mapping that
 * holds the stack (Rss, huge pages, flags). A flake that reports a whole stack resident is told apart from a wrong
 * range by this. */
static void stack_snapshot_locked(char* out, size_t cap, uintptr_t lo, size_t size)
{
    const size_t pg = page_size();
    const uintptr_t base = (lo + pg - 1) & ~(uintptr_t)(pg - 1), end = (lo + size) & ~(uintptr_t)(pg - 1);
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "range [%#lx,%#lx) %zu pages;", (unsigned long)base, (unsigned long)end, (size_t)((end - base) / pg));
    /* static, not on the stack being measured; never thread-local, which glibc places in the stack mapping.
     * mincore's vector is char on Darwin and unsigned char elsewhere, as in host_stack.h. */
#if defined(__APPLE__)
    static char vec[1u << 15];
#else
    static unsigned char vec[1u << 15];
#endif
    const size_t pages = (end - base) / pg;
    if (pages == 0 || pages > sizeof vec || mincore((void*)base, pages * pg, vec) != 0) {
        snprintf(out + n, cap - n, " mincore failed errno=%d", errno);
        return;
    }
    size_t resident = 0, runs = 0;
    for (size_t i = 0; i < pages; i++) resident += vec[i] & 1;
    n += (size_t)snprintf(out + n, cap - n, " resident %zu pages; runs(KiB off+len):", resident);
    for (size_t i = 0; i < pages;) {
        if (!(vec[i] & 1)) { i++; continue; }
        size_t j = i;
        while (j < pages && (vec[j] & 1)) j++;
        if (++runs <= 6 && n < cap)
            n += (size_t)snprintf(out + n, cap - n, " %zu+%zu", i * pg / 1024, (j - i) * pg / 1024);
        i = j;
    }
    if (runs > 6 && n < cap) n += (size_t)snprintf(out + n, cap - n, " ...(%zu runs)", runs);
#ifdef __linux__
    /* pagemap tells a page a thread wrote (present, exclusively mapped) from one it only read (the shared zero
     * page: present, not exclusive), which mincore cannot. Readable without privilege: flags only. */
    static uint64_t pm[1u << 15];
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd >= 0 && pread(fd, pm, pages * sizeof pm[0], (off_t)((base / pg) * sizeof pm[0])) == (ssize_t)(pages * sizeof pm[0])) {
        size_t excl = 0, shared = 0, swapped = 0, file = 0;
        for (size_t i = 0; i < pages; i++) {
            if (pm[i] >> 62 & 1) swapped++;
            else if (pm[i] >> 63 & 1) { if (pm[i] >> 61 & 1) file++; else if (pm[i] >> 56 & 1) excl++; else shared++; }
        }
        n += (size_t)snprintf(out + n, cap - n, " pagemap excl=%zu shared/zero=%zu swapped=%zu file=%zu;", excl, shared, swapped, file);
    }
    if (fd >= 0) close(fd);
    FILE* f = fopen("/proc/self/smaps", "r");
    char line[256];
    int in = 0;
    while (f && n < cap && fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) == 3) {
            in = base >= a && base < b;
            if (in) { line[strcspn(line, "\n")] = 0; n += (size_t)snprintf(out + n, cap - n, " | vma %.40s", line); }
        } else if (in && (!strncmp(line, "Rss:", 4) || !strncmp(line, "AnonHugePages:", 14) || !strncmp(line, "THPeligible:", 12) ||
                          !strncmp(line, "VmFlags:", 8))) {
            line[strcspn(line, "\n")] = 0;
            char* v = line;
            while (*v && *v != ':') v++;
            v++;
            while (*v == ' ') v++;
            n += (size_t)snprintf(out + n, cap - n, " %.*s=%s", (int)strcspn(line, ":"), line, v);
        }
    }
    if (f) fclose(f);
#endif
}

/* Several threads snapshot (their own start and recursion, the exit hook), and the vectors above are shared. */
static void stack_snapshot(char* out, size_t cap, uintptr_t lo, size_t size)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    stack_snapshot_locked(out, cap, lo, size);
    pthread_mutex_unlock(&lock);
}

/* ---- what the threads do ------------------------------------------------ */

#define FRAME  32768u          /* each recursion level reserves this much and touches every page of it */
#define NO_RECURSION 0xFFFFu   /* arg low half: run no recursion at all */
#define PARK         0x10000u  /* arg bit 16: park at the bottom until released, for the live report */

static uintptr_t probe_fp(void) __attribute__((noinline));
static uintptr_t probe_fp(void) { return (uintptr_t)__builtin_frame_address(0); }

/* Touch FRAME bytes per level, `depth` levels down, and return the frame pointer of a call made at the bottom:
 * the lowest address the chain has had a live frame at, to within a few bytes. */
static uintptr_t recurse(unsigned depth) __attribute__((noinline));
static uintptr_t recurse(unsigned depth)
{
    volatile char buf[FRAME];
    for (size_t i = 0; i < FRAME; i += 512) buf[i] = (char)depth;
    const uintptr_t fp = depth ? recurse(depth - 1) : probe_fp();
    __asm__ volatile("" ::: "memory");
    return fp + (uintptr_t)(buf[0] * 0);   /* keeps buf live across the call, so it is a real frame */
}

typedef struct {
    uintptr_t fp;         /* the deepest address the chain reached */
    uintptr_t end;        /* the top of the stack, rounded down to a host page, as the report counts from */
    size_t    hw;         /* the high-water mark measured by the thread itself, after the recursion returned */
    int       known;
    int       done;
    char      diag[3][1024];      /* stack_snapshot at the thread's start, after its recursion, and once released */
} thread_result;

static thread_result g_res[4];
static volatile int g_parked, g_release;


static void guest_entry(ppu_context* ctx)
{
    const unsigned arg = (unsigned)ctx->gpr[3];
    thread_result* r = &g_res[(arg >> 20) & 3];
    const unsigned depth = arg & 0xFFFFu;
    uintptr_t lo = 0;
    size_t size = 0;
    r->known = host_stack_range_self(&lo, &size);
    r->end = (lo + size) & ~(uintptr_t)(page_size() - 1);
    if (r->known) stack_snapshot(r->diag[0], sizeof r->diag[0], lo, size);   /* before it recurses */
    r->fp = depth == NO_RECURSION ? 0 : recurse(depth);
    r->known = r->known && host_stack_high_water(lo, size, &r->hw);
    if (r->known) stack_snapshot(r->diag[1], sizeof r->diag[1], lo, size);
    if (arg & PARK) {
        __atomic_store_n(&g_parked, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sched_yield();
        if (r->known) stack_snapshot(r->diag[2], sizeof r->diag[2], lo, size);
    }
    r->done = 1;
    ctx->gpr[3] = 0;
}

/* ---- the report hook ---------------------------------------------------- */

static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t tid; size_t reserved, hw; int known, live; char diag[1536]; } g_ev[16];
static int g_nev;

static void hook(uint64_t tid, size_t reserved, size_t hw, int known, int live)
{
    /* An exit report runs on the reported thread itself: look at its stack again with a second, independent
     * reader, so a mark that disagrees with the thread's own says what the kernel held at that moment. */
    char diag[sizeof g_ev[0].diag] = "";
    if (!live) {
        uintptr_t lo; size_t size;
        if (host_stack_range_self(&lo, &size)) {
            stack_snapshot(diag, sizeof diag, lo, size);
            /* The same measure again, a few times, a yield apart: a residency that was a moment's differs from one
             * that stays. */
            size_t n = strlen(diag);
            n += (size_t)snprintf(diag + n, sizeof diag - n, " | remeasured:");
            for (int i = 0; i < 6 && n < sizeof diag; i++) {
                size_t again = 0;
                const int ok = host_stack_high_water(lo, size, &again);
                n += (size_t)snprintf(diag + n, sizeof diag - n, " %s%zu", ok ? "" : "unknown/", again);
                sched_yield();
            }
        }
    }
    pthread_mutex_lock(&g_ev_mu);
    if (g_nev < 16) {
        g_ev[g_nev].tid = tid; g_ev[g_nev].reserved = reserved; g_ev[g_nev].hw = hw;
        g_ev[g_nev].known = known; g_ev[g_nev].live = live;
        memcpy(g_ev[g_nev].diag, diag, sizeof diag);
        g_nev++;
    }
    pthread_mutex_unlock(&g_ev_mu);
}

static int events_for(uint64_t tid, int live, size_t* reserved, size_t* hw, int* known)
{
    int n = 0;
    pthread_mutex_lock(&g_ev_mu);
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].tid == tid && g_ev[i].live == live) {
            *reserved = g_ev[i].reserved; *hw = g_ev[i].hw; *known = g_ev[i].known; n++;
        }
    pthread_mutex_unlock(&g_ev_mu);
    return n;
}

/* Counters of /proc/vmstat that bear on whether a page can become resident without a thread touching it: transparent
 * huge pages (faults, collapses, splits, the huge zero page) and the zero page. Empty elsewhere than Linux. */
struct vmstat_entry { char name[40]; long long v; };
static struct vmstat_entry g_vm0[48];
static int vmstat_read(struct vmstat_entry* out, int cap)
{
    int n = 0;
#ifdef __linux__
    FILE* f = fopen("/proc/vmstat", "r");
    char line[128];
    while (f && n < cap && fgets(line, sizeof line, f)) {
        char name[40];
        long long v;
        if (sscanf(line, "%39s %lld", name, &v) == 2 && (!strncmp(name, "thp_", 4) || !strncmp(name, "nr_anon_transparent", 19) ||
                                                         !strncmp(name, "zero_page", 9) || !strncmp(name, "nr_zero", 7))) {
            memcpy(out[n].name, name, sizeof name);
            out[n].v = v;
            n++;
        }
    }
    if (f) fclose(f);
#endif
    if (n < cap) out[n].name[0] = 0;
    return n;
}

/* What a failing run knows about each thread's stack: at its start, after its recursion, when it was released, and
 * at its exit report. */
static void dump_diagnostics(void)
{
    static const char* const phase[3] = { "start", "recursed", "released" };
    for (unsigned s = 0; s < 4; s++)
        for (unsigned p = 0; p < 3; p++)
            if (g_res[s].diag[p][0])
                fprintf(stderr, "DIAG slot %u %s: %s\n", s, phase[p], g_res[s].diag[p]);
    pthread_mutex_lock(&g_ev_mu);
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].diag[0])
            fprintf(stderr, "DIAG tid %llu exit report hw %zu: %s\n", (unsigned long long)g_ev[i].tid, g_ev[i].hw, g_ev[i].diag);
    pthread_mutex_unlock(&g_ev_mu);
    /* The kernel's own huge-page and zero-page counters, system-wide, as they moved over this run. */
    static struct vmstat_entry now[48];
    const int n = vmstat_read(now, 48);
    fprintf(stderr, "DIAG vmstat deltas since start:");
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 48 && g_vm0[j].name[0]; j++)
            if (!strcmp(now[i].name, g_vm0[j].name) && now[i].v != g_vm0[j].v)
                fprintf(stderr, " %s%+lld", now[i].name, (long long)(now[i].v - g_vm0[j].v));
    fprintf(stderr, "\n");
}

/* ---- driving sys_ppu_thread_create -------------------------------------- */

static int64_t create_thread(uint64_t arg, uint64_t* tid_out)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x100;                 /* tid out */
    ctx.gpr[4] = 0x10000;               /* entry (the trampoline is the test's) */
    ctx.gpr[5] = arg;
    ctx.gpr[6] = 1000;                  /* priority */
    ctx.gpr[7] = 0x10000;               /* guest stack size */
    ctx.gpr[8] = 1;                     /* joinable */
    ctx.gpr[9] = 0;                     /* name */
    const int64_t rc = sys_ppu_thread_create(&ctx);
    if (rc == 0 && tid_out) {
        uint64_t be;
        memcpy(&be, vm_base + 0x100, 8);
        *tid_out = __builtin_bswap64(be);
    }
    return rc;
}

static void join_thread(uint64_t tid)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = tid;
    CHECK(sys_ppu_thread_join(&ctx) == 0);
}

/* Run one thread to its end and return its tid. */
static uint64_t run_thread(unsigned slot, unsigned depth)
{
    uint64_t tid = 0;
    CHECK(create_thread(((uint64_t)slot << 20) | depth, &tid) == 0);
    join_thread(tid);
    CHECK(g_res[slot].done);
    return tid;
}
/* A create the host refuses: the stop is recorded, with the errno `want_errno` (0: any non-zero), for the id the
 * refused thread would have had (above `after`). */
static int refused_create(const uint64_t* after, int want_errno)
{
    int ok = 0;
    g_stop_armed = 1;
    if (setjmp(g_stop_jmp) == 0) {
        uint64_t tid = 0;
        (void)create_thread(0, &tid);
        CHECK(!"the create returned although the host refused the stack");
    } else {
        ok = g_stop.kind == (int)PS3_STOP_HOST_THREAD_STACK && !strcmp(g_stop.role, "ppu") &&
             (want_errno ? atoi(g_stop.err) == want_errno : atoi(g_stop.err) != 0) &&
             strtoull(g_stop.tid, NULL, 10) > *after;
    }
    g_stop_armed = 0;
    return ok;
}

static int g_reuse_on_refusal;
static void* reuse_worker(void* unused)
{
    (void)unused;
    const uint64_t tid = run_thread(0, 2);
    (void)tid;
    return NULL;
}
static void reuse_refused_slot(void)
{
    if (!g_reuse_on_refusal) return;
    const uint32_t saved_mb = g_stub_config.ppu_host_stack_mb;
    const int saved_fail = g_attr_init_fail;
    g_attr_init_fail = 0;
    g_stub_config.ppu_host_stack_mb = 8;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, reuse_worker, NULL) == 0);
    pthread_join(worker, NULL);
    g_stub_config.ppu_host_stack_mb = saved_mb;
    g_attr_init_fail = saved_fail;
}

int main(int argc, char** argv)
{
    const int on = !(argc > 1 && !strcmp(argv[1], "off"));
    vmstat_read(g_vm0, 48);
    if (on) setenv("PS3_HOST_STACK_HWM", "1", 1);
    else    unsetenv("PS3_HOST_STACK_HWM");

    vm_base = (uint8_t*)mmap(NULL, (size_t)VM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(vm_base != MAP_FAILED);
    ppu_vm_size = 0xE0000000u;
    vm_stack_alloc_init(&g_vm_stack_alloc);
    g_ppu_thread_entry_trampoline = guest_entry;
    ppu_thread_register_main();
    ppu_thread_set_host_stack_hook(hook);
    g_stub_config.ppu_host_stack_mb = 16;
    const size_t reserved = (size_t)16 * 1024 * 1024;
    const size_t pg = page_size();

    if (!on) {
        /* The default run: nothing recorded, nothing reported, and the slot's stack range stays unset. */
        const uint64_t tid = run_thread(0, 4);
        check_thp_off(0);
        size_t r, hw; int known;
        CHECK(g_res[0].known);                                   /* the helpers themselves work */
        CHECK(events_for(tid, 0, &r, &hw, &known) == 0);
        CHECK(g_nev == 0);
        const int slot = ppu_tid_slot(tid);
        CHECK(g_ppu_threads[slot].host_stack_lo == 0 && g_ppu_threads[slot].host_stack_size == 0);
        CHECK(g_ppu_threads[slot].host_stack_req == reserved);   /* the request is always recorded: it is the stop's */
        host_stack_summary();                                    /* no live thread: reports nothing, and is safe */
        CHECK(g_nev == 0);
        return g_fails ? 1 : 0;
    }

    /* 1. A thread that recursed 8 frames down reports that depth to within a page, in-thread and at its exit. */
    const uint64_t tid_a = run_thread(0, 8);
    /* 2. One that recursed 24 down reports the difference. */
    const uint64_t tid_b = run_thread(1, 24);
    /* 3. One that recursed nowhere reports little. */
    const uint64_t tid_c = run_thread(2, NO_RECURSION);

    const uint64_t tids[3] = { tid_a, tid_b, tid_c };
    for (unsigned i = 0; i < 3; i++) {
        const thread_result* r = &g_res[i];
        size_t rsv = 0, hw = 0; int known = 0;
        CHECK(r->known);
        CHECK(events_for(tids[i], 0, &rsv, &hw, &known) == 1);   /* once, at the exit */
        CHECK(known && rsv == reserved);
        CHECK_V(hw == r->hw, "thread %u: exit report %zu, in-thread %zu", i, hw, r->hw);   /* the exit report sees what the thread saw */
        if (i < 2) {
            /* The chain's lowest live address is r->fp; the report counts from the page-aligned top down to the
             * page that address is in, so it is the exact distance and at most one page more. */
            const size_t exact = (size_t)(r->end - r->fp);
            CHECK_V(r->hw >= exact, "thread %u: hw %zu, exact %zu", i, r->hw, exact);
            CHECK_V(r->hw <= exact + pg, "thread %u: hw %zu, exact %zu, page %zu", i, r->hw, exact, pg);
            CHECK_V(r->hw >= (i == 0 ? 8u : 24u) * FRAME, "thread %u: hw %zu", i, r->hw);
        } else {
            /* A thread that did nothing: below the shallowest recursing thread. Not zero: glibc puts the
             * thread's TLS block at the top of its stack, a hundred KB or more, and Darwin puts nothing there. */
            CHECK_V(r->hw < 8u * FRAME, "thread %u: hw %zu", i, r->hw);
        }
    }
    CHECK_V(g_res[1].hw > g_res[0].hw, "hw %zu (24 frames) vs %zu (8 frames)", g_res[1].hw, g_res[0].hw);
    check_thp_off(1);

    /* 4. A thread still running is reported live by the exit summary, and again at its exit. */
    g_nev = 0;
    uint64_t tid_d = 0;
    CHECK(create_thread(((uint64_t)3 << 20) | PARK | 8u, &tid_d) == 0);
    while (!__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) sched_yield();
    host_stack_summary();
    {
        size_t rsv = 0, hw = 0; int known = 0;
        CHECK(events_for(tid_d, 1, &rsv, &hw, &known) == 1);
        CHECK(known && rsv == reserved);
        CHECK_V(hw == g_res[3].hw && hw >= 8u * FRAME, "live report hw %zu, in-thread %zu", hw, g_res[3].hw);
    }
    __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
    join_thread(tid_d);
    {
        size_t rsv = 0, hw = 0; int known = 0;
        CHECK_V(events_for(tid_d, 0, &rsv, &hw, &known) == 1 && known && hw == g_res[3].hw,
                "exit report: known %d, hw %zu, in-thread %zu", known, hw, g_res[3].hw);
    }

    /* 5. A host that refuses the stack: the create stops, naming the thread, the bytes and the errno. */
    g_stub_config.ppu_host_stack_mb = 0x7FFFFFF0u;                /* ~2 PiB */
    {
        const int calls_init = g_init_calls, calls_destroy = g_destroy_calls;
        CHECK(refused_create(&tid_d, 0));
        CHECK(atoi(g_stop.err) != 0);
        char want[24];
        snprintf(want, sizeof want, "%llu", (unsigned long long)((size_t)0x7FFFFFF0u * 1024u * 1024u));
        CHECK(!strcmp(g_stop.bytes, want));
        CHECK(g_init_calls == calls_init + 1 && g_destroy_calls == calls_destroy + 1);   /* attr paired */
    }
    g_stub_config.ppu_host_stack_mb = 16;

    /* 6. pthread_attr_init refuses: the stop carries its answer, and the attribute is neither set nor destroyed. */
    {
        const int calls_stack = g_setstack_calls, calls_destroy = g_destroy_calls;
        g_attr_init_fail = EAGAIN;
        CHECK(refused_create(&tid_d, EAGAIN));
        g_attr_init_fail = 0;
        CHECK(g_setstack_calls == calls_stack && g_destroy_calls == calls_destroy);
    }

    /* 7. pthread_attr_setstacksize refuses: same stop, and the attribute that was initialised is destroyed. */
    {
        const int calls_destroy = g_destroy_calls;
        g_setstack_fail = EINVAL;
        CHECK(refused_create(&tid_d, EINVAL));
        g_setstack_fail = 0;
        CHECK(g_destroy_calls == calls_destroy + 1);
    }
    {
        g_reuse_on_refusal = 1;
        g_attr_init_fail = EAGAIN;
        CHECK(refused_create(&tid_d, EAGAIN));
        CHECK(strtoull(g_stop.bytes, NULL, 10) == 16u * 1024u * 1024u);
        g_reuse_on_refusal = 0;
        g_attr_init_fail = 0;
    }

    /* 8. And the table is intact after the refusals: the next create works. */
    {
        const uint64_t tid = run_thread(0, 2);
        CHECK(tid > tid_d);
    }

    printf("%s\n", g_fails ? "FAILED" : "ok");
    if (g_fails) dump_diagnostics();
    return g_fails ? 1 : 0;
}
