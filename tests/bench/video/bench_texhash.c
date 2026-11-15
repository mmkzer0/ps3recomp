/*
 * ps3recomp - throughput of the draw engine's full texture hash, old against new
 *
 * Not a test (no pass/fail, timing only): the old byte-for-8-bytes FNV-1a chain the engine used
 * (`fnv_old`, copied here verbatim), rsx_texhash_reference() (the plain-C stripe loop, what a target
 * without NEON or SSE2 runs) and rsx_texhash() (the vector loop) over spans of 16 KiB to 4 MiB of a
 * deterministic buffer, alternating the three for ROUNDS rounds and reporting the median GB/s of
 * each. Build with the optimiser on and run on an idle host:
 *
 *   cc -O2 -std=c17 -I libs/video tests/bench/video/bench_texhash.c -o bench_texhash && ./bench_texhash
 *
 * A span is hashed back to back until about 0.15 s have passed, so the 16 KiB span is L1-resident
 * and the 4 MiB one is L2-resident on the Apple cores, the shapes the engine's rehash sees.
 */
#include "rsx_texhash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROUNDS 3

static uint64_t fnv_old(const uint8_t* src, uint32_t span)
{
    uint64_t hash = 1469598103934665603ull;
    uint32_t i = 0;
    for (; i + 8 <= span; i += 8) {
        uint64_t word;
        memcpy(&word, src + i, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ull;
    }
    for (; i < span; i++) { hash ^= src[i]; hash *= 1099511628211ull; }
    return hash;
}

static uint64_t new_hash(const uint8_t* src, uint32_t span) { return rsx_texhash(src, span); }
static uint64_t scalar_hash(const uint8_t* src, uint32_t span) { return rsx_texhash_reference(src, span); }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint64_t g_sink;

static double gbps(uint64_t (*fn)(const uint8_t*, uint32_t), const uint8_t* p, uint32_t span)
{
    for (int i = 0; i < 16; i++) g_sink += fn(p, span);   /* warm */
    uint64_t iters = 0;
    const double t0 = now();
    double t;
    do {
        for (int i = 0; i < 64; i++) g_sink += fn(p + (i & 1), span - 1);
        iters += 64;
        t = now() - t0;
    } while (t < 0.15);
    return (double)iters * (double)(span - 1) / t / 1e9;
}

static int cmp(const void* a, const void* b)
{
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

int main(void)
{
    const uint32_t spans[] = { 16u * 1024u, 64u * 1024u, 256u * 1024u, 1024u * 1024u, 4u * 1024u * 1024u };
    const unsigned nspans = sizeof spans / sizeof spans[0];
    uint8_t* buf = (uint8_t*)malloc(4u * 1024u * 1024u + 8u);
    uint64_t s = 0x243F6A8885A308D3ull;
    for (uint32_t i = 0; i < 4u * 1024u * 1024u + 8u; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        buf[i] = (uint8_t)(s >> 56);
    }
    printf("%-10s %10s %10s %10s %8s %8s   (median of %d alternating rounds)\n", "span", "FNV GB/s",
           "scalar", "vector", "vec/FNV", "vec/scal", ROUNDS);
    for (unsigned k = 0; k < nspans; k++) {
        double o[ROUNDS], sc[ROUNDS], v[ROUNDS];
        for (int r = 0; r < ROUNDS; r++) {
            o[r] = gbps(fnv_old, buf, spans[k]);
            sc[r] = gbps(scalar_hash, buf, spans[k]);
            v[r] = gbps(new_hash, buf, spans[k]);
        }
        qsort(o, ROUNDS, sizeof o[0], cmp);
        qsort(sc, ROUNDS, sizeof sc[0], cmp);
        qsort(v, ROUNDS, sizeof v[0], cmp);
        printf("%-10u %10.2f %10.2f %10.2f %8.2f %8.2f\n", spans[k], o[ROUNDS / 2], sc[ROUNDS / 2],
               v[ROUNDS / 2], v[ROUNDS / 2] / o[ROUNDS / 2], v[ROUNDS / 2] / sc[ROUNDS / 2]);
    }
    free(buf);
    return g_sink == 42;   /* keep the hashes live */
}
