/* Native host reference for synlib, written from the specification in
 * syn_spec.h and nothing else (it is deliberately not a transliteration of
 * syn_c.c). Guest memory goes through the toolkit's big-endian accessors, the
 * way every library in libs/ does it. */
#include "ref_native.h"
#include "syn_spec.h"
#include "ppu/ppu_memory.h"
#include <math.h>

static int32_t err(uint32_t code) { return (int32_t)code; }

int32_t synObjInit(uint32_t obj, uint32_t name, uint32_t flags, uint32_t capacity)
{
    if (!obj)                              return err(SYN_ENULL);
    if (obj % 16)                          return err(SYN_EALIGN);
    if (flags & ~(uint32_t)SYN_FLAGS_MASK) return err(SYN_EINVAL);
    if (capacity < 1 || capacity > SYN_CAP_MAX) return err(SYN_ERANGE);

    vm_write32(obj + SYN_O_MAGIC, SYN_MAGIC);
    vm_write16(obj + SYN_O_VERSION, SYN_VERSION);
    vm_write16(obj + SYN_O_FLAGS, (uint16_t)flags);
    vm_write32(obj + SYN_O_CAPACITY, capacity);
    vm_write32(obj + SYN_O_COUNT, 0);
    vm_write64(obj + SYN_O_STAMP, ((uint64_t)capacity << 32) | (uint32_t)(flags ^ 0xFFFF0000u));

    uint8_t field[SYN_NAME_LEN] = {0};     /* strncpy(field, name, 23); field[23] = 0 */
    if (name) {
        for (unsigned i = 0; i < SYN_NAME_LEN - 1; ++i) {
            field[i] = vm_read8(name + i);
            if (!field[i]) break;
        }
    }
    for (unsigned i = 0; i < SYN_NAME_LEN; ++i) vm_write8(obj + SYN_O_NAME + i, field[i]);

    synHostMemset(obj + SYN_O_PACK_LO, 0, SYN_O_SIZE - SYN_O_PACK_LO);
    return 0;
}

int32_t synObjCommand(uint32_t obj, uint32_t cmd, uint32_t arg)
{
    if (!obj)       return err(SYN_ENULL);
    if (obj % 16)   return err(SYN_EALIGN);
    if (vm_read32(obj + SYN_O_MAGIC) != SYN_MAGIC) return err(SYN_ESTATE);

    const uint32_t count = vm_read32(obj + SYN_O_COUNT);
    const uint64_t stamp = vm_read64(obj + SYN_O_STAMP);
    switch (cmd) {
    case SYN_CMD_QUERY:
        return (int32_t)count;
    case SYN_CMD_ADD: {
        const uint32_t n = count + arg;
        if (n < count || n > vm_read32(obj + SYN_O_CAPACITY)) return err(SYN_ERANGE);
        vm_write32(obj + SYN_O_COUNT, n);
        vm_write64(obj + SYN_O_STAMP, stamp + arg);
        return (int32_t)n;
    }
    case SYN_CMD_SUB:
        if (arg > count) return err(SYN_ERANGE);
        vm_write32(obj + SYN_O_COUNT, count - arg);
        return (int32_t)(count - arg);
    case SYN_CMD_CLEAR:
        vm_write32(obj + SYN_O_COUNT, 0);
        vm_write64(obj + SYN_O_STAMP, 0);
        return 0;
    case SYN_CMD_SETFLAGS: {
        if (arg & ~(uint32_t)SYN_FLAGS_MASK) return err(SYN_EINVAL);
        const uint16_t old = vm_read16(obj + SYN_O_FLAGS);
        vm_write16(obj + SYN_O_FLAGS, (uint16_t)arg);
        return old;
    }
    case SYN_CMD_STAMPHI:
        return (int32_t)(uint32_t)(stamp >> 32);
    case SYN_CMD_PACK:
        vm_write16(obj + SYN_O_PACK_LO, (uint16_t)(arg & 0xFFFFu));
        vm_write16(obj + SYN_O_PACK_HI, (uint16_t)(arg >> 16));
        vm_write32(obj + SYN_O_PACK_MUL, (uint32_t)((uint64_t)arg * (uint32_t)SYN_PACK_K));
        vm_write64(obj + SYN_O_PACK_WIDE, (uint64_t)arg * (uint32_t)SYN_PACK_K);
        return 0;
    case SYN_CMD_ROTATE: {
        const unsigned r = arg & 63u;
        vm_write64(obj + SYN_O_STAMP, r ? (stamp << r) | (stamp >> (64u - r)) : stamp);
        return 0;
    }
    default:
        return err(SYN_EINVAL);
    }
}

int32_t synObjPush(uint32_t obj, uint32_t n)
{
    const int32_t r = synObjCommand(obj, SYN_CMD_ADD, n);
    if (r < 0) return r;
    vm_write32(obj + SYN_O_LAST_PUSH, n);
    vm_write32(obj + SYN_O_PUSH_SEQ, vm_read32(obj + SYN_O_PUSH_SEQ) + 1);
    return r;
}

int32_t synBufChecksum(uint32_t buf, uint32_t words, uint32_t out)
{
    if (!buf || !out)            return err(SYN_ENULL);
    if (buf % 4 || out % 8)      return err(SYN_EALIGN);
    if (words > SYN_WORDS_MAX)   return err(SYN_ERANGE);

    uint64_t sum = 0;
    uint32_t x = 0x12345678u;
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t w = vm_read32(buf + 4 * i);
        sum += w;
        x = ((x << 5) | (x >> 27)) ^ w;
    }
    vm_write64(out, sum);
    vm_write32(out + 8, x);
    vm_write32(out + 12, words);
    return 0;
}

uint64_t synBits(uint64_t a, uint64_t b, uint32_t c)
{
    uint64_t x = a * b + c;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    const uint64_t q = x / (b | 1u);
    const uint64_t r = x % (a | 1u);
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
    {
        const uint32_t v = (uint32_t)a; const unsigned n = (unsigned)(b & 31);
        h ^= n ? (v << n) | (v >> (32 - n)) : v;
    }
    {
        const uint32_t m = 0x00FFFF00u;
        h += (uint32_t)(((uint32_t)a & ~m) | (((uint32_t)b << 7) & m));
    }
    h ^= (uint64_t)(a < b) | (uint64_t)((int64_t)a < (int64_t)b) << 1 | (uint64_t)(a == b) << 2;
    h += (uint64_t)((int64_t)a / (int64_t)((b & 0x7FFF) | 1));
    h ^= (uint32_t)((int32_t)c / (int32_t)((a & 0x7F) | 1));
    h += (uint32_t)((uint32_t)a / (uint32_t)((c & 0xFFFF) | 1));
    h ^= (uint32_t)((uint32_t)a * (uint32_t)b);
    h += (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
    h ^= (uint32_t)(((uint64_t)(uint32_t)a * (uint32_t)b) >> 32);
    return h;
}

int32_t synFpu(uint32_t in, uint32_t out)
{
    if (!in || !out)         return err(SYN_ENULL);
    if ((in | out) % 8)      return err(SYN_EALIGN);
    const double a = vm_read_f64(in), b = vm_read_f64(in + 8);
    const float  c = vm_read_f32(in + 16), d = vm_read_f32(in + 20);
    if (b == 0.0)            return err(SYN_ERANGE);
    if (fabs(a) >= 2147483648.0) return err(SYN_ERANGE);

    const double prod = a * b;            /* separate statements: no contraction into an fma */
    const double sum = a + b, diff = a - b, quot = a / b;
    const double fused = fma(a, b, (double)c);
    const float  prodf = (float)prod, adds = c + d, muls = c * d;
    vm_write_f64(out + SYN_F_SUM, sum);
    vm_write_f64(out + SYN_F_DIFF, diff);
    vm_write_f64(out + SYN_F_PROD, prod);
    vm_write_f64(out + SYN_F_QUOT, quot);
    vm_write_f64(out + SYN_F_FMA, fused);
    vm_write_f32(out + SYN_F_PRODF, prodf);
    vm_write32(out + SYN_F_TRUNC, (uint32_t)(int32_t)a);
    vm_write32(out + SYN_F_CMP, (uint32_t)(a < b) | (uint32_t)(a == b) << 1 | (uint32_t)(a > b) << 2);
    vm_write_f32(out + SYN_F_ADDS, adds);
    vm_write_f32(out + SYN_F_MULS, muls);
    vm_write_f64(out + SYN_F_ABS, fabs(a));
    vm_write_f64(out + SYN_F_NEGB, -b);
    return 0;
}

void synHostMemset(uint32_t dst, int32_t value, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) vm_write8(dst + i, (uint8_t)value);
}
