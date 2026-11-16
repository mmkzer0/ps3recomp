/* Warmed CPU throughput of the portable RSX BC decoder. */
#include "rsx_bc_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WARMUPS 2u
#define SAMPLES 31u

static uint64_t s_sink;

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec * 1.0e-9;
}

static int compare_double(const void *left, const void *right)
{
    const double a = *(const double *)left;
    const double b = *(const double *)right;
    return (a > b) - (a < b);
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value = value * 1664525u + 1013904223u;
    *state = value;
    return value;
}

static const char *format_name(rsx_bc_format format)
{
    switch (format) {
    case RSX_BC_FORMAT_BC1: return "BC1";
    case RSX_BC_FORMAT_BC2: return "BC2";
    default: return "BC3";
    }
}

static int benchmark_case(rsx_bc_format format, uint32_t width, uint32_t height)
{
    const size_t block_bytes = format == RSX_BC_FORMAT_BC1 ? 8u : 16u;
    const size_t blocks_w = width / 4u + (width % 4u != 0u);
    const size_t blocks_h = height / 4u + (height % 4u != 0u);
    const size_t source_size = blocks_w * blocks_h * block_bytes;
    const size_t destination_pitch = (size_t)width * 4u;
    const size_t destination_size = destination_pitch * height;
    uint8_t *source = malloc(source_size);
    uint8_t *destination = malloc(destination_size);
    if (!source || !destination) {
        fprintf(stderr, "allocation failed for %s %ux%u\n",
                format_name(format), width, height);
        free(source);
        free(destination);
        return 0;
    }

    uint32_t random = 0xA341316Cu ^ width ^ (height << 7) ^ (uint32_t)format;
    for (size_t i = 0; i < source_size; ++i)
        source[i] = (uint8_t)(next_random(&random) >> 24);

    for (unsigned i = 0; i < WARMUPS; ++i) {
        if (!rsx_bc_decode(source, source_size, width, height, format,
                           destination, destination_pitch)) {
            fprintf(stderr, "warmup decode failed for %s %ux%u\n",
                    format_name(format), width, height);
            free(source);
            free(destination);
            return 0;
        }
    }

    double samples[SAMPLES];
    for (unsigned i = 0; i < SAMPLES; ++i) {
        const double start = monotonic_seconds();
        const int ok = rsx_bc_decode(source, source_size, width, height, format,
                                     destination, destination_pitch);
        const double elapsed = monotonic_seconds() - start;
        if (!ok || elapsed <= 0.0) {
            fprintf(stderr, "measured decode failed for %s %ux%u\n",
                    format_name(format), width, height);
            free(source);
            free(destination);
            return 0;
        }
        samples[i] = elapsed;
        for (size_t byte = 0; byte < destination_size; byte += 4093u)
            s_sink += destination[byte];
    }
    qsort(samples, SAMPLES, sizeof(samples[0]), compare_double);
    const double median = samples[SAMPLES / 2u];
    const double texels = (double)width * height;
    const double decoded_mb = texels * 4.0 / 1.0e6;
    printf("%-3s %4ux%-4u %9.2f MB/s %8.2f ns/texel\n",
           format_name(format), width, height, decoded_mb / median,
           median * 1.0e9 / texels);
    free(source);
    free(destination);
    return 1;
}

int main(void)
{
    static const uint32_t sizes[] = { 64u, 128u, 256u, 512u, 1024u, 2048u };
    printf("rsx_bc_decode CPU decode throughput; median of %u samples after %u warmups\n",
           SAMPLES, WARMUPS);
    puts("format size        decoded throughput latency");
    for (unsigned f = 1u; f <= 3u; ++f) {
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            if (!benchmark_case((rsx_bc_format)f, sizes[i], sizes[i])) return 1;
        }
    }
    if (s_sink == UINT64_MAX) puts("sink sentinel");
    return 0;
}
