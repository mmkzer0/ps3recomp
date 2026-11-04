/* CPU cost of the MFC engine lookup under contention (spu.mfc_lookup_cache).
 *
 * Several host threads, each driving its own SPU context through the real
 * channel layer, issue the channel sequence of a small DMA: the five MFC
 * parameter writes and the command, the tag mask and update, and the tag
 * status read. Every write and read goes through mfc_for(), so with the option
 * off each one takes the registry's process-wide read lock; the lock word is
 * what bounces between the cores.
 *
 *   workload dma       the full sequence, a 128-byte GET each time (8 writes,
 *                      1 read: about the 90 % / 10 % split of a title's profile)
 *   workload channel   the same writes and read without the command: the lookup
 *                      with the least else around it
 *   work N             N steps of integer mixing between sequences, standing in for
 *                      the SPU code that runs between channel accesses (0: none, the
 *                      lookup alone, which is the contention worst case)
 *   churn N            one more thread releases and re-creates a spare
 *                      context each time the workers have issued N more
 *                      sequences, so every release publishes a generation that
 *                      drops all remembered engines
 *
 * The workers start together on a spin gate. Each reports its own thread CPU
 * time over the loop (not wall time: the issue measured CPU samples), and the
 * sum is the result line. The cache mode is the process's configuration:
 * PS3RECOMP_SPU_MFC_LOOKUP_CACHE=0|1, read once. Driver: run_mfc_lookup_bench.py.
 * Not part of any ctest tier. */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#define main bench_slots_main
#include "../../../runtime/spu/tests/test_spu_mfc_slots.c"
#undef main

#define MAX_THREADS 64

typedef struct {
    int id;
    spu_context* ctx;
    unsigned iterations;
    int with_dma;
    unsigned work;
    int dma_ok;
    double cpu_s;
} worker;

static atomic_int g_ready, g_go;
static atomic_uint_fast64_t g_issued;   /* sequences issued by all workers */
static atomic_int g_done;

static double thread_cpu_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void* worker_main(void* opaque)
{
    worker* w = opaque;
    spu_context* ctx = w->ctx;
    atomic_fetch_add(&g_ready, 1);
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) YieldProcessor();

    const uint32_t ea_base = 0x20000u + (uint32_t)w->id * 0x1000u;
    uint32_t mix = 12345u + (uint32_t)w->id;
    const double t0 = thread_cpu_seconds();
    for (unsigned i = 0; i < w->iterations; ++i) {
        /* A new LSA and EA each time: the engine backs off from a transfer
         * repeated back to back (spu.dma_repeat_limit), which is not what is
         * being measured. */
        spu_wrch(ctx, MFC_LSA, spu_make_preferred_u32(0x2000u + (i & 15u) * 128u));
        spu_wrch(ctx, MFC_EAL, spu_make_preferred_u32(ea_base + (i & 31u) * 128u));
        spu_wrch(ctx, MFC_Size, spu_make_preferred_u32(128));
        spu_wrch(ctx, MFC_TagID, spu_make_preferred_u32(i & 7u));
        if (w->with_dma) spu_wrch(ctx, MFC_Cmd, spu_make_preferred_u32(MFC_GET_CMD));
        spu_wrch(ctx, MFC_WrTagMask, spu_make_preferred_u32(1u << (i & 7u)));
        spu_wrch(ctx, MFC_WrTagUpdate, spu_make_preferred_u32(2));
        (void)spu_rdch(ctx, MFC_RdTagStat);
        for (unsigned k = 0; k < w->work; ++k) {
            mix = mix * 1664525u + 1013904223u;
            __asm__ volatile("" : "+r"(mix));
        }
        if ((i & 255u) == 255u) atomic_fetch_add_explicit(&g_issued, 256, memory_order_relaxed);
    }
    w->cpu_s = thread_cpu_seconds() - t0;
    /* The transfers really happened: the last GET's bytes are in local store. */
    w->dma_ok = 1;
    if (w->with_dma) {
        const uint8_t* last = ctx->ls + 0x2000u + ((w->iterations - 1) & 15u) * 128u;
        for (unsigned b = 0; b < 128; ++b) if (last[b] != (uint8_t)(w->id + 1)) w->dma_ok = 0;
    }
    atomic_fetch_add(&g_done, 1);
    return NULL;
}

typedef struct { spu_context* spare; uint64_t every; uint64_t cycles; int workers; } churner;

static void* churn_main(void* opaque)
{
    churner* c = opaque;
    uint64_t next = c->every;
    while (atomic_load(&g_done) < c->workers) {
        if (atomic_load_explicit(&g_issued, memory_order_relaxed) < next) { YieldProcessor(); continue; }
        next += c->every;
        (void)mfc_for(c->spare);
        spu_mfc_release(c->spare);   /* publishes a generation */
        ++c->cycles;
    }
    return NULL;
}

int main(int argc, char** argv)
{
    int threads = 8;
    unsigned iterations = 300000;
    unsigned work = 0;
    int with_dma = 1;
    uint64_t churn_every = 0;
    const char* name = "dma";
    for (int a = 1; a + 1 < argc; a += 2) {
        if (!strcmp(argv[a], "--threads")) threads = atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "--iterations")) iterations = (unsigned)strtoul(argv[a + 1], NULL, 10);
        else if (!strcmp(argv[a], "--workload")) {
            name = argv[a + 1];
            with_dma = !strcmp(name, "dma");
            if (!with_dma && strcmp(name, "channel")) { fprintf(stderr, "workload dma|channel\n"); return 2; }
        }
        else if (!strcmp(argv[a], "--work")) work = (unsigned)strtoul(argv[a + 1], NULL, 10);
        else if (!strcmp(argv[a], "--churn")) churn_every = strtoull(argv[a + 1], NULL, 10);
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (threads < 1 || threads > MAX_THREADS) { fprintf(stderr, "threads 1..%d\n", MAX_THREADS); return 2; }

    worker w[MAX_THREADS];
    pthread_t th[MAX_THREADS];
    for (int i = 0; i < threads; ++i) {
        w[i] = (worker){ .id = i, .ctx = calloc(1, sizeof(spu_context)),
                         .iterations = iterations, .with_dma = with_dma, .work = work };
        if (!w[i].ctx) return 2;
        spu_context_init(w[i].ctx, (uint32_t)i);
        memset(guest_memory + 0x20000u + (size_t)i * 0x1000u, i + 1, 0x1000);
        (void)mfc_for(w[i].ctx);   /* every context holds its slot before the loop */
    }
    churner ch = { .spare = calloc(1, sizeof(spu_context)), .every = churn_every, .workers = threads };
    if (!ch.spare) return 2;
    spu_context_init(ch.spare, 99);

    struct timespec w0, w1;
    pthread_t churn_th;
    for (int i = 0; i < threads; ++i) pthread_create(&th[i], NULL, worker_main, &w[i]);
    if (churn_every) pthread_create(&churn_th, NULL, churn_main, &ch);
    while (atomic_load(&g_ready) < threads) YieldProcessor();
    const uint64_t gen0 = atomic_load(&s_mfc_cache_generation);
    clock_gettime(CLOCK_MONOTONIC, &w0);
    atomic_store_explicit(&g_go, 1, memory_order_release);
    for (int i = 0; i < threads; ++i) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &w1);
    if (churn_every) pthread_join(churn_th, NULL);

    double cpu = 0;
    int dma_ok = 1;
    for (int i = 0; i < threads; ++i) { cpu += w[i].cpu_s; dma_ok &= w[i].dma_ok; }
    const double wall = (double)(w1.tv_sec - w0.tv_sec) + (double)(w1.tv_nsec - w0.tv_nsec) * 1e-9;
    const double channel_ops = (double)threads * iterations * (with_dma ? 9.0 : 8.0);
    printf("RESULT workload=%s threads=%d iterations=%u work=%u churn_every=%llu cache=%d cpu_s=%.6f "
           "wall_s=%.6f ns_per_channel_op=%.2f generations=%llu dma_ok=%d\n",
           name, threads, iterations, work, (unsigned long long)churn_every, ps3_cfg_spu_mfc_lookup_cache(),
           cpu, wall, cpu * 1e9 / channel_ops,
           (unsigned long long)(atomic_load(&s_mfc_cache_generation) - gen0), dma_ok);
    return dma_ok ? 0 : 1;
}
