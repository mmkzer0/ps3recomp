/* Probe: the SPU trace destination is one per process, not per worker.
 *
 * argv[1] = "init" <path>: spu_trace_init(path) on this thread, SPU_TRACE is
 *          in the environment as the opt-in, then two workers trace. Both
 *          workers' PCs must land in <path> and nothing may go to stderr;
 *          init is a process-wide redirect, not a per-thread one.
 * argv[1] = "lazy": SPU_TRACE_FILE is in the environment and nobody calls
 *          init. Eight workers are released together and each writes five
 *          lines while they race the lazy initialisation. Any worker that
 *          opened the path itself (the old per-thread "w" handle) would
 *          truncate another worker's lines; one file must hold all 40.
 *
 * tools/test_spu_trace_file.py builds this against runtime/spu/spu_channels.c
 * with dead-section stripping, so only the trace path is linked in.
 */
#include "spu_context.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAZY_WORKERS 8
#define LAZY_LINES   5

void spu_trace_init(const char* path);
void spu_trace_pc(spu_context* ctx, uint32_t pc);

/* Referenced by spu_trace_pc's env-gated blocks; both gates are off here. */
uint32_t g_barrier_sync_watch = 0;
void* volatile g_pm_flow_ctx = 0;

static spu_context g_ctx[2];
static spu_context g_lazy_ctx[LAZY_WORKERS];
static atomic_int  g_go;

static void wait_for(atomic_int* p)
{
    while (!atomic_load_explicit(p, memory_order_acquire)) { /* spin */ }
}

static void* worker_init_mode(void* arg)
{
    spu_context* c = (spu_context*)arg;
    wait_for(&g_go);
    spu_trace_pc(c, c->spu_id == 0 ? 0x100u : 0x200u);
    return NULL;
}

static void* worker_lazy(void* arg)
{
    spu_context* c = (spu_context*)arg;
    wait_for(&g_go);
    for (uint32_t i = 0; i < LAZY_LINES; i++)
        spu_trace_pc(c, 0x1000u + (uint32_t)c->spu_id * 0x100u + i);
    return NULL;
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const int lazy = !strcmp(argv[1], "lazy");
    if (!lazy && argc < 3) return 2;

    pthread_t t[LAZY_WORKERS];
    if (lazy) {
        for (int i = 0; i < LAZY_WORKERS; i++)
            spu_context_init(&g_lazy_ctx[i], (uint32_t)i);
        for (int i = 0; i < LAZY_WORKERS; i++)
            if (pthread_create(&t[i], NULL, worker_lazy, &g_lazy_ctx[i]) != 0)
                return 3;
        /* Release all workers together: the first-call race is the point. */
        atomic_store_explicit(&g_go, 1, memory_order_release);
        for (int i = 0; i < LAZY_WORKERS; i++) pthread_join(t[i], NULL);
    } else {
        for (int i = 0; i < 2; i++) spu_context_init(&g_ctx[i], (uint32_t)i);
        spu_trace_init(argv[2]);
        if (pthread_create(&t[0], NULL, worker_init_mode, &g_ctx[0]) != 0) return 3;
        if (pthread_create(&t[1], NULL, worker_init_mode, &g_ctx[1]) != 0) return 3;
        atomic_store_explicit(&g_go, 1, memory_order_release);
        pthread_join(t[0], NULL);
        pthread_join(t[1], NULL);
    }

    printf("PASS: trace %s\n", lazy ? "lazy" : "init");
    return 0;
}
