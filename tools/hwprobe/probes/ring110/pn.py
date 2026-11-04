#!/usr/bin/env python3
"""Python twin of common/pn.h: the Layer 2 marker codes (needed by the later capture analysis)."""

MASK = 0xFFFFFFFF
LEN = 128


def xorshift32(s):
    s ^= (s << 13) & MASK
    s ^= s >> 17
    s ^= (s << 5) & MASK
    return s & MASK


def pn_signs(code_id):
    """128 signs (+1/-1) of code `code_id`; id 0 is the reference code, trial k uses id k + 1."""
    s = ((code_id * 2654435761) & MASK) ^ 0x9E3779B9
    if s == 0:
        s = 1
    for _ in range(8):
        s = xorshift32(s)
    out = []
    for _ in range(LEN):
        s = xorshift32(s)
        out.append(1 if (s >> 16) & 1 else -1)
    return out


if __name__ == "__main__":
    import sys
    cid = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    print(" ".join(f"{v:+d}" for v in pn_signs(cid)))
