/* synlib in C++ (syn_spec.h), for powerpc64-linux-gnu-g++ -m64 -O2 -ffreestanding
 * -fno-exceptions -fno-rtti. The point of this variant is the code a C++ compiler
 * makes of idiomatic C++ -- templates, constexpr tables, enum class, a small
 * wrapper class, inline member functions -- behind extern "C" exports. */
#include <stdint.h>
#include "syn_spec.h"

namespace {

typedef uint8_t  __attribute__((may_alias)) a8;
typedef uint16_t __attribute__((may_alias)) a16;
typedef uint32_t __attribute__((may_alias)) a32;
typedef uint64_t __attribute__((may_alias)) a64;

/* typed big-endian guest memory access (the target is big-endian) */
template <typename T> struct Raw;
template <> struct Raw<uint8_t>  { typedef a8  type; };
template <> struct Raw<uint16_t> { typedef a16 type; };
template <> struct Raw<uint32_t> { typedef a32 type; };
template <> struct Raw<uint64_t> { typedef a64 type; };

template <typename T> inline T ld(uint32_t ea) { return *reinterpret_cast<typename Raw<T>::type*>(static_cast<uintptr_t>(ea)); }
template <typename T> inline void st(uint32_t ea, T v) { *reinterpret_cast<typename Raw<T>::type*>(static_cast<uintptr_t>(ea)) = v; }

typedef double __attribute__((may_alias)) af64;
typedef float  __attribute__((may_alias)) af32;
template <typename T> inline T ldf(uint32_t ea) { return *reinterpret_cast<T*>(static_cast<uintptr_t>(ea)); }
template <typename T> inline void stf(uint32_t ea, T v) { *reinterpret_cast<T*>(static_cast<uintptr_t>(ea)) = v; }

enum class Err : uint32_t {
    Invalid = SYN_EINVAL, Align = SYN_EALIGN, Null = SYN_ENULL, Range = SYN_ERANGE, State = SYN_ESTATE,
};
constexpr int32_t code(Err e) { return static_cast<int32_t>(static_cast<uint32_t>(e)); }

template <unsigned N> constexpr bool misaligned(uint32_t ea) { return (ea & (N - 1)) != 0; }
template <typename T> constexpr T rotl(T v, unsigned r)
{
    constexpr unsigned bits = sizeof(T) * 8;
    return r ? static_cast<T>((v << r) | (v >> (bits - r))) : v;
}

constexpr uint32_t kPackK = SYN_PACK_K;

/* a view of one object: field accessors, no state of its own */
class Obj {
public:
    explicit Obj(uint32_t ea) : ea_(ea) {}
    template <typename T> T get(uint32_t off) const { return ld<T>(ea_ + off); }
    template <typename T> void put(uint32_t off, T v) const { st<T>(ea_ + off, v); }
    uint32_t ea() const { return ea_; }
    bool valid() const { return get<uint32_t>(SYN_O_MAGIC) == SYN_MAGIC; }
private:
    uint32_t ea_;
};

} // namespace

extern "C" {

void synHostMemset(uint32_t dst, int32_t value, uint32_t n);

__attribute__((used))
int32_t synObjInit(uint32_t obj, uint32_t name, uint32_t flags, uint32_t capacity)
{
    if (obj == 0) return code(Err::Null);
    if (misaligned<16>(obj)) return code(Err::Align);
    if (flags & ~static_cast<uint32_t>(SYN_FLAGS_MASK)) return code(Err::Invalid);
    if (capacity == 0 || capacity > static_cast<uint32_t>(SYN_CAP_MAX)) return code(Err::Range);

    const Obj o(obj);
    o.put<uint32_t>(SYN_O_MAGIC, SYN_MAGIC);
    o.put<uint16_t>(SYN_O_VERSION, SYN_VERSION);
    o.put<uint16_t>(SYN_O_FLAGS, static_cast<uint16_t>(flags));
    o.put<uint32_t>(SYN_O_CAPACITY, capacity);
    o.put<uint32_t>(SYN_O_COUNT, 0);
    o.put<uint64_t>(SYN_O_STAMP, (static_cast<uint64_t>(capacity) << 32) | static_cast<uint32_t>(flags ^ 0xFFFF0000u));

    uint32_t i = 0;
    if (name != 0) {
        while (i < SYN_NAME_LEN - 1) {
            const uint8_t ch = ld<uint8_t>(name + i);
            if (ch == 0) break;
            o.put<uint8_t>(SYN_O_NAME + i, ch);
            ++i;
        }
    }
    while (i < SYN_NAME_LEN) o.put<uint8_t>(SYN_O_NAME + i++, 0);

    synHostMemset(obj + SYN_O_PACK_LO, 0, SYN_O_SIZE - SYN_O_PACK_LO);
    return SYN_OK;
}

__attribute__((used, noinline))
int32_t synObjCommand(uint32_t obj, uint32_t cmd, uint32_t arg)
{
    if (obj == 0) return code(Err::Null);
    if (misaligned<16>(obj)) return code(Err::Align);
    const Obj o(obj);
    if (!o.valid()) return code(Err::State);

    switch (static_cast<unsigned>(cmd)) {
    case SYN_CMD_QUERY:
        return static_cast<int32_t>(o.get<uint32_t>(SYN_O_COUNT));
    case SYN_CMD_ADD: {
        const uint32_t cnt = o.get<uint32_t>(SYN_O_COUNT);
        const uint32_t n = cnt + arg;
        if (n < cnt || n > o.get<uint32_t>(SYN_O_CAPACITY)) return code(Err::Range);
        o.put<uint32_t>(SYN_O_COUNT, n);
        o.put<uint64_t>(SYN_O_STAMP, o.get<uint64_t>(SYN_O_STAMP) + arg);
        return static_cast<int32_t>(n);
    }
    case SYN_CMD_SUB: {
        const uint32_t cnt = o.get<uint32_t>(SYN_O_COUNT);
        if (arg > cnt) return code(Err::Range);
        o.put<uint32_t>(SYN_O_COUNT, cnt - arg);
        return static_cast<int32_t>(cnt - arg);
    }
    case SYN_CMD_CLEAR:
        o.put<uint32_t>(SYN_O_COUNT, 0);
        o.put<uint64_t>(SYN_O_STAMP, 0);
        return SYN_OK;
    case SYN_CMD_SETFLAGS: {
        if (arg & ~static_cast<uint32_t>(SYN_FLAGS_MASK)) return code(Err::Invalid);
        const uint16_t old = o.get<uint16_t>(SYN_O_FLAGS);
        o.put<uint16_t>(SYN_O_FLAGS, static_cast<uint16_t>(arg));
        return old;
    }
    case SYN_CMD_STAMPHI:
        return static_cast<int32_t>(static_cast<uint32_t>(o.get<uint64_t>(SYN_O_STAMP) >> 32));
    case SYN_CMD_PACK:
        o.put<uint16_t>(SYN_O_PACK_LO, static_cast<uint16_t>(arg & 0xFFFF));
        o.put<uint16_t>(SYN_O_PACK_HI, static_cast<uint16_t>(arg >> 16));
        o.put<uint32_t>(SYN_O_PACK_MUL, arg * kPackK);
        o.put<uint64_t>(SYN_O_PACK_WIDE, static_cast<uint64_t>(arg) * kPackK);
        return SYN_OK;
    case SYN_CMD_ROTATE:
        o.put<uint64_t>(SYN_O_STAMP, rotl<uint64_t>(o.get<uint64_t>(SYN_O_STAMP), arg & 63));
        return SYN_OK;
    default:
        return code(Err::Invalid);
    }
}

__attribute__((used))
int32_t synObjPush(uint32_t obj, uint32_t n)
{
    const int32_t r = synObjCommand(obj, SYN_CMD_ADD, n);
    if (r < 0) return r;
    const Obj o(obj);
    o.put<uint32_t>(SYN_O_LAST_PUSH, n);
    o.put<uint32_t>(SYN_O_PUSH_SEQ, o.get<uint32_t>(SYN_O_PUSH_SEQ) + 1);
    return r;
}

__attribute__((used))
int32_t synBufChecksum(uint32_t buf, uint32_t words, uint32_t out)
{
    if (buf == 0 || out == 0) return code(Err::Null);
    if (misaligned<4>(buf) || misaligned<8>(out)) return code(Err::Align);
    if (words > static_cast<uint32_t>(SYN_WORDS_MAX)) return code(Err::Range);

    uint64_t sum = 0;
    uint32_t x = 0x12345678u;
    for (uint32_t i = 0; i < words; ++i) {
        const uint32_t w = ld<uint32_t>(buf + 4 * i);
        sum += w;
        x = rotl<uint32_t>(x, 5) ^ w;
    }
    st<uint64_t>(out, sum);
    st<uint32_t>(out + 8, x);
    st<uint32_t>(out + 12, words);
    return SYN_OK;
}

__attribute__((used))
uint64_t synBits(uint64_t a, uint64_t b, uint32_t c)
{
    constexpr uint64_t kMul = 0xBF58476D1CE4E5B9ull;
    uint64_t x = a * b + c;
    x ^= x >> 29;
    x *= kMul;
    const uint64_t q = x / (b | 1);
    const uint64_t r = x % (a | 1);
    uint64_t h = q ^ (r << 3) ^ static_cast<uint64_t>(static_cast<int64_t>(x) >> 7);

    h ^= static_cast<uint64_t>(a ? __builtin_clzll(a) : 64) << 8;
    h += __builtin_bswap64(b);
    h ^= __builtin_bswap32(c);
    h ^= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(a)));
    h += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(b)));
    h ^= static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(c)));
    h += static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >> 64);
    h ^= static_cast<uint64_t>((static_cast<__int128>(static_cast<int64_t>(a)) * static_cast<int64_t>(b)) >> 64);
    h ^= static_cast<uint64_t>(static_cast<int64_t>(a) >> (b & 63));
    h += static_cast<uint32_t>(static_cast<int32_t>(a) >> (c & 31));
    h ^= rotl<uint32_t>(static_cast<uint32_t>(a), static_cast<unsigned>(b) & 31);
    constexpr uint32_t m = 0x00FFFF00u;
    h += static_cast<uint32_t>((static_cast<uint32_t>(a) & ~m) | ((static_cast<uint32_t>(b) << 7) & m));
    h ^= static_cast<uint64_t>(a < b) | static_cast<uint64_t>(static_cast<int64_t>(a) < static_cast<int64_t>(b)) << 1
       | static_cast<uint64_t>(a == b) << 2;
    h += static_cast<uint64_t>(static_cast<int64_t>(a) / static_cast<int64_t>((b & 0x7FFF) | 1));
    h ^= static_cast<uint32_t>(static_cast<int32_t>(c) / static_cast<int32_t>((a & 0x7F) | 1));
    h += static_cast<uint32_t>(static_cast<uint32_t>(a) / static_cast<uint32_t>((c & 0xFFFF) | 1));
    h ^= static_cast<uint32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
    h += static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b))) >> 32);
    h ^= static_cast<uint32_t>((static_cast<uint64_t>(static_cast<uint32_t>(a)) * static_cast<uint32_t>(b)) >> 32);
    return h;
}

__attribute__((used))
int32_t synFpu(uint32_t in, uint32_t out)
{
    if (in == 0 || out == 0) return code(Err::Null);
    if (misaligned<8>(in | out)) return code(Err::Align);
    const double a = ldf<af64>(in), b = ldf<af64>(in + 8);
    const float c = ldf<af32>(in + 16), d = ldf<af32>(in + 20);
    if (b == 0.0) return code(Err::Range);
    if (__builtin_fabs(a) >= 2147483648.0) return code(Err::Range);

    stf<af64>(out + SYN_F_SUM, a + b);
    stf<af64>(out + SYN_F_DIFF, a - b);
    stf<af64>(out + SYN_F_PROD, a * b);
    stf<af64>(out + SYN_F_QUOT, a / b);
    stf<af64>(out + SYN_F_FMA, __builtin_fma(a, b, static_cast<double>(c)));
    stf<af32>(out + SYN_F_PRODF, static_cast<float>(a * b));
    st<uint32_t>(out + SYN_F_TRUNC, static_cast<uint32_t>(static_cast<int32_t>(a)));
    st<uint32_t>(out + SYN_F_CMP, static_cast<uint32_t>(a < b) | static_cast<uint32_t>(a == b) << 1 | static_cast<uint32_t>(a > b) << 2);
    stf<af32>(out + SYN_F_ADDS, c + d);
    stf<af32>(out + SYN_F_MULS, c * d);
    stf<af64>(out + SYN_F_ABS, __builtin_fabs(a));
    stf<af64>(out + SYN_F_NEGB, -b);
    return SYN_OK;
}

} // extern "C"
