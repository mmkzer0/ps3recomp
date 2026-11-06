#!/usr/bin/env python3
"""Independent oracle for test_spu_exhaustive: re-computes a sample of calls from the SPU ISA (big-endian quadword model,
exact rational arithmetic for single precision) and compares with the scalar build's dump. This is NOT derived from the
C helpers; it checks that the scalar reference itself (the *_ref code every other build is compared against) is right.

  oracle_exhaustive.py BIN [--n 20000] [--ops a,b] [--blocks 0]
BIN is the scalar test binary; for each (op, domain) of the model table, the first N calls of block 0 are dumped and
checked. Prints one line per op/domain with the number of calls checked and mismatches (first mismatch in detail).
"""
import argparse
import math
import struct
import subprocess
import sys
from fractions import Fraction as F

M32 = 0xFFFFFFFF; M128 = (1 << 128) - 1
def words(q): return [(q >> (96 - 32 * i)) & M32 for i in range(4)]
def unwords(w): return (w[0] << 96) | (w[1] << 64) | (w[2] << 32) | w[3]
def lanes(q, bits): n = 128 // bits; return [(q >> (128 - bits * (i + 1))) & ((1 << bits) - 1) for i in range(n)]
def unlanes(l, bits):
    q = 0
    for x in l: q = (q << bits) | (x & ((1 << bits) - 1))
    return q
def s(x, bits): return x - (1 << bits) if x >> (bits - 1) else x
MASK = {8: 0xFF, 16: 0xFFFF, 32: M32}
def lane2(bits, f):                       # binary lane op, f(x, y) -> int
    return lambda a, b, c, imm: unlanes([f(x, y) for x, y in zip(lanes(a, bits), lanes(b, bits))], bits)
def lane1(bits, f): return lambda a, b, c, imm: unlanes([f(x) for x in lanes(a, bits)], bits)
def lane3(bits, f): return lambda a, b, c, imm: unlanes([f(x, y, z) for x, y, z in zip(lanes(a, bits), lanes(b, bits), lanes(c, bits))], bits)
def laneI(bits, f): return lambda a, b, c, imm: unlanes([f(x, imm) for x in lanes(a, bits)], bits)
T = lambda c, bits: (MASK[bits] if c else 0)
OPS = {}
def reg(name, fn, doms): OPS[name] = (fn, doms)

# ---- logic / word arithmetic
reg("a",   lane2(32, lambda x, y: (x + y) & M32), "wpool2,wrand")
reg("sf",  lane2(32, lambda x, y: (y - x) & M32), "wpool2,wrand")
reg("and", lane2(32, lambda x, y: x & y), "wpool2"); reg("or", lane2(32, lambda x, y: x | y), "wpool2")
reg("xor", lane2(32, lambda x, y: x ^ y), "wpool2"); reg("nand", lane2(32, lambda x, y: ~(x & y) & M32), "wpool2")
reg("nor", lane2(32, lambda x, y: ~(x | y) & M32), "wpool2"); reg("andc", lane2(32, lambda x, y: x & ~y & M32), "wpool2")
reg("orc", lane2(32, lambda x, y: (x | ~y) & M32), "wpool2"); reg("eqv", lane2(32, lambda x, y: ~(x ^ y) & M32), "wpool2")
reg("ceq", lane2(32, lambda x, y: T(x == y, 32)), "wpool2,wrand"); reg("cgt", lane2(32, lambda x, y: T(s(x, 32) > s(y, 32), 32)), "wpool2,wrand")
reg("clgt", lane2(32, lambda x, y: T(x > y, 32)), "wpool2,wrand")
reg("cg", lane2(32, lambda x, y: (x + y) >> 32), "wpool2,wrand"); reg("bg", lane2(32, lambda x, y: int(y >= x)), "wpool2,wrand")
reg("addx", lane3(32, lambda x, y, t: (x + y + (t & 1)) & M32), "wrand"); reg("sfx", lane3(32, lambda x, y, t: (y + (~x & M32) + (t & 1)) & M32), "wrand")
reg("cgx", lane3(32, lambda x, y, t: (x + y + (t & 1)) >> 32), "wrand"); reg("bgx", lane3(32, lambda x, y, t: (y + (~x & M32) + (t & 1)) >> 32), "wrand")
reg("selb", lambda a, b, c, i: (b & c) | (a & ~c & M128), "wrand")
# ---- multiplies (word lanes, 16-bit halves)
reg("mpy",  lane2(32, lambda x, y: (s(x & 0xFFFF, 16) * s(y & 0xFFFF, 16)) & M32), "wpool2,h16ll")
reg("mpyu", lane2(32, lambda x, y: (x & 0xFFFF) * (y & 0xFFFF)), "wpool2,h16ll")
reg("mpyh", lane2(32, lambda x, y: ((s(x >> 16, 16) * s(y & 0xFFFF, 16)) << 16) & M32), "wpool2,h16hl")
reg("mpyhh", lane2(32, lambda x, y: (s(x >> 16, 16) * s(y >> 16, 16)) & M32), "wpool2,h16hh")
reg("mpyhhu", lane2(32, lambda x, y: (x >> 16) * (y >> 16)), "wpool2,h16hh")
reg("mpys", lane2(32, lambda x, y: ((s(x & 0xFFFF, 16) * s(y & 0xFFFF, 16)) >> 16) & M32), "wpool2,h16ll")
reg("mpya", lane3(32, lambda x, y, z: (s(x & 0xFFFF, 16) * s(y & 0xFFFF, 16) + z) & M32), "wrand,h16ll")
reg("mpyhha", lane3(32, lambda x, y, z: (s(x >> 16, 16) * s(y >> 16, 16) + z) & M32), "wrand,h16hh")
reg("mpyhhau", lane3(32, lambda x, y, z: ((x >> 16) * (y >> 16) + z) & M32), "wrand,h16hh")
# ---- byte / halfword lanes
reg("absdb", lane2(8, lambda x, y: abs(x - y)), "byte2"); reg("avgb", lane2(8, lambda x, y: (x + y + 1) >> 1), "byte2")
reg("ceqb", lane2(8, lambda x, y: T(x == y, 8)), "byte2"); reg("cgtb", lane2(8, lambda x, y: T(s(x, 8) > s(y, 8), 8)), "byte2")
reg("clgtb", lane2(8, lambda x, y: T(x > y, 8)), "byte2"); reg("cntb", lane1(8, lambda x: (x).bit_count()), "byte2,w32")
reg("ceqh", lane2(16, lambda x, y: T(x == y, 16)), "half2"); reg("cgth", lane2(16, lambda x, y: T(s(x, 16) > s(y, 16), 16)), "half2")
reg("clgth", lane2(16, lambda x, y: T(x > y, 16)), "half2"); reg("ah", lane2(16, lambda x, y: (x + y) & 0xFFFF), "half2")
reg("sfh", lane2(16, lambda x, y: (y - x) & 0xFFFF), "half2")
reg("shlh", lane2(16, lambda x, y: 0 if (y & 0x1F) > 15 else (x << (y & 0x1F)) & 0xFFFF), "half2")
reg("roth", lane2(16, lambda x, y: ((x << (y & 15)) | (x >> (16 - (y & 15)))) & 0xFFFF), "half2")
reg("rothm", lane2(16, lambda x, y: 0 if ((-y) & 0x1F) > 15 else x >> ((-y) & 0x1F)), "half2")
reg("rothma", lane2(16, lambda x, y: (s(x, 16) >> min(15, (-y) & 0x1F)) & 0xFFFF), "half2")
reg("sumb", lambda a, b, c, i: unwords([ (sum(lanes(w2, 8)) << 16 | sum(lanes(w1, 8))) & M32 for w1, w2 in [(x, y) for x, y in zip(words(a), words(b))] ]) if False else
    unwords([((sum(unlanes_b(y)) << 16) | sum(unlanes_b(x))) & M32 for x, y in zip(words(a), words(b))]), "wpool2,wrand")
def unlanes_b(w): return [(w >> sh) & 0xFF for sh in (24, 16, 8, 0)]
reg("xsbh", lane1(16, lambda x: s(x & 0xFF, 8) & 0xFFFF), "w32"); reg("xshw", lane1(32, lambda x: s(x & 0xFFFF, 16) & M32), "w32")
reg("clz", lane1(32, lambda x: 32 - x.bit_length()), "w32")
reg("xswd", lambda a, b, c, i: unwords([ (M32 if w[1] >> 31 else 0), w[1], (M32 if w[3] >> 31 else 0), w[3]]) if (w := words(a)) else 0, "w32o,wrand")
reg("orx", lambda a, b, c, i: (words(a)[0] | words(a)[1] | words(a)[2] | words(a)[3]) << 96, "wrand")
reg("gb", lambda a, b, c, i: sum((w & 1) << (3 - k) for k, w in enumerate(words(a))) << 96, "bits16,wrand")
reg("gbh", lambda a, b, c, i: sum((h & 1) << (7 - k) for k, h in enumerate(lanes(a, 16))) << 96, "bits16,wrand")
reg("gbb", lambda a, b, c, i: sum((h & 1) << (15 - k) for k, h in enumerate(lanes(a, 8))) << 96, "bits16,wrand")
reg("fsm", lambda a, b, c, i: unwords([M32 if (words(a)[0] >> (3 - k)) & 1 else 0 for k in range(4)]), "qcnta")
reg("fsmh", lambda a, b, c, i: unlanes([0xFFFF if (words(a)[0] >> (7 - k)) & 1 else 0 for k in range(8)], 16), "qcnta")
reg("fsmb", lambda a, b, c, i: unlanes([0xFF if (words(a)[0] >> (15 - k)) & 1 else 0 for k in range(16)], 8), "qcnta")
reg("fsmbi", lambda a, b, c, i: unlanes([0xFF if ((i & 0xFFFF) >> (15 - k)) & 1 else 0 for k in range(16)], 8), "fsmbi")
# ---- variable shifts
reg("shl", lane2(32, lambda x, y: 0 if (y & 0x3F) > 31 else (x << (y & 0x3F)) & M32), "wcnt")
reg("rot", lane2(32, lambda x, y: ((x << (y & 31)) | (x >> (32 - (y & 31)))) & M32), "wcnt")
reg("rotm", lane2(32, lambda x, y: 0 if ((-y) & 0x3F) > 31 else x >> ((-y) & 0x3F)), "wcnt")
reg("rotma", lane2(32, lambda x, y: (s(x, 32) >> min(31, (-y) & 0x3F)) & M32), "wcnt")
def qsh_l(q, n): return (q << n) & M128
def qrot_l(q, n): n %= 128; return ((q << n) | (q >> (128 - n))) & M128 if n else q
def qsh_r(q, n): return q >> n
b0 = lambda b: words(b)[0]
reg("shlqbi", lambda a, b, c, i: qsh_l(a, b0(b) & 7), "qcnt"); reg("rotqbi", lambda a, b, c, i: qrot_l(a, b0(b) & 7), "qcnt")
reg("rotqmbi", lambda a, b, c, i: qsh_r(a, (-b0(b)) & 7), "qcnt")
reg("shlqby", lambda a, b, c, i: 0 if (b0(b) & 0x1F) >= 16 else qsh_l(a, 8 * (b0(b) & 0x1F)), "qcnt")
reg("rotqby", lambda a, b, c, i: qrot_l(a, 8 * (b0(b) & 0xF)), "qcnt")
reg("rotqmby", lambda a, b, c, i: 0 if ((-b0(b)) & 0x1F) >= 16 else qsh_r(a, 8 * ((-b0(b)) & 0x1F)), "qcnt")
reg("shlqbybi", lambda a, b, c, i: 0 if ((b0(b) >> 3) & 0x1F) >= 16 else qsh_l(a, 8 * ((b0(b) >> 3) & 0x1F)), "qcnt")
reg("rotqbybi", lambda a, b, c, i: qrot_l(a, 8 * ((b0(b) >> 3) & 0xF)), "qcnt")
reg("rotqmbybi", lambda a, b, c, i: 0 if ((-(b0(b) >> 3)) & 0x1F) >= 16 else qsh_r(a, 8 * ((-(b0(b) >> 3)) & 0x1F)), "qcnt")
def ctl(t, n):
    bs = [0x10 + k for k in range(16)]; t &= 0xF & ~(n - 1)
    for k in range(n): bs[t + k] = k + (4 - n if n < 4 else 0) if n < 4 else k
    return unlanes(bs, 8)
def ctlx(pos, n):
    bs = [0x10 + k for k in range(16)]; t = pos & 0xF & ~(n - 1)
    vals = {1: [3], 2: [2, 3], 4: [0, 1, 2, 3], 8: [0, 1, 2, 3, 4, 5, 6, 7]}[n]
    for k, v in enumerate(vals): bs[t + k] = v
    return unlanes(bs, 8)
reg("cbx", lambda a, b, c, i: ctlx((b0(a) + b0(b)) & M32, 1), "qcnt"); reg("chx", lambda a, b, c, i: ctlx((b0(a) + b0(b)) & M32, 2), "qcnt")
reg("cwx", lambda a, b, c, i: ctlx((b0(a) + b0(b)) & M32, 4), "qcnt"); reg("cdx", lambda a, b, c, i: ctlx((b0(a) + b0(b)) & M32, 8), "qcnt")
reg("cbd", lambda a, b, c, i: ctlx((b0(a) + i) & M32, 1), "immw"); reg("chd", lambda a, b, c, i: ctlx((b0(a) + i) & M32, 2), "immw")
reg("cwd", lambda a, b, c, i: ctlx((b0(a) + i) & M32, 4), "immw"); reg("cdd", lambda a, b, c, i: ctlx((b0(a) + i) & M32, 8), "immw")
def shufb(a, b, c, i):
    cat = lanes(a, 8) + lanes(b, 8); out = []
    for sel in lanes(c, 8):
        if sel & 0xE0 == 0xE0: out.append(0x80)
        elif sel & 0xC0 == 0xC0: out.append(0xFF)
        elif sel & 0xC0 == 0x80: out.append(0x00)
        else: out.append(cat[sel & 0x1F])
    return unlanes(out, 8)
reg("shufb", shufb, "shufb_pos,shufb_rand,shufb_all")
# ---- immediates
def I32(i): return i & M32
reg("ai", laneI(32, lambda x, i: (x + i) & M32), "immw"); reg("sfi", laneI(32, lambda x, i: (i - x) & M32), "immw")
reg("andi", laneI(32, lambda x, i: x & I32(i)), "immw"); reg("ori", laneI(32, lambda x, i: x | I32(i)), "immw"); reg("xori", laneI(32, lambda x, i: x ^ I32(i)), "immw")
reg("ceqi", laneI(32, lambda x, i: T(x == I32(i), 32)), "immw"); reg("cgti", laneI(32, lambda x, i: T(s(x, 32) > s(I32(i), 32), 32)), "immw")
reg("clgti", laneI(32, lambda x, i: T(x > I32(i), 32)), "immw")
reg("ahi", laneI(16, lambda x, i: (x + i) & 0xFFFF), "immh"); reg("sfhi", laneI(16, lambda x, i: (i - x) & 0xFFFF), "immh")
reg("ceqhi", laneI(16, lambda x, i: T(x == (i & 0xFFFF), 16)), "immh"); reg("cgthi", laneI(16, lambda x, i: T(s(x, 16) > s(i & 0xFFFF, 16), 16)), "immh")
reg("clgthi", laneI(16, lambda x, i: T(x > (i & 0xFFFF), 16)), "immh")
reg("andhi", laneI(16, lambda x, i: x & (i & 0xFFFF)), "immh"); reg("orhi", laneI(16, lambda x, i: x | (i & 0xFFFF)), "immh"); reg("xorhi", laneI(16, lambda x, i: x ^ (i & 0xFFFF)), "immh")
reg("andbi", laneI(8, lambda x, i: x & (i & 0xFF)), "immb"); reg("orbi", laneI(8, lambda x, i: x | (i & 0xFF)), "immb"); reg("xorbi", laneI(8, lambda x, i: x ^ (i & 0xFF)), "immb")
reg("ceqbi", laneI(8, lambda x, i: T(x == (i & 0xFF), 8)), "immb"); reg("cgtbi", laneI(8, lambda x, i: T(s(x, 8) > s(i & 0xFF, 8), 8)), "immb")
reg("clgtbi", laneI(8, lambda x, i: T(x > (i & 0xFF), 8)), "immb")
reg("mpyi", laneI(32, lambda x, i: (s(x & 0xFFFF, 16) * s(i & 0xFFFF, 16)) & M32), "immh"); reg("mpyui", laneI(32, lambda x, i: (x & 0xFFFF) * (i & 0xFFFF)), "immh")
reg("shli", laneI(32, lambda x, i: 0 if (i & 0x3F) > 31 else (x << (i & 0x3F)) & M32), "immw")
reg("shlhi", laneI(16, lambda x, i: 0 if (i & 0x1F) > 15 else (x << (i & 0x1F)) & 0xFFFF), "immh")
reg("roti", laneI(32, lambda x, i: ((x << (i & 31)) | (x >> (32 - (i & 31)))) & M32), "immw")
reg("rothi", laneI(16, lambda x, i: ((x << (i & 15)) | (x >> (16 - (i & 15)))) & 0xFFFF), "immh")
reg("rotmi", laneI(32, lambda x, i: 0 if ((-i) & 0x3F) > 31 else x >> ((-i) & 0x3F)), "immw")
reg("rotmhi", laneI(16, lambda x, i: 0 if ((-i) & 0x1F) > 15 else x >> ((-i) & 0x1F)), "immh")
reg("rothmi", laneI(16, lambda x, i: 0 if ((-i) & 0x1F) > 15 else x >> ((-i) & 0x1F)), "immh")
reg("rotmai", laneI(32, lambda x, i: (s(x, 32) >> min(31, (-i) & 0x3F)) & M32), "immw")
reg("rotmahi", laneI(16, lambda x, i: (s(x, 16) >> min(15, (-i) & 0x1F)) & 0xFFFF), "immh")
reg("shlqbii", lambda a, b, c, i: qsh_l(a, i & 7), "immw"); reg("rotqbii", lambda a, b, c, i: qrot_l(a, i & 7), "immw")
reg("rotqmbii", lambda a, b, c, i: qsh_r(a, (-i) & 7), "immw")
reg("shlqbyi", lambda a, b, c, i: 0 if (i & 0x1F) >= 16 else qsh_l(a, 8 * (i & 0x1F)), "immw")
reg("rotqbyi", lambda a, b, c, i: qrot_l(a, 8 * (i & 0xF)), "immw")
reg("rotqmbyi", lambda a, b, c, i: 0 if ((-i) & 0x1F) >= 16 else qsh_r(a, 8 * ((-i) & 0x1F)), "immw")
reg("il", lambda a, b, c, i: unwords([I32(i)] * 4), "immw"); reg("ila", lambda a, b, c, i: unwords([i & 0x3FFFF] * 4), "immw")
reg("ilh", lambda a, b, c, i: unlanes([i & 0xFFFF] * 8, 16), "immw"); reg("ilhu", lambda a, b, c, i: unwords([(i & 0xFFFF) << 16] * 4), "immw")
reg("iohl", laneI(32, lambda x, i: x | (i & 0xFFFF)), "immw")

# ---- single precision (extended range, exact rational model)
def sp_val(x):
    e = (x >> 23) & 255
    if e == 0: return F(0)
    v = F((1 << 23) | (x & 0x7FFFFF), 1 << 23) * (F(2) ** (e - 127))
    return -v if x >> 31 else v
def sp_pack(v):                                   # truncate an exact value to an SPU single
    if v == 0: return 0
    sg = 0x80000000 if v < 0 else 0
    m = abs(v); e = m.numerator.bit_length() - m.denominator.bit_length()
    if F(2) ** e > m: e -= 1                      # floor(log2 m)
    if e + 127 > 255: return sg | 0x7FFFFFFF
    if e + 127 < 1: return 0
    f = int((m / (F(2) ** e) - 1) * (1 << 23))    # truncation toward zero
    return sg | ((e + 127) << 23) | f
def lanes_sp2(fn): return lambda a, b, c, i: unwords([fn(x, y) for x, y in zip(words(a), words(b))])
def lanes_sp3(fn): return lambda a, b, c, i: unwords([fn(x, y, z) for x, y, z in zip(words(a), words(b), words(c))])
reg("fa", lanes_sp2(lambda x, y: sp_pack(sp_val(x) + sp_val(y))), "fpool2,frand2,fnear2,fovf2")
reg("fs", lanes_sp2(lambda x, y: sp_pack(sp_val(x) - sp_val(y))), "fpool2,frand2,fnear2,fovf2")
reg("fm", lanes_sp2(lambda x, y: sp_pack(sp_val(x) * sp_val(y))), "fpool2,frand2,fnear2,fovf2")
reg("fma", lanes_sp3(lambda x, y, z: sp_pack(sp_val(x) * sp_val(y) + sp_val(z))), "fpool3,frand3,fnear3")
reg("fms", lanes_sp3(lambda x, y, z: sp_pack(sp_val(x) * sp_val(y) - sp_val(z))), "fpool3,frand3,fnear3")
reg("fnms", lanes_sp3(lambda x, y, z: sp_pack(sp_val(z) - sp_val(x) * sp_val(y))), "fpool3,frand3,fnear3")
reg("fceq", lanes_sp2(lambda x, y: M32 if sp_val(x) == sp_val(y) else 0), "fpool2,frand2,fnear2")
reg("fcgt", lanes_sp2(lambda x, y: M32 if sp_val(x) > sp_val(y) else 0), "fpool2,frand2,fnear2")
reg("fcmeq", lanes_sp2(lambda x, y: M32 if abs(sp_val(x)) == abs(sp_val(y)) else 0), "fpool2,frand2,fnear2")
reg("fcmgt", lanes_sp2(lambda x, y: M32 if abs(sp_val(x)) > abs(sp_val(y)) else 0), "fpool2,frand2,fnear2")
def fi_lane(y, est):
    e = (est >> 23) & 255
    if e == 0: return 0
    base, step, yy = (est >> 10) & 0x1FFF, est & 0x3FF, y & 0x7FFFF
    mag = 1 + F(base, 1 << 13) - F(step, 1 << 13) * F(yy, 1 << 19)
    v = mag * (F(2) ** (e - 127))
    return sp_pack(-v if est >> 31 else v)
reg("fi", lanes_sp2(lambda x, y: fi_lane(x, y)), "fi")
def cflt(x, sc, uns):
    v = sp_val(x) * (F(2) ** (173 - sc)); t = int(v)          # int() truncates toward zero
    if uns: return 0 if t < 0 else min(t, M32)
    return max(-(1 << 31), min((1 << 31) - 1, t)) & M32
def csflt(w, sc, uns):
    v = F(w if uns else s(w, 32)) / (F(2) ** (155 - sc)); return sp_pack(v)
def _cvt_fn(conv, sc, uns):
    return lambda a, b, c, i: unwords([conv(x, sc, uns) for x in words(a)])
for sc in (0, 1, 2, 126, 127, 128, 154, 155, 156, 157, 173, 255):
    reg(f"cflts@{sc}", _cvt_fn(cflt, sc, False), "w32")
    reg(f"cfltu@{sc}", _cvt_fn(cflt, sc, True), "w32")
    reg(f"csflt@{sc}", _cvt_fn(csflt, sc, False), "w32")
    reg(f"cuflt@{sc}", _cvt_fn(csflt, sc, True), "w32")

# ---- double precision (IEEE nearest-even; denormal operands read as zero; NaN -> default QNaN), exact via Fractions
QNAN = 0x7FF8000000000000
def dwords(q): return [(q >> 64) & ((1 << 64) - 1), q & ((1 << 64) - 1)]
def undw(l): return (l[0] << 64) | l[1]
def d_cls(u):
    e = (u >> 52) & 0x7FF; f = u & ((1 << 52) - 1)
    if e == 0x7FF: return "nan" if f else "inf"
    if e == 0: return "zero"                      # denormal read as zero, sign kept
    return "fin"
def d_val(u):
    e = (u >> 52) & 0x7FF; f = u & ((1 << 52) - 1)
    if e == 0: return F(0)
    v = F((1 << 52) | f, 1 << 52) * (F(2) ** (e - 1023)); return -v if u >> 63 else v
def rne(v, mant, emin, emax, sign_zero):          # round exact v to a binary float format; returns (kind, value bits info)
    # returns (sign, e_biased, frac) fields or ('inf', sign)
    if v == 0: return ("z", sign_zero)
    sg = 1 if v < 0 else 0; m = abs(v)
    e = m.numerator.bit_length() - m.denominator.bit_length()
    if F(2) ** e > m: e -= 1
    e = max(e, emin)                          # subnormal: fixed exponent
    q = m / (F(2) ** (e - mant))                   # significand scaled to integer grid
    n = q.numerator // q.denominator; r = q - n
    if r > F(1, 2) or (r == F(1, 2) and (n & 1)): n += 1
    if n >> (mant + 1): n >>= 1; e += 1            # carried to next binade
    if e > emax: return ("inf", sg)
    if n >> mant == 0: return ("sub", sg, n)       # subnormal, biased exp 0
    return ("n", sg, e, n & ((1 << mant) - 1))
def pack64(r):
    if r[0] == "z": return r[1] << 63
    if r[0] == "inf": return (r[1] << 63) | (0x7FF << 52)
    if r[0] == "sub": return (r[1] << 63) | r[2]
    return (r[1] << 63) | ((r[2] + 1023) << 52) | r[3]
def pack32(r):
    if r[0] == "z": return r[1] << 31
    if r[0] == "inf": return (r[1] << 31) | (0xFF << 23)
    if r[0] == "sub": return (r[1] << 31) | r[2]
    return (r[1] << 31) | ((r[2] + 127) << 23) | r[3]
def dop(kind):
    def f(a, b, c, i):
        out = []
        for x, y, z in zip(dwords(a), dwords(b), dwords(c)):
            ops = [x, y] + ([z] if kind in ("fma", "fms", "fnms", "fnma") else [])
            cl = [d_cls(o) for o in ops]
            if "nan" in cl: out.append((QNAN, False)); continue
            if kind == "add": ex = d_val(x) + d_val(y); inv = (cl == ["inf", "inf"] and (x >> 63) != (y >> 63))
            if kind == "sub": ex = d_val(x) - d_val(y); inv = (cl == ["inf", "inf"] and (x >> 63) == (y >> 63))
            if kind == "mul": ex = d_val(x) * d_val(y); inv = ("inf" in cl and "zero" in cl)
            if kind in ("fma", "fms", "fnms", "fnma"):
                zz = d_val(z) * (-1 if kind in ("fms", "fnms") else 1)
                ex = d_val(x) * d_val(y) + zz
                inv = ("inf" in cl[:2] and "zero" in cl[:2])
            if "inf" in cl or inv:
                out.append(None); continue         # inf arithmetic: checked separately below
            out.append((pack64(rne(ex, 52, -1022, 1023, 0)), ex == 0))
        return out
    return f
# exact-zero sign and inf cases need care; the generic model above handles finite results and leaves inf to specialised code
def dfull(kind):
    base = dop(kind)
    def f(a, b, c, i):
        res = base(a, b, c, i); out = []
        for k, (x, y, z) in enumerate(zip(dwords(a), dwords(b), dwords(c))):
            r = res[k]
            if r is None: r = d_inf(kind, x, y, z)
            else:
                r, exact0 = r
                if exact0: r = d_zero_sign(kind, x, y, z, r)     # IEEE RN sign of an exactly-zero result
            out.append(r)
        return undw(out)
    return f
def sgn(u): return u >> 63
def d_inf(kind, x, y, z):
    def ival(u):
        if d_cls(u) == "inf": return float("-inf") if sgn(u) else float("inf")
        return float(d_val(u)) if d_cls(u) == "fin" else (-0.0 if sgn(u) else 0.0)
    xv, yv, zv = ival(x), ival(y), ival(z)
    with __import__("warnings").catch_warnings():
        if kind == "add": r = xv + yv
        elif kind == "sub": r = xv - yv
        elif kind == "mul": r = xv * yv
        else:
            zz = -zv if kind in ("fms", "fnms") else zv
            if d_cls(x) != "inf" and d_cls(y) != "inf" and d_cls(z) == "inf": r = zz     # exact finite product + inf
            else:
                p = xv * yv; r = p + zz
        if math.isnan(r): return QNAN
        if kind in ("fnma", "fnms") and not math.isnan(r): r = -r
    if math.isnan(r): return QNAN
    return (0xFFF0000000000000 if r < 0 else 0x7FF0000000000000) if abs(r) == float("inf") else d_pack_float(r)
def d_pack_float(r): return struct.unpack(">Q", struct.pack(">d", r))[0]
def d_zero_sign(kind, x, y, z, r):
    # exact-zero results: IEEE RN: sum of like-signed zeros keeps the sign, otherwise +0; product zero sign = xor
    def zs(u): return sgn(u)
    if kind in ("add", "sub"):
        sy = zs(y) ^ (1 if kind == "sub" else 0)
        if d_val(x) == 0 and d_val(y) == 0: return (zs(x) & sy) << 63
        return 0
    if kind == "mul": return (zs(x) ^ zs(y)) << 63
    sp = zs(x) ^ zs(y); sz = zs(z) ^ (1 if kind in ("fms", "fnms") else 0)
    pz = d_val(x) * d_val(y) == 0
    if pz and d_val(z) == 0: res_s = sp & sz
    else: res_s = 0
    if kind in ("fnma", "fnms"): res_s ^= 1
    return res_s << 63
def dnegate_post(kind, f):
    def g(a, b, c, i):
        q = f(a, b, c, i)
        if kind in ("fnma", "fnms"):
            out = []
            for u in dwords(q):
                out.append(u if (u & 0x7FFFFFFFFFFFFFFF) > 0x7FF0000000000000 else u ^ (1 << 63))
            q = undw(out)
        return q
    return g
def mk(kind, k2):
    base = dfull(k2)
    return base
# simple approach: model dfnma/dfnms as negation of the (fma/fms) result, NaN stays default QNaN
def dmodel(kind):
    inner = {"dfa": "add", "dfs": "sub", "dfm": "mul", "dfma": "fma", "dfms": "fms", "dfnms": "fms", "dfnma": "fma"}[kind]
    f = dfull(inner)
    if kind in ("dfnms", "dfnma"): return dnegate_post("fnma", f)
    return f
for k in ("dfa", "dfs", "dfm"): reg(k, dmodel(k), "dpool2,drand2,dnear2")
for k in ("dfma", "dfms", "dfnms", "dfnma"): reg(k, dmodel(k), "dpool3,drand3,dnear3")
# dfceq/dfcmeq/dfcgt/dfcmgt/dftsv are not modelled here either: a retail CBE SPU
# raises an invalid-instruction error for them (hwprobe spuexh special A, issue
# #124), so the sweep (test_spu_exhaustive.c) carries no rows to check.
def fesd(a, b, c, i):
    out = []
    for w in words(a)[0::2]:
        be = (w >> 23) & 255; f = w & 0x7FFFFF; sg = (w >> 31) << 63
        if be == 0: u = 0 if f else sg
        elif be == 255: u = 0x7FF8000000000000 if f else (sg | (0x7FF << 52))
        else: u = sg | ((be - 127 + 1023) << 52) | (f << 29)
        out.append(u)
    return undw(out)
reg("fesd", fesd, "w32e")
def frds(a, b, c, i):
    res = []
    for u in dwords(a):
        if d_cls(u) == "nan": res += [0x7FC00000, 0]; continue
        if d_cls(u) == "inf": res += [(sgn(u) << 31) | (0xFF << 23), 0]; continue
        v = d_val(u)                               # denormal double reads as 0 (sign lost: header: spu__d_in keeps sign of zero)
        sz = sgn(u)
        res += [pack32(rne(v, 23, -126, 127, sz)), 0]
    return unwords(res)
reg("frds", frds, "dfrds,dpool2,drand2")

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("bin"); ap.add_argument("--n", type=int, default=20000)
    ap.add_argument("--ops", default=None); ap.add_argument("--blocks", type=int, default=0); ap.add_argument("--doms", default=None)
    a = ap.parse_args()
    sel = set(a.ops.split(",")) if a.ops else None
    seld = set(a.doms.split(",")) if a.doms else None
    tot_bad = 0
    for op, (fn, doms) in OPS.items():
        if sel and op not in sel: continue
        for dom in doms.split(","):
            if seld and dom not in seld: continue
            p = subprocess.run([a.bin, "--dump", op, dom, str(a.blocks), "--dump-n", str(a.n)], capture_output=True, text=True, check=False)
            lines = p.stdout.splitlines(); bad = 0; first = None
            for ln in lines:
                f = ln.split()
                av, bv, cv = int(f[1], 16), int(f[2], 16), int(f[3], 16); imm = int(f[4]); out = int(f[5], 16)
                exp = fn(av, bv, cv, imm)
                if exp != out:
                    bad += 1
                    if first is None: first = (f[0], f[1], f[2], f[3], imm, f"{out:032x}", f"{exp:032x}" if exp is not None else "None")
            tot_bad += bad
            detail = "" if not bad else "\n    first: call {} a={} b={} c={} imm={} got={} model={}".format(*first)
            print(f"{op:<12} {dom:<12} calls={len(lines):<6} mismatches={bad}{detail}")
            sys.stdout.flush()
    return 1 if tot_bad else 0
if __name__ == "__main__": sys.exit(main())
