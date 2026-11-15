/* The host stack of an SPU thread-group member (spu.host_stack_mb) through the real lv2_register.c:
 *
 *   - PS3_HOST_STACK_HWM=1: a member that recurses a known number of frames reports a high-water mark of at least
 *     that depth, and no more than the depth plus the few hundred KB the top of a thread's stack holds, in its exit
 *     line, and the process-exit summary reports one that is still running; both name the configured reserve.
 *   - A host that refuses the stack ends the run with the stop host-thread-stack (role spu) instead of starting the
 *     thread on the default stack: pthread_attr_init refusing (the attribute is then never set or destroyed),
 *     pthread_attr_setstacksize refusing, and a reservation the host cannot hold (an absurd spu.host_stack_mb),
 *     each in a child process, since a stop ends the process.
 *
 * Nothing sleeps: the live thread parks on flags the test sets, and the others are joined.
 *
 * clang -std=gnu17 -D_GNU_SOURCE -I include -I runtime -I runtime/syscalls -I runtime/spu -I libs/video
 *   runtime/spu/tests/test_spu_group_host_stack.c -Wl,-dead_strip -pthread -o /tmp/test_spu_group_host_stack
 */
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

/* The attribute calls an SPU host thread's create makes, made to fail in a child. */
static int g_attr_init_fail, g_setstack_fail;
static int test_attr_init(pthread_attr_t* a) { return g_attr_init_fail ? g_attr_init_fail : pthread_attr_init(a); }
static int g_destroy_calls, g_setstack_calls;
static int test_attr_setstacksize(pthread_attr_t* a, size_t n)
{
    g_setstack_calls++;
    return g_setstack_fail ? g_setstack_fail : pthread_attr_setstacksize(a, n);
}
static int test_attr_destroy(pthread_attr_t* a) { g_destroy_calls++; return pthread_attr_destroy(a); }
#define PS3_HOST_THREAD_ATTR_INIT test_attr_init
#define pthread_attr_setstacksize test_attr_setstacksize
#define pthread_attr_destroy test_attr_destroy

int64_t lv2_spu_lock_line_classify(uint32_t a, uint64_t f) { (void)a; (void)f; return 0; }
#include "../../syscalls/lv2_register.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../../tests/guest_range_catch.h"

uint8_t* vm_base;
uint32_t ppu_vm_size;
void SetLastError(DWORD e) { (void)e; }

/* spu.host_stack_mb, set by the test; and the stop, which a child reports on stderr and ends with the stop's status. */
static ps3_config g_cfg = { .spu_host_stack_mb = 16 };
const ps3_config* ps3_config_get(void) { return &g_cfg; }
_Noreturn void ps3_stop(ps3_stop_kind kind, const char* what, const char* remedy,
                        const ps3_stop_field* fields, size_t nfields, const ps3_stop_site* site)
{
    (void)what; (void)site;
    fprintf(stderr, "STOP kind=%d", (int)kind);
    for (size_t i = 0; i < nfields; i++) fprintf(stderr, " %s=%s", fields[i].key, fields[i].value);
    fprintf(stderr, " remedy=\"%s\"\n", remedy);
    fflush(stderr);
    _exit(70);
}

/* Link stubs for what lv2_register.c references and the rest of the runtime supplies; none is reached below, except
 * spu_lifted_thread_run, which a lifted thread's proc calls: here it recurses a known depth, and can park. */
uint32_t g_spu_interp_last_pc;
uint64_t g_spu_interp_steps;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void ppu_guard_page(uint32_t ea) { (void)ea; }
uint32_t ps3_spu_image_source_ea(uint32_t img_ea) { (void)img_ea; return 0; }
void spu_coh_unregister(spu_context* c) { (void)c; }
int spu_elf_load_to_ls(const uint8_t* image, size_t n, uint8_t* ls, uint32_t* entry) { (void)image; (void)n; (void)ls; (void)entry; return 0; }
uint32_t spu_interp_run(spu_context* c, uint32_t lsa) { (void)c; (void)lsa; return 0; }
int spu_lifted_thread_available(uint32_t entry) { (void)entry; return 0; }
void spu_lifted_thread_setup(struct spu_context* c, const spu_lifted_thread_desc* d) { (void)c; (void)d; }
spu_ppu_fallback_fn spu_lookup_ppu_fallback(uint32_t entry, void** user) { (void)entry; (void)user; return 0; }
void spu_mfc_release(spu_context* c) { (void)c; }
void spu_raw_note_image(uint32_t src_ea, uint32_t entry) { (void)src_ea; (void)entry; }
int32_t spu_registry_fallback(uint32_t a, uint32_t b, uint32_t c, void* d) { (void)a; (void)b; (void)c; (void)d; return 0; }
spu_lifted_entry_fn spu_workload_find_img(uint64_t fp, int* id) { (void)fp; (void)id; return 0; }
uint64_t spu_workload_fingerprint(const void* d, size_t n) { (void)d; (void)n; return 0; }
void sys_event_queue_push_from_kernel(uint32_t q, uint64_t s, uint64_t d1, uint64_t d2, uint64_t d3, const char* who)
{ (void)q; (void)s; (void)d1; (void)d2; (void)d3; (void)who; }
uint32_t spurs_port_queue(uint32_t port) { (void)port; return 0; }
int64_t sys_event_flag_set(ppu_context* c) { (void)c; return 0; }
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define FRAME  32768u        /* each recursion level reserves this much and touches every page of it */
#define LEVELS 6u            /* what the lifted thread recurses before it ends */

/* Not optimised into a loop at -O2: the frame's address escapes into an asm both before and after the call, so each
 * level keeps its own FRAME bytes live across the call below it. */
static char recurse_frames(unsigned levels) __attribute__((noinline));
static char recurse_frames(unsigned levels)
{
    volatile char buf[FRAME];
    for (size_t i = 0; i < FRAME; i += 512) buf[i] = (char)levels;
    __asm__ volatile("" :: "r"(buf) : "memory");
    char r = levels ? recurse_frames(levels - 1) : 0;
    __asm__ volatile("" :: "r"(buf) : "memory");
    return (char)(r + buf[0]);
}

static volatile int g_park, g_parked, g_release;   /* the thread parks after its recursion when g_park is set */
void spu_lifted_thread_run(struct spu_context* c, spu_lifted_thread_result* out)
{
    (void)c;
    memset(out, 0, sizeof *out);
    (void)recurse_frames(LEVELS);
    if (__atomic_load_n(&g_park, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&g_parked, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_release, __ATOMIC_ACQUIRE)) sched_yield();
    }
}

/* A group with one lifted thread, not yet started. */
static spu_group_t* batch_group;
static spu_thread_t* make_thread(void)
{
    spu_group_t* g = batch_group ? batch_group : spu_alloc_group(); assert(g);
    spu_thread_t* t = spu_alloc_thread(); assert(t);
    t->group_id = g->id;
    const unsigned member = g->num_threads++;
    g->state = SPU_GROUP_STATE_RUNNING;
    g->thread_indices[member] = (uint32_t)(t - s_spu_threads);
    t->sctx = (struct spu_context*)calloc(1, sizeof(spu_context)); assert(t->sctx);
    t->local_store = (uint8_t*)calloc(1, 64); assert(t->local_store);
    pthread_mutex_init(&t->finish_event.mu, NULL);
    pthread_cond_init(&t->finish_event.cv, NULL);
    t->finish_event.done = 0;
    spu_thread_set_running(t, 1);
    return t;
}

/* Start `t` the way group_start does. */
static void start_thread(spu_thread_t* t) { spu_spawn_host_thread(&t->host_thread, spu_exec_thread_proc, t, t->tid); }

/* Run `fn` with stderr going to a temporary file and return what it wrote (malloc'd). */
static char* capture_stderr(void (*fn)(void*), void* arg)
{
    char path[] = "/tmp/spu-group-host-stack-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    fflush(stderr);
    const int saved = dup(2);
    dup2(fd, 2);
    fn(arg);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    char* out = (char*)calloc(1, 1u << 20);
    const ssize_t n = pread(fd, out, (1u << 20) - 1, 0);
    if (n < 0) out[0] = 0;
    close(fd);
    unlink(path);
    return out;
}

/* From `text`: the figure after `field` on the "[HOSTSTACK] spu id=<tid> <state>" line, or -1. */
static long long mark(const char* text, uint32_t tid, const char* state, const char* field)
{
    char key[64];
    snprintf(key, sizeof key, "[HOSTSTACK] spu id=%u %s ", tid, state);
    const char* line = strstr(text, key);
    if (!line) return -1;
    const char* end = strchr(line, '\n');
    const char* f = strstr(line, field);
    if (!f || (end && f > end)) return -1;
    return atoll(f + strlen(field));
}

static void run_and_join(void* arg)
{
    spu_thread_t* t = (spu_thread_t*)arg;
    start_thread(t);
    pthread_join(t->host_thread, NULL);
}

static void run_summary(void* arg) { (void)arg; spu_hs_summary(); }

static void destroy_group(spu_thread_t* t)
{
    const uint32_t gid = t->group_id;
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = gid;
    CHECK(sys_spu_thread_group_destroy_handler(&ctx) == 0, "group_destroy of a finished group");
}

/* A child that starts one thread under `mode`, which the stop ends; its stderr comes back in `out`. */
static int refused_child(int mode, char* out, size_t cap)
{
    int fds[2];
    assert(pipe(fds) == 0);
    fflush(NULL);
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        if (mode == 1) g_attr_init_fail = EAGAIN;
        if (mode == 2) g_setstack_fail = EINVAL;
        if (mode == 3) g_cfg.spu_host_stack_mb = 0x7FFFFFF0u;           /* ~2 PiB */
        spu_thread_t* t = make_thread();
        start_thread(t);
        _exit(0);                                                      /* reached only if nothing refused */
    }
    close(fds[1]);
    size_t len = 0;
    for (ssize_t n; (n = read(fds[0], out + len, cap - 1 - len)) > 0;) len += (size_t)n;
    out[len] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void)
{
    vm_base = calloc(1, 1u << 20); assert(vm_base);
    ppu_vm_size = 1u << 20;
    setenv("PS3_HOST_STACK_HWM", "1", 1);

    /* Refused stacks first, while no thread exists to fork around. */
    static const char* const names[] = { "", "pthread_attr_init", "pthread_attr_setstacksize", "an absurd reserve" };
    for (int mode = 1; mode <= 3; mode++) {
        char out[4096];
        const int rc = refused_child(mode, out, sizeof out);
        CHECK(rc == 70, "%s refused: the run ends with the stop's status (got %d)", names[mode], rc);
        CHECK(strstr(out, "role=spu ") != NULL && strstr(out, "bytes=") != NULL && strstr(out, "errno=") != NULL,
              "%s refused: the stop names the role, the bytes and the errno: %s", names[mode], out);
        CHECK(strstr(out, "spu.host_stack_mb") != NULL, "%s refused: the remedy names the key", names[mode]);
        char want[32];
        if (mode == 1) { snprintf(want, sizeof want, "errno=%d", EAGAIN);
                         CHECK(strstr(out, want) != NULL, "pthread_attr_init refused: the stop carries EAGAIN: %s", out); }
        if (mode == 2) { snprintf(want, sizeof want, "errno=%d", EINVAL);
                         CHECK(strstr(out, want) != NULL, "pthread_attr_setstacksize refused: the stop carries EINVAL: %s", out); }
    }
    {   /* The in-process view of the attribute: init refusing sets and destroys nothing (the child's counters died). */
        g_attr_init_fail = EAGAIN;
        const int s0 = g_setstack_calls, d0 = g_destroy_calls;
        pthread_t th;
        CHECK(ps3_host_pthread_create(&th, 1u << 20, 0, (void* (*)(void*))abort, NULL) == EAGAIN, "init refusal is returned");
        g_attr_init_fail = 0;
        CHECK(g_setstack_calls == s0 && g_destroy_calls == d0, "a refused pthread_attr_init is never followed by a set or a destroy");
        g_setstack_fail = EINVAL;
        CHECK(ps3_host_pthread_create(&th, 1u << 20, 0, (void* (*)(void*))abort, NULL) == EINVAL, "set refusal is returned");
        g_setstack_fail = 0;
        CHECK(g_destroy_calls == d0 + 1, "a refused pthread_attr_setstacksize still destroys the attribute that was initialised");
    }

    /* The exit line of a member that recursed LEVELS frames. */
    {
        spu_thread_t* t = make_thread();
        const uint32_t tid = t->tid;
        char* text = capture_stderr(run_and_join, t);
        const long long hw = mark(text, tid, "exited", "high_water=");
        const long long rsv = mark(text, tid, "exited", "reserved=");
        const long long floor = (long long)LEVELS * FRAME;
        CHECK(hw >= floor && hw < floor + 512 * 1024, "exit report: %lld bytes for %u frames of %u (text: %s)", hw, LEVELS, FRAME, text);
        CHECK(rsv == (long long)ps3_spu_host_stack_bytes(), "exit report names the configured reserve (%lld)", rsv);
        free(text);
        destroy_group(t);
    }

    /* A member still running is reported live by the summary, and again at its exit. */
    {
        spu_thread_t* t = make_thread();
        const uint32_t tid = t->tid;
        __atomic_store_n(&g_park, 1, __ATOMIC_RELEASE);
        start_thread(t);
        while (!__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE)) sched_yield();
        char* text = capture_stderr(run_summary, NULL);
        const long long hw = mark(text, tid, "live", "high_water=");
        const long long floor = (long long)LEVELS * FRAME;
        CHECK(hw >= floor && hw < floor + 512 * 1024, "live report: %lld bytes for %u frames of %u (text: %s)", hw, LEVELS, FRAME, text);
        CHECK(strstr(text, "[HOSTSTACK] spu summary: threads=") != NULL, "the summary line follows");
        free(text);
        __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
        pthread_join(t->host_thread, NULL);
        destroy_group(t);
    }
    {
        spu_thread_t* threads[80];
        __atomic_store_n(&g_release, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_parked, 0, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < 80; ++i) {
            if (!(i % 8)) batch_group = spu_alloc_group();
            threads[i] = make_thread();
        }
        batch_group = NULL;
        for (unsigned i = 0; i < 80; ++i) start_thread(threads[i]);
        while (__atomic_load_n(&g_parked, __ATOMIC_ACQUIRE) != 80) sched_yield();
        char* text = capture_stderr(run_summary, NULL);
        for (unsigned i = 0; i < 80; ++i)
            CHECK(mark(text, threads[i]->tid, "live", "high_water=") >= LEVELS * FRAME,
                  "member %u beyond the old 64-slot bound is measured", i);
        free(text);
        __atomic_store_n(&g_release, 1, __ATOMIC_RELEASE);
        for (unsigned i = 0; i < 80; ++i) {
            pthread_join(threads[i]->host_thread, NULL);
        }
        for (unsigned i = 0; i < 80; i += 8) {
            destroy_group(threads[i]);
        }
    }

    printf("[spu_group_host_stack] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    free(vm_base);
    return fails ? 1 : 0;
}
