/* CPU decoder against the GPU tier of the BC emulation, by level size (opt-in; Metal only).
 *
 *   cmake --build <dir> --target bench_rsx_bc_gpu && <dir>/bench_rsx_bc_gpu
 *
 * Per format and square level size, medians of warmed samples:
 *   cpu decode   rsx_bc_decode into a scratch (the figure bench_rsx_bc_decode prints), wall time
 *   cpu tier     that plus the replaceRegion into an RGBA8 texture: what the backend's CPU tier costs
 *                the RSX thread, which is also its CPU time
 *   gpu upload   wall time of RsxMetalBc's decodeBlocks (copy the blocks, encode, commit) on the calling thread
 *   gpu settle   wall time of the following waitIdleWithin, the wait eng_bc_settle makes before a frame commit:
 *                an upload immediately followed by a commit, the worst case for the RSX thread
 *   gpu rsx      upload + settle: the RSX-thread time of the GPU tier, which is what the CPU tier is compared with
 *   batch4 rsx   4 uploads of the level, then one settle, per level: a frame that uploads several textures before
 *                it commits shares the GPU's latency between them
 *   gpu submit   CPU time of the submitting thread (CLOCK_THREAD_CPUTIME_ID) over the upload alone
 *   gpu exec     the command buffer's own GPUStartTime..GPUEndTime
 * and, per format, the smallest measured size where the GPU tier is cheaper than the CPU tier under each
 * definition: RSX-thread time including the settle (worst case, and 4 uploads per commit), and the submitting
 * thread's CPU time alone, which leaves the settle out and is what the first round reported. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "rsx_bc_decode.h"
#include "rsx_metal_bc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { WARMUPS = 3, SAMPLES = 31 };

static uint64_t ns_now(clockid_t clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double median(double* v, unsigned n)
{
    qsort(v, n, sizeof v[0], cmp);
    return v[n / 2u];
}

typedef struct {
    uint32_t side;
    double cpu_decode_us, cpu_tier_us, gpu_upload_us, gpu_settle_us, gpu_rsx_us, batch_rsx_us, gpu_submit_us, gpu_exec_us;
} row;

static const char* name(rsx_bc_format f) { return f == RSX_BC_FORMAT_BC1 ? "BC1" : f == RSX_BC_FORMAT_BC2 ? "BC2" : "BC3"; }

static int measure(id<MTLDevice> dev, id<MTLCommandQueue> q, RsxMetalBc* bc, rsx_bc_format f, uint32_t side, row* out)
{
    const size_t block_bytes = f == RSX_BC_FORMAT_BC1 ? 8u : 16u;
    const size_t blocks = (size_t)(side / 4u) * (side / 4u);
    const size_t src_bytes = blocks * block_bytes, pitch = (size_t)side * 4u, rgba_bytes = pitch * side;
    uint8_t* src = malloc(src_bytes);
    uint8_t* rgba = malloc(rgba_bytes);
    uint32_t state = 0xA341316Cu ^ side ^ (uint32_t)f;
    for (size_t i = 0; i < src_bytes; ++i) { state = state * 1664525u + 1013904223u; src[i] = (uint8_t)(state >> 24); }

    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.pixelFormat = MTLPixelFormatRGBA8Unorm;
    td.width = td.height = side;
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];

    double decode[SAMPLES], tier[SAMPLES], upload[SAMPLES], settle[SAMPLES], submit[SAMPLES], exec[SAMPLES], batch[SAMPLES];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block double gpu_us = 0;
    enum { BATCH = 4 };   /* RSX_METAL_BC_SLOTS: the most decodes that can be in flight without the upload itself waiting */
    for (unsigned i = 0; i < WARMUPS + SAMPLES; ++i) {
        const unsigned k = i < WARMUPS ? 0 : i - WARMUPS;
        const uint64_t d0 = ns_now(CLOCK_MONOTONIC);
        if (!rsx_bc_decode(src, src_bytes, side, side, f, rgba, pitch)) return 0;
        const uint64_t d1 = ns_now(CLOCK_MONOTONIC);
        [tex replaceRegion:MTLRegionMake2D(0, 0, side, side) mipmapLevel:0 slice:0 withBytes:rgba
               bytesPerRow:pitch bytesPerImage:0];
        const uint64_t d2 = ns_now(CLOCK_MONOTONIC);

        /* The worst case for the RSX thread: an upload immediately followed by the commit's settle. The settle is
         * the backend's own wait (waitIdleWithin), here with a budget no run reaches. */
        const uint64_t w0 = ns_now(CLOCK_MONOTONIC), c0 = ns_now(CLOCK_THREAD_CPUTIME_ID);
        const BOOL ok = [bc decodeBlocks:src size:src_bytes format:f width:side height:side into:tex slice:0 level:0
                                       z:0 queue:q completion:^(id<MTLCommandBuffer> cb) {
            gpu_us = ([cb GPUEndTime] - [cb GPUStartTime]) * 1e6;
            dispatch_semaphore_signal(done);
        }];
        const uint64_t c1 = ns_now(CLOCK_THREAD_CPUTIME_ID), w1 = ns_now(CLOCK_MONOTONIC);
        if (!ok) return 0;
        if ([bc waitIdleWithin:600000] != nil) return 0;
        const uint64_t w2 = ns_now(CLOCK_MONOTONIC);
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   /* the caller's completion runs just after the slot is released */

        /* BATCH uploads of the same level, then one settle: what a frame with several textures costs the RSX
         * thread, per level. */
        const uint64_t b0 = ns_now(CLOCK_MONOTONIC);
        for (unsigned j = 0; j < BATCH; ++j)
            if (![bc decodeBlocks:src size:src_bytes format:f width:side height:side into:tex slice:0 level:0 z:0
                            queue:q completion:NULL]) return 0;
        if ([bc waitIdleWithin:600000] != nil) return 0;
        const uint64_t b1 = ns_now(CLOCK_MONOTONIC);
        if (i >= WARMUPS) {
            decode[k] = (double)(d1 - d0) / 1e3;
            tier[k] = (double)(d2 - d0) / 1e3;
            upload[k] = (double)(w1 - w0) / 1e3;
            settle[k] = (double)(w2 - w1) / 1e3;
            submit[k] = (double)(c1 - c0) / 1e3;
            exec[k] = gpu_us;
            batch[k] = (double)(b1 - b0) / 1e3 / BATCH;
        }
    }
    out->side = side;
    out->cpu_decode_us = median(decode, SAMPLES);
    out->cpu_tier_us = median(tier, SAMPLES);
    out->gpu_upload_us = median(upload, SAMPLES);
    out->gpu_settle_us = median(settle, SAMPLES);
    out->gpu_rsx_us = out->gpu_upload_us + out->gpu_settle_us;
    out->batch_rsx_us = median(batch, SAMPLES);
    out->gpu_submit_us = median(submit, SAMPLES);
    out->gpu_exec_us = median(exec, SAMPLES);
    free(src);
    free(rgba);
    return 1;
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { puts("no Metal device: skipped"); return 77; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSError* err = nil;
        RsxMetalBc* bc = [[RsxMetalBc alloc] initWithDevice:dev host:NULL error:&err];
        if (!bc) { fprintf(stderr, "GPU decoder: %s\n", [[err localizedDescription] UTF8String]); return 1; }
        /* The listed sizes of the assignment (64..2048) and the steps between them that locate a crossover. */
        static const uint32_t sides[] = { 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048 };
        const unsigned n = sizeof sides / sizeof sides[0];
        printf("%s on %s; medians of %d samples after %d warmups; times per level in microseconds\n",
               "bench_rsx_bc_gpu", [[dev name] UTF8String], SAMPLES, WARMUPS);
        for (unsigned fi = 0; fi < 2; ++fi) {
            const rsx_bc_format f = fi ? RSX_BC_FORMAT_BC3 : RSX_BC_FORMAT_BC1;
            row rows[sizeof sides / sizeof sides[0]];
            printf("\n%s   side   texels | cpu decode  ns/tx | cpu tier  ns/tx | gpu upload | gpu settle | gpu rsx  ns/tx | batch4 rsx  ns/tx | gpu submit cpu | gpu exec\n", name(f));
            for (unsigned i = 0; i < n; ++i) {
                if (!measure(dev, q, bc, f, sides[i], &rows[i])) { fprintf(stderr, "measure failed\n"); return 1; }
                const row* r = &rows[i];
                const double texels = (double)r->side * r->side;
                printf("    %5u %8.0f | %10.1f %6.2f | %9.1f %6.2f | %10.1f | %10.1f | %9.1f %6.2f | %10.1f %6.2f | %14.1f | %8.1f\n",
                       r->side, texels, r->cpu_decode_us, r->cpu_decode_us * 1e3 / texels,
                       r->cpu_tier_us, r->cpu_tier_us * 1e3 / texels, r->gpu_upload_us, r->gpu_settle_us,
                       r->gpu_rsx_us, r->gpu_rsx_us * 1e3 / texels, r->batch_rsx_us, r->batch_rsx_us * 1e3 / texels,
                       r->gpu_submit_us, r->gpu_exec_us);
            }
            const row *rsx_x = NULL, *batch_x = NULL, *cpu_x = NULL;
            for (unsigned i = 0; i < n; ++i) {
                if (!rsx_x && rows[i].gpu_rsx_us < rows[i].cpu_tier_us) rsx_x = &rows[i];
                if (!batch_x && rows[i].batch_rsx_us < rows[i].cpu_tier_us) batch_x = &rows[i];
                if (!cpu_x && rows[i].gpu_submit_us < rows[i].cpu_tier_us) cpu_x = &rows[i];
            }
            if (rsx_x) printf("  %s crossover, RSX-thread time incl. settle, worst case (upload, then a commit: gpu rsx < cpu tier): first at %ux%u = %u texels\n",
                              name(f), rsx_x->side, rsx_x->side, rsx_x->side * rsx_x->side);
            else       printf("  %s crossover, RSX-thread time incl. settle, worst case: none up to 2048x2048\n", name(f));
            if (batch_x) printf("  %s crossover, RSX-thread time incl. settle, 4 uploads then one commit (batch4 rsx < cpu tier): first at %ux%u = %u texels\n",
                                name(f), batch_x->side, batch_x->side, batch_x->side * batch_x->side);
            else         printf("  %s crossover, RSX-thread time incl. settle, 4 uploads then one commit: none up to 2048x2048\n", name(f));
            if (cpu_x) printf("  %s crossover, submitting-thread CPU time only, settle excluded (gpu submit cpu < cpu tier): first at %ux%u = %u texels\n",
                              name(f), cpu_x->side, cpu_x->side, cpu_x->side * cpu_x->side);
            else       printf("  %s crossover, submitting-thread CPU time only: none up to 2048x2048\n", name(f));
        }
    }
    return 0;
}
