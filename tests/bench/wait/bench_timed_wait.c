/*
 * Timed-wait overshoot, one cell per run: an unsignalled wait of T on one object kind, repeated.
 *
 *   bench_timed_wait <cond|queue|sem|shim|mutex> <T_us> <iterations> [spinners]
 *
 * Prints one line: kind T_us n p50 p90 p99 max (overshoot = elapsed - T, ns) cpu_ns_per_call vcsw_per_call.
 * It uses only the lv2 syscall handlers and the Win32 shim API, so the same file builds against the tree
 * before and after #128 (tests/bench/wait/run_timed_wait_bench.py builds both and interleaves the rounds).
 * "spinners" busy threads run during the measurement (controlled contention).
 */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_cond.h"
#include "runtime/syscalls/sys_event.h"
#include "runtime/syscalls/sys_mutex.h"
#include "runtime/syscalls/sys_semaphore.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

uint8_t* vm_base;
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{ (void)image; (void)image_size; (void)args_ea; return 0; }
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }
uint32_t yz_thread_current_id(void) { return 0; }

static uint64_t now_ns(void)
{
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

typedef int64_t (*sc_fn)(ppu_context*);
static int32_t call(sc_fn f, uint64_t tid, uint64_t a, uint64_t b, uint64_t c)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.thread_id = tid;
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c;
    return (int32_t)f(&ctx);
}
static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t g_obj, g_mutex;
static HANDLE g_event;

static int one_wait(const char* kind, uint64_t us)
{
    if (!strcmp(kind, "queue")) return call(sys_event_queue_receive, 2, g_obj, 0x300, us) == (int32_t)CELL_ETIMEDOUT;
    if (!strcmp(kind, "sem"))   return call(sys_semaphore_wait, 2, g_obj, us, 0) == (int32_t)CELL_ETIMEDOUT;
    if (!strcmp(kind, "cond"))  return call(sys_cond_wait, 5, g_obj, us, 0) == (int32_t)CELL_ETIMEDOUT;
    if (!strcmp(kind, "mutex")) return call(sys_mutex_lock, 6, g_mutex, us, 0) == (int32_t)CELL_ETIMEDOUT;
    if (!strcmp(kind, "shim"))  return WaitForSingleObject(g_event, (DWORD)(us / 1000)) == WAIT_TIMEOUT;
    return 0;
}

static volatile int g_stop;
static void* spinner(void* a) { (void)a; while (!g_stop) { } return NULL; }

static int cmp(const void* a, const void* b)
{
    const int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv)
{
    if (argc < 4) return 2;
    const char* kind = argv[1];
    const uint64_t us = strtoull(argv[2], NULL, 10);
    const int n = atoi(argv[3]);
    const int spin = argc > 4 ? atoi(argv[4]) : 0;
    if (!strcmp(kind, "shim") && us < 1000) { printf("%s %llu n/a\n", kind, (unsigned long long)us); return 0; }

    vm_base = (uint8_t*)calloc(1, 65536);
    if (!strcmp(kind, "queue")) { call(sys_event_queue_create, 1, 0x200, 0, 0x1234); g_obj = be_read(0x200); }
    if (!strcmp(kind, "sem"))   { ppu_context c; memset(&c, 0, sizeof c); c.gpr[3] = 0x220; c.gpr[6] = 10; sys_semaphore_create(&c); g_obj = be_read(0x220); }
    if (!strcmp(kind, "cond")) {
        call(sys_mutex_create, 1, 0x230, 0, 0); g_mutex = be_read(0x230);
        call(sys_cond_create, 1, 0x234, g_mutex, 0); g_obj = be_read(0x234);
        call(sys_mutex_lock, 5, g_mutex, 0, 0);
    }
    if (!strcmp(kind, "mutex")) {
        call(sys_mutex_create, 1, 0x230, 0, 0); g_mutex = be_read(0x230);
        call(sys_mutex_lock, 5, g_mutex, 0, 0);                  /* held: every timed lock by tid 6 contends */
    }
    if (!strcmp(kind, "shim")) g_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    pthread_t sp[16];
    for (int i = 0; i < spin && i < 16; i++) pthread_create(&sp[i], NULL, spinner, NULL);

    int64_t* over = (int64_t*)malloc(sizeof(int64_t) * (size_t)n);
    for (int i = 0; i < 20; i++) one_wait(kind, us);          /* warm up */
    struct rusage r0, r1;
    getrusage(RUSAGE_SELF, &r0);
    const uint64_t wall0 = now_ns();
    int bad = 0;
    for (int i = 0; i < n; i++) {
        const uint64_t t0 = now_ns();
        if (!one_wait(kind, us)) bad++;
        over[i] = (int64_t)(now_ns() - t0) - (int64_t)(us / (strcmp(kind, "shim") ? 1 : 1000) * (strcmp(kind, "shim") ? 1000 : 1000000));
    }
    getrusage(RUSAGE_SELF, &r1);
    (void)wall0;
    g_stop = 1;
    for (int i = 0; i < spin && i < 16; i++) pthread_join(sp[i], NULL);
    if (bad) { fprintf(stderr, "%s %llu: %d waits did not time out\n", kind, (unsigned long long)us, bad); return 1; }

    qsort(over, (size_t)n, sizeof *over, cmp);
    /* CPU of the main thread's process minus nothing: with spinners the process figure includes them, so report
     * it only for spin=0. */
    const double cpu = ((r1.ru_utime.tv_sec - r0.ru_utime.tv_sec) * 1e9 + (r1.ru_utime.tv_usec - r0.ru_utime.tv_usec) * 1e3 +
                        (r1.ru_stime.tv_sec - r0.ru_stime.tv_sec) * 1e9 + (r1.ru_stime.tv_usec - r0.ru_stime.tv_usec) * 1e3) / n;
    printf("%s %llu %d %lld %lld %lld %lld %.0f %.2f\n", kind, (unsigned long long)us, n,
           (long long)over[n / 2], (long long)over[(size_t)n * 90 / 100], (long long)over[(size_t)n * 99 / 100],
           (long long)over[n - 1], spin ? -1.0 : cpu, (double)(r1.ru_nvcsw - r0.ru_nvcsw) / n);
    return 0;
}
