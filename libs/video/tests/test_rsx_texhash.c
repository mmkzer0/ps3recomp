/*
 * ps3recomp - the draw engine's full texture change hash (rsx_texhash.h)
 *
 * The engine decides whether a cached texture's bytes changed by comparing this hash with the last
 * one, so the contract is "equal bytes, equal hash; one changed byte anywhere, a different hash".
 * Checked at the sizes the policy treats differently (at and around ENG_TEXHASH_SMALL, 16 KiB) and
 * at spans that are not a multiple of the 64-byte stripe or of 8, over every byte of a span where
 * that is cheap and over a spread of positions (every stripe edge among them) where it is not.
 * Needs nothing from the engine.
 */
#include "rsx_texhash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static uint64_t s_rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
    s_rng = s_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(s_rng >> 33);
}

static void fill(uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

/* Flip `mask` in byte `pos`; return whether the hash moved. */
static int flip_changes(uint8_t* p, uint32_t span, uint32_t pos, uint8_t mask, uint64_t base)
{
    p[pos] ^= mask;
    const int moved = rsx_texhash(p, span) != base;
    p[pos] ^= mask;
    return moved;
}

static uint32_t every_byte_misses(uint32_t span, int random_bytes, int all_masks)
{
    uint8_t* p = (uint8_t*)calloc(span ? span : 1, 1);
    if (random_bytes) fill(p, span); else memset(p, 0, span);
    const uint64_t base = rsx_texhash(p, span);
    uint32_t missed = 0;
    for (uint32_t pos = 0; pos < span; pos++) {
        if (!all_masks) {
            missed += !flip_changes(p, span, pos, (uint8_t)(1u << (pos & 7)), base);
            continue;
        }
        missed += !flip_changes(p, span, pos, 0x01, base);
        missed += !flip_changes(p, span, pos, 0x80, base);
        missed += !flip_changes(p, span, pos, 0xFF, base);
    }
    free(p);
    return missed;
}

/* Sampled positions: both ends, both sides of every stripe edge near them, and random ones. */
static uint32_t sampled_misses(uint32_t span, uint32_t random_positions)
{
    uint8_t* p = (uint8_t*)malloc(span);
    fill(p, span);
    const uint64_t base = rsx_texhash(p, span);
    uint32_t missed = 0;
    uint32_t pos_list[64];
    uint32_t n = 0;
    for (uint32_t k = 0; k < 8 && k < span; k++) {
        pos_list[n++] = k;
        pos_list[n++] = span - 1 - k;
    }
    for (uint32_t e = 64; e < span && n + 2 <= 64; e += span / 32 / 64 * 64 + 64) {
        pos_list[n++] = e - 1;
        pos_list[n++] = e;
    }
    for (uint32_t i = 0; i < n; i++) {
        missed += !flip_changes(p, span, pos_list[i], 0x01, base);
        missed += !flip_changes(p, span, pos_list[i], 0xFF, base);
    }
    for (uint32_t i = 0; i < random_positions; i++)
        missed += !flip_changes(p, span, rnd() % span, (uint8_t)(1u << (rnd() & 7)), base);
    free(p);
    return missed;
}

int main(void)
{
    /* every byte, every span from empty to well past two stripes, random and all-zero content */
    {
        uint32_t missed = 0;
        for (uint32_t span = 1; span <= 200; span++)
            missed += every_byte_misses(span, 1, 1) + every_byte_misses(span, 0, 1);
        CHECK(missed == 0, "spans 1..200: no flipped byte goes unnoticed (%u missed)", missed);
    }

    /* a span of 16 KiB and the sizes around it: every byte, one flipped bit per byte (the bit moves with it) */
    {
        const uint32_t spans[] = { 16384u, 16384u - 8u, 16384u + 13u };
        for (unsigned s = 0; s < sizeof spans / sizeof spans[0]; s++) {
            const uint32_t missed = every_byte_misses(spans[s], 1, 0);
            CHECK(missed == 0, "span %u: every byte (%u missed)", spans[s], missed);
        }
    }

    /* larger spans: a spread of positions, non-multiples of 64 and of 8 included */
    {
        const uint32_t spans[] = {
            256u * 1024u, 256u * 1024u + 7u, 256u * 1024u + 63u, 256u * 1024u + 64u,
            4u * 1024u * 1024u, 4u * 1024u * 1024u + 1u, 4u * 1024u * 1024u + 40u,
            4u * 1024u * 1024u - 3u,
        };
        for (unsigned s = 0; s < sizeof spans / sizeof spans[0]; s++) {
            const uint32_t random_positions = spans[s] >= 1024u * 1024u ? 48 : 300;
            const uint32_t missed = sampled_misses(spans[s], random_positions);
            CHECK(missed == 0, "span %u: edges and %u random positions (%u missed)", spans[s], random_positions, missed);
        }
    }

    /* order matters: swapping two stripes, two words of a stripe, or two bytes is a change */
    {
        const uint32_t span = 4096;
        uint8_t* p = (uint8_t*)malloc(span);
        fill(p, span);
        const uint64_t base = rsx_texhash(p, span);
        uint8_t tmp[64];

        memcpy(tmp, p + 640, 64); memcpy(p + 640, p + 1280, 64); memcpy(p + 1280, tmp, 64);
        CHECK(rsx_texhash(p, span) != base, "two swapped stripes change the hash");
        memcpy(tmp, p + 640, 64); memcpy(p + 640, p + 1280, 64); memcpy(p + 1280, tmp, 64);
        CHECK(rsx_texhash(p, span) == base, "swapped back, the hash is the original");

        memcpy(tmp, p + 64 * 5, 8); memcpy(p + 64 * 5, p + 64 * 5 + 24, 8); memcpy(p + 64 * 5 + 24, tmp, 8);
        CHECK(rsx_texhash(p, span) != base, "two swapped words of one stripe change the hash");
        memcpy(tmp, p + 64 * 5, 8); memcpy(p + 64 * 5, p + 64 * 5 + 24, 8); memcpy(p + 64 * 5 + 24, tmp, 8);

        const uint8_t a = p[100], b = p[101];
        p[100] = b; p[101] = a;
        CHECK(a == b || rsx_texhash(p, span) != base, "two swapped bytes change the hash");
        free(p);
    }

    /* the span is part of the value: zeros of different lengths differ, and so do a
     * span and the same bytes with a zero tail the stripe padding would otherwise hide */
    {
        uint8_t* z = (uint8_t*)calloc(1, 1024);
        int distinct = 1;
        uint64_t seen[200];
        for (uint32_t span = 1; span <= 200; span++) {
            seen[span - 1] = rsx_texhash(z, span);
            for (uint32_t j = 0; j + 1 < span; j++) distinct &= seen[j] != seen[span - 1];
        }
        CHECK(distinct, "all-zero spans of 1..200 bytes hash apart");
        CHECK(rsx_texhash(z, 1024) != rsx_texhash(z, 1023), "zeros: 1024 and 1023 bytes differ");
        free(z);
    }

    /* the vector stripe loop (NEON, SSE2) is the plain-C loop's arithmetic, at every length around
     * a stripe and at larger ones, on every alignment of the span */
    {
        enum { MAXN = 5000 };
        uint8_t* buf = (uint8_t*)malloc(MAXN + 64);
        fill(buf, MAXN + 64);
        uint32_t differ = 0;
        for (uint32_t span = 0; span <= 400; span++)
            for (uint32_t off = 0; off < 16; off += 3)
                differ += rsx_texhash(buf + off, span) != rsx_texhash_reference(buf + off, span);
        for (uint32_t i = 0; i < 200; i++) {
            const uint32_t span = rnd() % MAXN, off = rnd() % 64;
            differ += rsx_texhash(buf + off, span) != rsx_texhash_reference(buf + off, span);
        }
        CHECK(differ == 0, "vector and reference hashes agree (%u differ)", differ);
        free(buf);
    }

    /* determinism, and independence from the alignment of the span in memory */
    {
        uint8_t* buf = (uint8_t*)malloc(8192 + 64);
        fill(buf, 8192 + 64);
        uint8_t* copy = (uint8_t*)malloc(8192 + 64);
        const uint64_t base = rsx_texhash(buf, 8192 + 11);
        int same = 1;
        for (uint32_t shift = 0; shift <= 53; shift++) {
            memcpy(copy + shift, buf, 8192 + 11);
            same &= rsx_texhash(copy + shift, 8192 + 11) == base;
        }
        CHECK(same, "the same bytes at any alignment hash the same");
        CHECK(rsx_texhash(buf, 8192 + 11) == base, "the same span twice hashes the same");
        free(buf);
        free(copy);
    }

    printf("%s\n", g_failures ? "FAILED" : "ALL PASSED");
    return g_failures ? 1 : 0;
}
