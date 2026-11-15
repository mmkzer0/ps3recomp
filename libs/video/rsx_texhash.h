/*
 * ps3recomp - the draw engine's full texture change hash
 *
 * A mutation detector for a texture's source bytes, not a content id: the value is compared only
 * with the previous value of the same cached texture inside one process, and is never written to a
 * file or a capture (captures hash with SHA-256). So it may change across builds; it may not miss a
 * change.
 *
 * Why not a byte-chain FNV: one multiply per 8 bytes in one dependency chain runs at the multiply's
 * latency, about 2 B/cycle, and the FIFO walker rehashes every bound texture every few frames.
 * Here the span is read in 64-byte stripes into sixteen independent 64-bit accumulators (the
 * XXH3 long hash's shape). For word i of a stripe, v, and the stripe's key K:
 *
 *     sum[i]  += v
 *     prod[i] += lo32(v ^ K) * hi32(v ^ K)
 *
 * Every step adds constants that depend only on the stripe's data, so it is a bijection on the
 * accumulator state: two spans of one length that differ in one word never reach the same state
 * (`sum` alone sees it). Only the final fold to 64 bits can collide, at about 2^-64. K advances by
 * an odd constant per stripe, so stripes are not interchangeable: a moved block changes the hash,
 * which a plain sum would not see. A tail shorter than a stripe is zero-padded into one more
 * stripe, so every byte of the span is in exactly one stripe and the one-word guarantee holds for
 * it too; the length is folded in, so the padding is not content.
 *
 * The lanes are independent, so the stripe loop is vector code where the target has the ops
 * (NEON, SSE2) and the same arithmetic in plain C elsewhere. The three agree bit for bit
 * (test_rsx_texhash compares them); only the speed differs.
 */
#ifndef PS3RECOMP_RSX_TEXHASH_H
#define PS3RECOMP_RSX_TEXHASH_H

#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_NEON) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  include <arm_neon.h>
#  define RSX_TEXHASH_NEON 1
#elif defined(__SSE2__) || defined(_M_X64)
#  include <emmintrin.h>
#  define RSX_TEXHASH_SSE2 1
#endif

#define RSX_TEXHASH_STRIPE 64u
#define RSX_TEXHASH_KEY0   0xD6E8FEB86659FD93ull
#define RSX_TEXHASH_STEP   0x9E3779B97F4A7C15ull   /* odd */

typedef struct rsx_texhash_acc {
    uint64_t sum[8];
    uint64_t prod[8];
} rsx_texhash_acc;

static inline uint64_t rsx_texhash_load64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

/* n whole stripes at p; stripe s uses key + s * STEP. The reference arithmetic. */
static inline void rsx_texhash_blocks_scalar(rsx_texhash_acc* a, const uint8_t* p, uint32_t n,
                                             uint64_t key)
{
    for (uint32_t s = 0; s < n; s++, p += RSX_TEXHASH_STRIPE, key += RSX_TEXHASH_STEP)
        for (unsigned i = 0; i < 8; i++) {
            const uint64_t v = rsx_texhash_load64(p + 8u * i);
            const uint64_t k = v ^ key;
            a->sum[i] += v;
            a->prod[i] += (uint64_t)(uint32_t)k * (k >> 32);
        }
}

#if defined(RSX_TEXHASH_NEON)
static inline void rsx_texhash_blocks_vec(rsx_texhash_acc* a, const uint8_t* p, uint32_t n,
                                          uint64_t key)
{
    uint64x2_t sum[4], prod[4];
    for (unsigned j = 0; j < 4; j++) {
        sum[j] = vld1q_u64(a->sum + 2u * j);
        prod[j] = vld1q_u64(a->prod + 2u * j);
    }
    for (uint32_t s = 0; s < n; s++, p += RSX_TEXHASH_STRIPE, key += RSX_TEXHASH_STEP) {
        const uint64x2_t kv = vdupq_n_u64(key);
        for (unsigned j = 0; j < 4; j++) {
            const uint64x2_t v = vld1q_u64((const uint64_t*)(p + 16u * j));
            const uint64x2_t k = veorq_u64(v, kv);
            prod[j] = vmlal_u32(prod[j], vmovn_u64(k), vshrn_n_u64(k, 32));   /* lo32 * hi32 */
            sum[j] = vaddq_u64(sum[j], v);
        }
    }
    for (unsigned j = 0; j < 4; j++) {
        vst1q_u64(a->sum + 2u * j, sum[j]);
        vst1q_u64(a->prod + 2u * j, prod[j]);
    }
}
#elif defined(RSX_TEXHASH_SSE2)
static inline void rsx_texhash_blocks_vec(rsx_texhash_acc* a, const uint8_t* p, uint32_t n,
                                          uint64_t key)
{
    __m128i sum[4], prod[4];
    for (unsigned j = 0; j < 4; j++) {
        sum[j] = _mm_loadu_si128((const __m128i*)(a->sum + 2u * j));
        prod[j] = _mm_loadu_si128((const __m128i*)(a->prod + 2u * j));
    }
    for (uint32_t s = 0; s < n; s++, p += RSX_TEXHASH_STRIPE, key += RSX_TEXHASH_STEP) {
        const __m128i kv = _mm_set1_epi64x((long long)key);
        for (unsigned j = 0; j < 4; j++) {
            const __m128i v = _mm_loadu_si128((const __m128i*)(p + 16u * j));
            const __m128i k = _mm_xor_si128(v, kv);
            sum[j] = _mm_add_epi64(sum[j], v);
            prod[j] = _mm_add_epi64(prod[j], _mm_mul_epu32(k, _mm_srli_epi64(k, 32)));
        }
    }
    for (unsigned j = 0; j < 4; j++) {
        _mm_storeu_si128((__m128i*)(a->sum + 2u * j), sum[j]);
        _mm_storeu_si128((__m128i*)(a->prod + 2u * j), prod[j]);
    }
}
#else
#  define rsx_texhash_blocks_vec rsx_texhash_blocks_scalar
#endif

static inline uint64_t rsx_texhash_fold(const rsx_texhash_acc* a, uint64_t span)
{
    uint64_t h = span * 0x9E3779B185EBCA87ull;
    for (unsigned i = 0; i < 8; i++) {
        h = (h ^ a->prod[i]) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 32;
        h = (h ^ a->sum[i]) * 0x165667B19E3779F9ull;
        h ^= h >> 29;
    }
    return h;
}

static inline void rsx_texhash_init(rsx_texhash_acc* a)
{
    static const uint64_t seed[8] = {
        0x9E3779B185EBCA87ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x85EBCA77C2B2AE63ull,
        0x27D4EB2F165667C5ull, 0xFF51AFD7ED558CCDull, 0xC4CEB9FE1A85EC53ull, 0x94D049BB133111EBull,
    };
    for (unsigned i = 0; i < 8; i++) { a->sum[i] = ~seed[i]; a->prod[i] = seed[i]; }
}

static inline uint64_t rsx_texhash_with(const uint8_t* src, uint32_t span, int vector)
{
    rsx_texhash_acc a;
    rsx_texhash_init(&a);
    const uint32_t n = span / RSX_TEXHASH_STRIPE;
    if (vector) rsx_texhash_blocks_vec(&a, src, n, RSX_TEXHASH_KEY0);
    else        rsx_texhash_blocks_scalar(&a, src, n, RSX_TEXHASH_KEY0);
    const uint32_t tail = span - n * RSX_TEXHASH_STRIPE;
    if (tail || !span) {
        uint8_t pad[RSX_TEXHASH_STRIPE] = {0};
        if (tail) memcpy(pad, src + (size_t)n * RSX_TEXHASH_STRIPE, tail);
        rsx_texhash_blocks_scalar(&a, pad, 1, RSX_TEXHASH_KEY0 + (uint64_t)n * RSX_TEXHASH_STEP);
    }
    return rsx_texhash_fold(&a, span);
}

/* The engine's hash of span bytes at src. */
static inline uint64_t rsx_texhash(const uint8_t* src, uint32_t span)
{
    return rsx_texhash_with(src, span, 1);
}

/* The same value through the plain-C stripe loop, for the test that the vector paths agree. */
static inline uint64_t rsx_texhash_reference(const uint8_t* src, uint32_t span)
{
    return rsx_texhash_with(src, span, 0);
}

#endif
