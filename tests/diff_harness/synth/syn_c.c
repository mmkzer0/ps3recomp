/* synlib in C, for powerpc64-linux-gnu-gcc -m64 -O2 -ffreestanding (see syn_spec.h
 * for the specification). Guest memory is big-endian and so is the target, so
 * the accessors are plain loads and stores; a guest pointer is a 32-bit value
 * zero-extended into a 64-bit register.
 *
 * Built with -DSYN_BUG_* (syn_c_bug.c) this file carries three deliberate bugs:
 *   SYN_BUG_ORDER   synObjInit tests the capacity before the flags
 *   SYN_BUG_LIMIT   synObjInit rejects capacity == 0x1000 (off by one)
 *   SYN_BUG_WIDE    PACK stores a 32-bit product where the spec says 64-bit
 */
#include <stdint.h>
#include "syn_spec.h"

typedef uint8_t  __attribute__((may_alias)) a8;
typedef uint16_t __attribute__((may_alias)) a16;
typedef uint32_t __attribute__((may_alias)) a32;
typedef uint64_t __attribute__((may_alias)) a64;

#define G8(ea)  (*(a8*)(uintptr_t)(ea))
#define G16(ea) (*(a16*)(uintptr_t)(ea))
#define G32(ea) (*(a32*)(uintptr_t)(ea))
#define G64(ea) (*(a64*)(uintptr_t)(ea))

extern void synHostMemset(uint32_t dst, int32_t value, uint32_t n);

#define EXPORT __attribute__((used))

EXPORT int32_t synObjInit(uint32_t obj, uint32_t name, uint32_t flags, uint32_t capacity)
{
    if (obj == 0) return (int32_t)SYN_ENULL;
    if (obj & 0xF) return (int32_t)SYN_EALIGN;
#ifdef SYN_BUG_ORDER
    if (capacity == 0 || capacity > SYN_CAP_MAX) return (int32_t)SYN_ERANGE;
    if (flags & ~(uint32_t)SYN_FLAGS_MASK) return (int32_t)SYN_EINVAL;
#else
    if (flags & ~(uint32_t)SYN_FLAGS_MASK) return (int32_t)SYN_EINVAL;
    if (capacity == 0 || capacity > SYN_CAP_MAX) return (int32_t)SYN_ERANGE;
#endif
#ifdef SYN_BUG_LIMIT
    if (capacity >= SYN_CAP_MAX) return (int32_t)SYN_ERANGE;
#endif

    G32(obj + SYN_O_MAGIC) = SYN_MAGIC;
    G16(obj + SYN_O_VERSION) = SYN_VERSION;
    G16(obj + SYN_O_FLAGS) = (uint16_t)flags;
    G32(obj + SYN_O_CAPACITY) = capacity;
    G32(obj + SYN_O_COUNT) = 0;
    G64(obj + SYN_O_STAMP) = ((uint64_t)capacity << 32) | (uint32_t)(flags ^ 0xFFFF0000u);

    uint32_t i = 0;
    if (name) {
        for (; i < SYN_NAME_LEN - 1; ++i) {
            uint8_t ch = G8(name + i);
            if (ch == 0) break;
            G8(obj + SYN_O_NAME + i) = ch;
        }
    }
    for (; i < SYN_NAME_LEN; ++i) G8(obj + SYN_O_NAME + i) = 0;

    synHostMemset(obj + SYN_O_PACK_LO, 0, SYN_O_SIZE - SYN_O_PACK_LO);
    return SYN_OK;
}

EXPORT __attribute__((noinline)) int32_t synObjCommand(uint32_t obj, uint32_t cmd, uint32_t arg)
{
    if (obj == 0) return (int32_t)SYN_ENULL;
    if (obj & 0xF) return (int32_t)SYN_EALIGN;
    if (G32(obj + SYN_O_MAGIC) != SYN_MAGIC) return (int32_t)SYN_ESTATE;

    switch (cmd) {
    case SYN_CMD_QUERY:
        return (int32_t)G32(obj + SYN_O_COUNT);
    case SYN_CMD_ADD: {
        uint32_t cnt = G32(obj + SYN_O_COUNT);
        uint32_t n = cnt + arg;
        if (n < cnt || n > G32(obj + SYN_O_CAPACITY)) return (int32_t)SYN_ERANGE;
        G32(obj + SYN_O_COUNT) = n;
        G64(obj + SYN_O_STAMP) = G64(obj + SYN_O_STAMP) + arg;
        return (int32_t)n;
    }
    case SYN_CMD_SUB: {
        uint32_t cnt = G32(obj + SYN_O_COUNT);
        if (arg > cnt) return (int32_t)SYN_ERANGE;
        G32(obj + SYN_O_COUNT) = cnt - arg;
        return (int32_t)(cnt - arg);
    }
    case SYN_CMD_CLEAR:
        G32(obj + SYN_O_COUNT) = 0;
        G64(obj + SYN_O_STAMP) = 0;
        return SYN_OK;
    case SYN_CMD_SETFLAGS: {
        if (arg & ~(uint32_t)SYN_FLAGS_MASK) return (int32_t)SYN_EINVAL;
        uint16_t old = G16(obj + SYN_O_FLAGS);
        G16(obj + SYN_O_FLAGS) = (uint16_t)arg;
        return old;
    }
    case SYN_CMD_STAMPHI:
        return (int32_t)(uint32_t)(G64(obj + SYN_O_STAMP) >> 32);
    case SYN_CMD_PACK:
        G16(obj + SYN_O_PACK_LO) = (uint16_t)(arg & 0xFFFF);
        G16(obj + SYN_O_PACK_HI) = (uint16_t)(arg >> 16);
        G32(obj + SYN_O_PACK_MUL) = arg * (uint32_t)SYN_PACK_K;
#ifdef SYN_BUG_WIDE
        G64(obj + SYN_O_PACK_WIDE) = (uint64_t)(uint32_t)(arg * (uint32_t)SYN_PACK_K);
#else
        G64(obj + SYN_O_PACK_WIDE) = (uint64_t)arg * (uint64_t)SYN_PACK_K;
#endif
        return SYN_OK;
    case SYN_CMD_ROTATE: {
        uint64_t s = G64(obj + SYN_O_STAMP);
        unsigned r = arg & 63;
        G64(obj + SYN_O_STAMP) = r ? (s << r) | (s >> (64 - r)) : s;
        return SYN_OK;
    }
    default:
        return (int32_t)SYN_EINVAL;
    }
}

EXPORT int32_t synObjPush(uint32_t obj, uint32_t n)
{
    int32_t r = synObjCommand(obj, SYN_CMD_ADD, n);
    if (r < 0) return r;
    G32(obj + SYN_O_LAST_PUSH) = n;
    G32(obj + SYN_O_PUSH_SEQ) = G32(obj + SYN_O_PUSH_SEQ) + 1;
    return r;
}

EXPORT int32_t synBufChecksum(uint32_t buf, uint32_t words, uint32_t out)
{
    if (buf == 0 || out == 0) return (int32_t)SYN_ENULL;
    if ((buf & 3) || (out & 7)) return (int32_t)SYN_EALIGN;
    if (words > SYN_WORDS_MAX) return (int32_t)SYN_ERANGE;

    uint64_t sum = 0;
    uint32_t x = 0x12345678u;
    for (uint32_t i = 0; i < words; ++i) {
        uint32_t w = G32(buf + 4 * i);
        sum += w;
        x = ((x << 5) | (x >> 27)) ^ w;
    }
    G64(out) = sum;
    G32(out + 8) = x;
    G32(out + 12) = words;
    return SYN_OK;
}

EXPORT uint64_t synBits(uint64_t a, uint64_t b, uint32_t c)
{
    uint64_t x = a * b + c;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    uint64_t q = x / (b | 1);
    uint64_t r = x % (a | 1);
    uint64_t h = q ^ (r << 3) ^ (uint64_t)((int64_t)x >> 7);

    h ^= (uint64_t)(a ? __builtin_clzll(a) : 64) << 8;
    h += __builtin_bswap64(b);
    h ^= __builtin_bswap32(c);
    h ^= (uint64_t)(int64_t)(int8_t)a;
    h += (uint64_t)(int64_t)(int16_t)b;
    h ^= (uint64_t)(int64_t)(int32_t)c;
    h += (uint64_t)(((unsigned __int128)a * b) >> 64);
    h ^= (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64);
    h ^= (uint64_t)((int64_t)a >> (b & 63));
    h += (uint32_t)((int32_t)a >> (c & 31));
    uint32_t rot = (uint32_t)b & 31;
    uint32_t a32 = (uint32_t)a;
    h ^= rot ? (a32 << rot) | (a32 >> (32 - rot)) : a32;
    const uint32_t m = 0x00FFFF00u;
    h += (uint32_t)((a32 & ~m) | (((uint32_t)b << 7) & m));
    h ^= (uint64_t)(a < b) | (uint64_t)((int64_t)a < (int64_t)b) << 1 | (uint64_t)(a == b) << 2;
    h += (uint64_t)((int64_t)a / (int64_t)((b & 0x7FFF) | 1));
    h ^= (uint32_t)((int32_t)c / (int32_t)((a & 0x7F) | 1));
    h += (uint32_t)((uint32_t)a / (uint32_t)((c & 0xFFFF) | 1));
    h ^= (uint32_t)((uint32_t)a * (uint32_t)b);
    h += (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
    h ^= (uint32_t)(((uint64_t)(uint32_t)a * (uint32_t)b) >> 32);
    return h;
}

typedef double __attribute__((may_alias)) af64;
typedef float  __attribute__((may_alias)) af32;
#define GF64(ea) (*(af64*)(uintptr_t)(ea))
#define GF32(ea) (*(af32*)(uintptr_t)(ea))

EXPORT int32_t synFpu(uint32_t in, uint32_t out)
{
    if (in == 0 || out == 0) return (int32_t)SYN_ENULL;
    if ((in | out) & 7) return (int32_t)SYN_EALIGN;
    double a = GF64(in), b = GF64(in + 8);
    float c = GF32(in + 16), d = GF32(in + 20);
    if (b == 0.0) return (int32_t)SYN_ERANGE;
    if (__builtin_fabs(a) >= 2147483648.0) return (int32_t)SYN_ERANGE;

    GF64(out + SYN_F_SUM) = a + b;
    GF64(out + SYN_F_DIFF) = a - b;
    GF64(out + SYN_F_PROD) = a * b;
    GF64(out + SYN_F_QUOT) = a / b;
    GF64(out + SYN_F_FMA) = __builtin_fma(a, b, (double)c);
    GF32(out + SYN_F_PRODF) = (float)(a * b);
    G32(out + SYN_F_TRUNC) = (uint32_t)(int32_t)a;
    G32(out + SYN_F_CMP) = (uint32_t)(a < b) | (uint32_t)(a == b) << 1 | (uint32_t)(a > b) << 2;
    GF32(out + SYN_F_ADDS) = c + d;
    GF32(out + SYN_F_MULS) = c * d;
    GF64(out + SYN_F_ABS) = __builtin_fabs(a);
    GF64(out + SYN_F_NEGB) = -b;
    return SYN_OK;
}
