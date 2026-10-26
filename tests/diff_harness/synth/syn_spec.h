/* The "synlib" specification: a small synthetic PPU library written for the
 * firmware-free test of the call-level differential harness (and, with it, of
 * the lifter). It is OUR code, written from the spec below; nothing in it comes
 * from, or resembles, a firmware module.
 *
 * Four implementations of this spec exist: syn_c.c, syn_cpp.cpp, syn_asm.S
 * (cross-compiled to PPC64 BE and lifted) and ref_native.c (host C, plays the
 * HLE side). syn_c_bug.c is syn_c.c with three deliberate bugs. This header is
 * shared by all of them, so it holds only constants (it is also read by the
 * assembler).
 *
 * Conventions (the PS3 ones): guest pointers are 32-bit effective addresses
 * passed zero-extended in the 64-bit argument registers; guest memory is
 * big-endian; an s32 result is sign-extended into r3.
 *
 * Object layout (0x50 bytes, 16-byte aligned):
 *   +0x00 u32 magic        SYN_MAGIC
 *   +0x04 u16 version      0x0102
 *   +0x06 u16 flags
 *   +0x08 u32 capacity
 *   +0x0C u32 count
 *   +0x10 u64 stamp
 *   +0x18 char name[24]    NUL padded, byte 23 always 0
 *   +0x30 u16 pack_lo      \
 *   +0x32 u16 pack_hi       | written by synObjCommand(PACK) only
 *   +0x34 u32 pack_mul      |
 *   +0x38 u64 pack_wide    /
 *   +0x40 u32 last_push    \ written by synObjPush only
 *   +0x44 u32 push_seq     /
 *   +0x48 u64 reserved
 *
 * Exports (s32 results unless noted; error codes below):
 *
 * s32 synObjInit(u32 obj, u32 name, u32 flags, u32 capacity)
 *   checks, in this order:
 *     obj == 0                          -> SYN_ENULL
 *     obj & 15                          -> SYN_EALIGN
 *     flags & ~0x3F                     -> SYN_EINVAL
 *     capacity == 0 || capacity > 0x1000 -> SYN_ERANGE
 *   then writes: magic, version 0x0102, flags (u16), capacity, count 0,
 *   stamp = (u64)capacity << 32 | (u32)(flags ^ 0xFFFF0000), name = strncpy
 *   semantics into the 24-byte field (at most 23 characters are copied, the
 *   rest is zero padded; name == 0 gives an all-zero field), and zeroes
 *   +0x30..+0x4F by calling the imported helper synHostMemset. Returns SYN_OK.
 *   Bytes outside +0x00..+0x4F are never touched; +0x48..+0x4F end up zero.
 *
 * s32 synObjCommand(u32 obj, u32 cmd, u32 arg)
 *   checks: obj == 0 -> ENULL; obj & 15 -> EALIGN; magic != SYN_MAGIC -> ESTATE;
 *   then switch (cmd):
 *     0 QUERY     return count
 *     1 ADD       n = count + arg (u32 wrap); n < count || n > capacity -> ERANGE
 *                 (nothing written); else count = n, stamp += arg (64-bit);
 *                 return n
 *     2 SUB       arg > count -> ERANGE; else count -= arg; return count
 *     3 CLEAR     count = 0, stamp = 0; return OK
 *     4 SETFLAGS  arg & ~0x3F -> EINVAL; else flags = arg (u16); return the old flags
 *     5 STAMPHI   return (u32)(stamp >> 32)
 *     6 PACK      pack_lo = arg & 0xFFFF, pack_hi = arg >> 16,
 *                 pack_mul = (u32)(arg * 0x9E3779B1),
 *                 pack_wide = (u64)arg * 0x9E3779B1 (full 64-bit product); return OK
 *     7 ROTATE    stamp = rotl64(stamp, arg & 63); return OK
 *     other       -> EINVAL
 *
 * s32 synObjPush(u32 obj, u32 n)
 *   r = synObjCommand(obj, ADD, n). r < 0 -> return r (nothing else written).
 *   Otherwise last_push = n, push_seq += 1, return r. (Calls another export.)
 *
 * s32 synBufChecksum(u32 buf, u32 words, u32 out)
 *   checks: buf == 0 || out == 0 -> ENULL; (buf & 3) || (out & 7) -> EALIGN;
 *   words > 0x4000 -> ERANGE.
 *   Then, over the big-endian u32 words at buf: sum (u64) += w;
 *   x (u32, starts 0x12345678) = rotl32(x, 5) ^ w. Writes u64 sum at out,
 *   u32 x at out+8, u32 words at out+12. Returns OK.
 *
 * u64 synBits(u64 a, u64 b, u32 c)          (a pure register function: 64-bit math, bit ops)
 *   x = a * b + c; x ^= x >> 29; x *= 0xBF58476D1CE4E5B9;
 *   q = x / (b | 1); r = x % (a | 1);   (unsigned)
 *   h  = q ^ (r << 3) ^ (u64)((s64)x >> 7)
 *   h ^= (u64)(a ? clz64(a) : 64) << 8
 *   h += bswap64(b);   h ^= bswap32(c)
 *   h ^= (u64)(s64)(s8)a;  h += (u64)(s64)(s16)b;  h ^= (u64)(s64)(s32)c
 *   h += high 64 bits of the unsigned 128-bit product a*b
 *   h ^= high 64 bits of the signed 128-bit product (s64)a*(s64)b
 *   h ^= (u64)((s64)a >> (b & 63));   h += (u32)((s32)a >> (c & 31))
 *   h ^= rotl32((u32)a, b & 31)
 *   m = 0x00FFFF00; h += (u32)(((u32)a & ~m) | (((u32)b << 7) & m))
 *   h ^= (a < b) | ((s64)a < (s64)b) << 1 | (a == b) << 2
 *   h += (u64)((s64)a / (s64)((b & 0x7FFF) | 1))
 *   h ^= (u32)((s32)c / (s32)((a & 0x7F) | 1))
 *   h += (u32)((u32)a / (u32)((c & 0xFFFF) | 1))
 *   h ^= (u32)((u32)a * (u32)b)
 *   h += (u32)(((s64)(s32)a * (s64)(s32)b) >> 32)
 *   h ^= (u32)(((u64)(u32)a * (u32)b) >> 32)
 *   return h
 *
 * s32 synFpu(u32 in, u32 out)                (floating point; values big-endian in guest memory)
 *   in:  +0x00 f64 a, +0x08 f64 b, +0x10 f32 c, +0x14 f32 d
 *   checks: in == 0 || out == 0 -> ENULL; (in | out) & 7 -> EALIGN;
 *           b == 0.0 -> ERANGE; fabs(a) >= 2147483648.0 -> ERANGE
 *           (finite inputs only; NaN and denormals are not part of the contract)
 *   out: +0x00 f64 a+b   +0x08 f64 a-b   +0x10 f64 a*b   +0x18 f64 a/b
 *        +0x20 f64 fma(a, b, (f64)c)   (one rounding)
 *        +0x28 f32 (f32)(a*b)          +0x2C s32 (s32)a, truncated toward zero
 *        +0x30 u32 (a<b) | (a==b) << 1 | (a>b) << 2
 *        +0x34 f32 c+d (single)        +0x38 f32 c*d (single)
 *        +0x40 f64 fabs(a)             +0x48 f64 -b        (+0x3C is not written)
 *   Returns OK.
 *
 * Import (provided by the harness, not part of the library):
 *   void synHostMemset(u32 dst, s32 value, u32 n)
 */
#ifndef SYN_SPEC_H
#define SYN_SPEC_H

#define SYN_OK      0
#define SYN_EINVAL  0x80770001
#define SYN_EALIGN  0x80770002
#define SYN_ENULL   0x80770003
#define SYN_ERANGE  0x80770004
#define SYN_ESTATE  0x80770005

#define SYN_MAGIC       0x53594E31
#define SYN_VERSION     0x0102
#define SYN_FLAGS_MASK  0x3F
#define SYN_CAP_MAX     0x1000
#define SYN_WORDS_MAX   0x4000

#define SYN_O_MAGIC     0x00
#define SYN_O_VERSION   0x04
#define SYN_O_FLAGS     0x06
#define SYN_O_CAPACITY  0x08
#define SYN_O_COUNT     0x0C
#define SYN_O_STAMP     0x10
#define SYN_O_NAME      0x18
#define SYN_O_PACK_LO   0x30
#define SYN_O_PACK_HI   0x32
#define SYN_O_PACK_MUL  0x34
#define SYN_O_PACK_WIDE 0x38
#define SYN_O_LAST_PUSH 0x40
#define SYN_O_PUSH_SEQ  0x44
#define SYN_O_SIZE      0x50

#define SYN_NAME_LEN    24

#define SYN_CMD_QUERY    0
#define SYN_CMD_ADD      1
#define SYN_CMD_SUB      2
#define SYN_CMD_CLEAR    3
#define SYN_CMD_SETFLAGS 4
#define SYN_CMD_STAMPHI  5
#define SYN_CMD_PACK     6
#define SYN_CMD_ROTATE   7

#define SYN_PACK_K       0x9E3779B1

#define SYN_F_SUM   0x00
#define SYN_F_DIFF  0x08
#define SYN_F_PROD  0x10
#define SYN_F_QUOT  0x18
#define SYN_F_FMA   0x20
#define SYN_F_PRODF 0x28
#define SYN_F_TRUNC 0x2C
#define SYN_F_CMP   0x30
#define SYN_F_ADDS  0x34
#define SYN_F_MULS  0x38
#define SYN_F_ABS   0x40
#define SYN_F_NEGB  0x48
#define SYN_F_OUT_SIZE 0x50

#endif
