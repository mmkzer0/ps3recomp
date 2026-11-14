#!/usr/bin/env python3
"""Synthetic SPU job for test_spu_effect_port.c (spu_effect_port.h).

usage: gen_xdiff_port_job.py <outdir>

Writes <outdir>/xdiff_port_job.elf, which build rules lift with
tools/spu_lifter.py, and <outdir>/xdiff_port_job.h, which carries the same
bytes for the interpreter and every address the test checks, so the two tiers
and the test read one definition. Hand-encoded instructions only.

The job at XJ_ENTRY touches every kind of effect once, in a fixed order: a GET,
a tag wait (MFC_WrTagMask, MFC_WrTagUpdate, MFC_RdTagStat), a two-element list
GET, GETLLAR and PUTLLC with their status reads, an rchcnt, a mailbox write and
a stop. The MFC parameter latches are written before each command and are not
effects. The job at XJ_MISS_ENTRY branches to XJ_MISS_TARGET, which no lift
holds.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

MFC_LSA, MFC_EAH, MFC_EAL, MFC_Size, MFC_TagID, MFC_Cmd = 16, 17, 18, 19, 20, 21
MFC_WrTagMask, MFC_WrTagUpdate, MFC_RdTagStat = 22, 23, 24
MFC_RdListStallStat, MFC_WrListStallAck, MFC_RdAtomicStat = 25, 26, 27
SPU_WrOutMbox = 28

ENTRY = 0x000
MISS_ENTRY = 0x400
MISS_TARGET = 0x3000

# data sits above the code (the interpreted tier keeps the code in local store)
LS_GET, LS_LIST_DEST, LS_ATOM, LS_LIST = 0x1000, 0x1100, 0x1200, 0x1300
EA_GET, EA_LIST0, EA_LIST1, EA_ATOM = 0x11000, 0x11100, 0x11200, 0x12000
GET_SIZE, LIST0_SIZE, LIST1_SIZE = 32, 16, 32
TAG = 1
# a PUT, an SDCRZ, and a list whose first element stalls (tag TAG2) until MFC_WrListStallAck
LS_PUT, EA_PUT, PUT_SIZE = 0x1400, 0x13000, 32
EA_ZERO, ZERO_SIZE = 0x14000, 128
LS_SLIST, LS_SLIST_DEST, EA_SL0, EA_SL1, EA_SL2, SL_SIZE, TAG2 = 0x1500, 0x1600, 0x11300, 0x11310, 0x11320, 16, 2
STOP_CODE, MBOX_VALUE, MISS_STOP_CODE, MISS_MARK = 0x1234, 0x5A5A, 0x2222, 0x1234


def word(v):
    return v & 0xFFFFFFFF


def il(rt, imm16):
    return word((0x081 << 23) | ((imm16 & 0xFFFF) << 7) | rt)


def ila(rt, imm18):
    return word((0x21 << 25) | ((imm18 & 0x3FFFF) << 7) | rt)


def rdch(rt, ch):
    return word((0x00D << 21) | (ch << 7) | rt)


def rchcnt(rt, ch):
    return word((0x00F << 21) | (ch << 7) | rt)


def wrch(ch, rt):
    return word((0x10D << 21) | (ch << 7) | rt)


def bi(ra):
    return word((0x1A8 << 21) | (ra << 7))


def stop(code):
    return word(code & 0x3FFF)


def job():
    w = []
    # GET EA_GET -> LS_GET, tag TAG
    w += [il(3, LS_GET), il(4, 0), ila(5, EA_GET), il(6, GET_SIZE), il(7, TAG), il(8, 0x40)]
    w += [wrch(MFC_LSA, 3), wrch(MFC_EAH, 4), wrch(MFC_EAL, 5), wrch(MFC_Size, 6),
          wrch(MFC_TagID, 7), wrch(MFC_Cmd, 8)]
    # wait for the tag group: mask = 1 << TAG, update = all
    w += [il(9, 1 << TAG), wrch(MFC_WrTagMask, 9), il(10, 2), wrch(MFC_WrTagUpdate, 10),
          rdch(11, MFC_RdTagStat)]
    # GETL: the list sits at LS_LIST (planted by the test), two elements
    w += [il(3, LS_LIST_DEST), il(5, LS_LIST), il(6, 16), il(8, 0x44)]
    w += [wrch(MFC_LSA, 3), wrch(MFC_EAL, 5), wrch(MFC_Size, 6), wrch(MFC_TagID, 7),
          wrch(MFC_Cmd, 8)]
    # PUT PUT_SIZE bytes from LS_PUT (planted by the test) to EA_PUT
    w += [il(3, LS_PUT), ila(5, EA_PUT), il(6, PUT_SIZE), il(8, 0x20)]
    w += [wrch(MFC_LSA, 3), wrch(MFC_EAL, 5), wrch(MFC_Size, 6), wrch(MFC_Cmd, 8)]
    # SDCRZ: zero ZERO_SIZE bytes at EA_ZERO (no local-store operand)
    w += [ila(5, EA_ZERO), il(6, ZERO_SIZE), il(8, 0x89)]
    w += [wrch(MFC_EAL, 5), wrch(MFC_Size, 6), wrch(MFC_Cmd, 8)]
    # a three-element GETL whose first element stalls: read the stall status, then
    # acknowledge it, which resumes the other two inside the acknowledgement
    w += [il(3, LS_SLIST_DEST), il(5, LS_SLIST), il(6, 24), il(7, TAG2), il(8, 0x44)]
    w += [wrch(MFC_LSA, 3), wrch(MFC_EAL, 5), wrch(MFC_Size, 6), wrch(MFC_TagID, 7),
          wrch(MFC_Cmd, 8)]
    w += [rdch(16, MFC_RdListStallStat), wrch(MFC_WrListStallAck, 7), il(7, TAG),
          wrch(MFC_TagID, 7)]
    # GETLLAR then PUTLLC on one line
    w += [il(3, LS_ATOM), ila(5, EA_ATOM), il(8, 0xD0)]
    w += [wrch(MFC_LSA, 3), wrch(MFC_EAH, 4), wrch(MFC_EAL, 5), wrch(MFC_Cmd, 8),
          rdch(12, MFC_RdAtomicStat)]
    w += [il(8, 0xB4), wrch(MFC_Cmd, 8), rdch(13, MFC_RdAtomicStat)]
    # a count, a mailbox write, the end
    w += [rchcnt(14, SPU_WrOutMbox), il(15, MBOX_VALUE), wrch(SPU_WrOutMbox, 15), stop(STOP_CODE)]
    return w


def miss():
    return [il(3, MISS_TARGET), il(10, 0), bi(3)]


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    code = bytearray(MISS_ENTRY + 4 * 3)
    for base, words in ((ENTRY, job()), (MISS_ENTRY, miss())):
        assert base + 4 * len(words) <= (MISS_ENTRY if base == ENTRY else len(code))
        for i, v in enumerate(words):
            struct.pack_into(">I", code, base + 4 * i, v)
    symbols = [{"name": "xj_job", "addr": ENTRY, "size": 4 * len(job())},
               {"name": "xj_miss", "addr": MISS_ENTRY, "size": 4 * len(miss())}]
    with open(os.path.join(out, "xdiff_port_job.elf"), "wb") as f:
        f.write(wrap(bytes(code), base=0, entry=ENTRY, symbols=symbols))
    with open(os.path.join(out, "xdiff_port_job.h"), "w") as f:
        f.write("/* Generated by gen_xdiff_port_job.py -- do not edit. */\n#pragma once\n")
        consts = (("ENTRY", ENTRY), ("MISS_ENTRY", MISS_ENTRY), ("MISS_TARGET", MISS_TARGET),
                  ("LS_GET", LS_GET), ("LS_LIST_DEST", LS_LIST_DEST), ("LS_ATOM", LS_ATOM),
                  ("LS_LIST", LS_LIST), ("EA_GET", EA_GET), ("EA_LIST0", EA_LIST0),
                  ("EA_LIST1", EA_LIST1), ("EA_ATOM", EA_ATOM), ("GET_SIZE", GET_SIZE),
                  ("LIST0_SIZE", LIST0_SIZE), ("LIST1_SIZE", LIST1_SIZE), ("TAG", TAG),
                  ("STOP_CODE", STOP_CODE), ("MBOX_VALUE", MBOX_VALUE),
                  ("MISS_STOP_CODE", MISS_STOP_CODE), ("MISS_MARK", MISS_MARK),
                  ("LS_PUT", LS_PUT), ("EA_PUT", EA_PUT), ("PUT_SIZE", PUT_SIZE),
                  ("EA_ZERO", EA_ZERO), ("ZERO_SIZE", ZERO_SIZE), ("LS_SLIST", LS_SLIST),
                  ("LS_SLIST_DEST", LS_SLIST_DEST), ("EA_SL0", EA_SL0), ("EA_SL1", EA_SL1),
                  ("EA_SL2", EA_SL2), ("SL_SIZE", SL_SIZE), ("TAG2", TAG2))
        f.writelines(f"#define XJ_{name} 0x{value:X}u\n" for name, value in consts)
        f.write(f"#define XJ_CODE_BYTES {len(code)}u\n")
        f.write("static const unsigned char xj_code[XJ_CODE_BYTES] = {\n")
        for i in range(0, len(code), 16):
            f.write("  " + ",".join(f"0x{b:02X}" for b in code[i:i + 16]) + ",\n")
        f.write("};\n")


if __name__ == "__main__":
    main()
