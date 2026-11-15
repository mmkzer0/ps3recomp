/* Many short SPU jobs on contending host workers, each doing a real MFC GET
 * and PUT before releasing its context engine. This exercises the lookup,
 * claim and release cadence of JobQueue2 without title data.
 *
 * Not part of any ctest tier. Driver: run_mfc_lookup_bench.py. */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define main bench_slots_main
#include "../../../runtime/spu/tests/test_spu_mfc_contexts.c"
#undef main

#define MAX_THREADS 64

typedef struct {
    int id;
    spu_context* ctx;
    unsigned iterations;
    uint32_t last_lsa;
    uint32_t last_out_ea;
    int dma_ok;
    double cpu_s;
} worker;

static atomic_int g_ready, g_go;

static double thread_cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void issue_transfer(spu_context* ctx, uint32_t cmd, uint32_t lsa,
                           uint32_t ea, uint32_t tag)
{
    spu_wrch(ctx, MFC_LSA, spu_make_preferred_u32(lsa));
    spu_wrch(ctx, MFC_EAH, spu_make_preferred_u32(0));
    spu_wrch(ctx, MFC_EAL, spu_make_preferred_u32(ea));
    spu_wrch(ctx, MFC_Size, spu_make_preferred_u32(128));
    spu_wrch(ctx, MFC_TagID, spu_make_preferred_u32(tag));
    spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(cmd));
}

static void* worker_main(void* opaque)
{
    worker* w = opaque;
    spu_context* ctx = w->ctx;
    const uint32_t ea_base = 0x20000u + (uint32_t)w->id * 0x2000u;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) YieldProcessor();

    w->dma_ok = 1;
    const double t0 = thread_cpu_seconds();
    for (unsigned i = 0; i < w->iterations; ++i) {
        const uint32_t slot = i & 15u;
        const uint32_t tag = i & 7u;
        const uint32_t lsa = 0x4000u + slot * 128u;
        const uint32_t in_ea = ea_base + slot * 128u;
        const uint32_t out_ea = ea_base + 0x1000u + slot * 128u;

        issue_transfer(ctx, MFC_GET_CMD, lsa, in_ea, tag);
        issue_transfer(ctx, MFC_PUT_CMD, lsa, out_ea, tag);
        spu_wrch(ctx, MFC_WrTagMask, spu_make_preferred_u32(1u << tag));
        spu_wrch(ctx, MFC_WrTagUpdate, spu_make_preferred_u32(2));
        u128 status = spu_rdch(ctx, MFC_RdTagStat);
        if (spu_preferred_u32(&status) != (1u << tag)) w->dma_ok = 0;

        /* A JobQueue2 worker owns this context only until the short job ends. */
        spu_mfc_release(ctx);
        w->last_lsa = lsa;
        w->last_out_ea = out_ea;
    }
    w->cpu_s = thread_cpu_seconds() - t0;
    return NULL;
}

int main(int argc, char** argv)
{
    int threads = 13;
    unsigned iterations = 10000;
    for (int a = 1; a + 1 < argc; a += 2) {
        if (!strcmp(argv[a], "--threads")) threads = atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "--iterations"))
            iterations = (unsigned)strtoul(argv[a + 1], NULL, 10);
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (threads < 1 || threads > MAX_THREADS || !iterations) {
        fprintf(stderr, "threads 1..%d and iterations > 0 required\n", MAX_THREADS);
        return 2;
    }

    worker w[MAX_THREADS];
    pthread_t th[MAX_THREADS];
    for (int i = 0; i < threads; ++i) {
        w[i] = (worker){ .id = i, .ctx = calloc(1, sizeof(spu_context)),
                         .iterations = iterations };
        if (!w[i].ctx) return 2;
        spu_context_init(w[i].ctx, (uint32_t)i);
        const uint32_t ea_base = 0x20000u + (uint32_t)i * 0x2000u;
        memset(guest_memory + ea_base, i + 1, 0x1000);
        memset(guest_memory + ea_base + 0x1000u, 0, 0x1000);
        if (pthread_create(&th[i], NULL, worker_main, &w[i]) != 0) return 2;
    }
    while (atomic_load(&g_ready) < threads) YieldProcessor();
    struct timespec w0, w1;
    clock_gettime(CLOCK_MONOTONIC, &w0);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    for (int i = 0; i < threads; ++i) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &w1);

    double cpu = 0;
    int dma_ok = 1;
    for (int i = 0; i < threads; ++i) {
        const uint8_t value = (uint8_t)(i + 1);
        const uint8_t* ls = w[i].ctx->ls + w[i].last_lsa;
        const uint8_t* out = guest_memory + w[i].last_out_ea;
        for (unsigned b = 0; b < 128; ++b)
            if (ls[b] != value || out[b] != value) dma_ok = 0;
        if (spu_mfc_has_engine(w[i].ctx)) dma_ok = 0;
        cpu += w[i].cpu_s;
        spu_mfc_release(w[i].ctx);
        free(w[i].ctx);
    }
    const double wall = (double)(w1.tv_sec - w0.tv_sec) +
                        (double)(w1.tv_nsec - w0.tv_nsec) * 1e-9;
    const uint64_t jobs = (uint64_t)threads * iterations;
    printf("RESULT threads=%d iterations=%u jobs=%llu cpu_s=%.6f wall_s=%.6f "
           "ns_per_job=%.2f dma_ok=%d\n",
           threads, iterations, (unsigned long long)jobs, cpu, wall,
           cpu * 1e9 / (double)jobs, dma_ok);
    return dma_ok ? 0 : 1;
}
