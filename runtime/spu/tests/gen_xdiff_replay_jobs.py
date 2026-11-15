#!/usr/bin/env python3
"""Synthetic SPU jobs for test_spu_xdiff_replay.c (docs/SPU_XDIFF.md section 16, commit 8).

usage: gen_xdiff_replay_jobs.py <outdir>

Writes one ELF per variant, <outdir>/xr_<name>.elf, which build rules lift with
tools/spu_lifter.py --symbol-prefix xr_<name>_ so that every variant links into one
test, and <outdir>/xdiff_replay_jobs.h, which carries the bytes of the images the
interpreter runs and every address and value the test checks.

The base job is gen_xdiff_port_job.job(): GET, tag wait, a two-element list, PUT,
SDCRZ, a list whose first element stalls and is resumed by MFC_WrListStallAck, GETLLAR
and PUTLLC, rchcnt and a mailbox write. The test records it interpreted, then replays
each lifted variant into a shadow built from that fixture. Each variant is the base
with one mutation that a same-program comparison has to name:

  base    unchanged: the lift is equal.
  size    one instruction changed: the GET asks for fewer bytes (effect 0, effect-request).
  mask    writes another tag mask, then overwrites the register with the right one, so no
          register shows it (effect 2, effect-request).
  stall   rewrites the remaining elements of the stalled list before the acknowledgement
          and puts them back afterwards, scratch registers included (the acknowledgement,
          effect-request, `where` the first rewritten list byte).
  latent  stores one quadword to local store at the end and nothing else: only the end
          local store differs (end-ls).
  nested  a different job: a function is called twice and stops on the second call; the
          interpreter ends at that stop, the lift returns into its caller and runs on to
          another stop (docs/SPU_XDIFF.md 5.1). The first call returns through `bi $r0`,
          which stores pc = the return address; lifted code stores pc at nothing else, so
          the second call's stop returns with pc still equal to the return address and the
          call bracket (spu_drain_call) lets the caller go on. A single call that stops
          would return with a stale pc, which the drain resumes at whatever it finds there.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import gen_xdiff_port_job as P
from wrap_spu_elf import wrap

LATENT_LS = 0x1800          # the latent variant's quadword
LATENT_IMM = LATENT_LS >> 4
NESTED_SUB_STOP, NESTED_AFTER_STOP, NESTED_MARK = 0x77, 0x78, 1234
MASK_BAD = (1 << P.TAG) | 0x20
SIZE_BAD = P.GET_SIZE // 2
STALL_WHERE = (16 + 3) - 8  # the rewritten element's size byte, counted from the remaining list's first byte
SCRATCH = (60, 61, 62)


def ri10(op8, i10, ra, rt):
    return P.word((op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt)


def lqd(rt, ra, i10):
    return ri10(0x34, i10, ra, rt)


def stqd(rt, ra, i10):
    return ri10(0x24, i10, ra, rt)


def brsl(rt, word_off):
    return P.word((0x66 << 23) | ((word_off & 0xFFFF) << 7) | rt)


def variant_size():
    w = list(P.job())
    w[w.index(P.il(6, P.GET_SIZE))] = P.il(6, SIZE_BAD)
    return w


def variant_mask():
    w = list(P.job())
    i = w.index(P.il(9, 1 << P.TAG))
    assert w[i + 1] == P.wrch(P.MFC_WrTagMask, 9)
    w[i] = P.il(9, MASK_BAD)
    w.insert(i + 2, P.il(9, 1 << P.TAG))   # the register is right again after the write
    return w


def variant_stall():
    w = list(P.job())
    ack = w.index(P.wrch(P.MFC_WrListStallAck, 7))
    assert w[ack - 1] == P.rdch(16, P.MFC_RdListStallStat)
    # the stalled list sits at LS_SLIST; its remaining elements are the second and third
    # (bytes +8 .. +24), and the quadword at +16 holds the third
    before = [P.il(60, P.LS_SLIST), lqd(61, 60, 1), P.il(62, 0), stqd(62, 60, 1)]
    after = [stqd(61, 60, 1)] + [P.il(r, 0) for r in SCRATCH]
    return w[:ack] + before + [w[ack]] + after + w[ack + 1:]


def variant_latent():
    w = list(P.job())
    assert w[-1] == P.stop(P.STOP_CODE)
    return w[:-1] + [stqd(15, 0, LATENT_IMM), w[-1]]   # r15 holds the mailbox word; r0 is zero


def ceqi(rt, ra, i10):
    return ri10(0x7C, i10, ra, rt)


def brnz(rt, word_off):
    return P.word((0x42 << 23) | ((word_off & 0xFFFF) << 7) | rt)


def nested_words():
    return [
        brsl(0, 5),                      # 0x00 main: brsl r0, sub  (return address 0x04)
        ceqi(24, 21, 1),                 # 0x04 r24 = (r21 == 1)
        brnz(24, -2),                    # 0x08 first return: call again
        P.il(20, NESTED_MARK),           # 0x0C only the lift gets here
        P.stop(NESTED_AFTER_STOP),       # 0x10
        ri10(0x1C, 1, 21, 21),           # 0x14 sub: ai r21, r21, 1
        ceqi(23, 21, 2),                 # 0x18
        brnz(23, 3),                     # 0x1C second call: stop
        P.bi(0),                         # 0x20 first call: return
        P.stop(0x79),                    # 0x24 not reached
        P.stop(NESTED_SUB_STOP),         # 0x28
    ]


def image(words_at_entry, extra=()):
    """A local-store image: the job at 0 (below MISS_ENTRY) and optional code at MISS_ENTRY."""
    code = bytearray(P.MISS_ENTRY + 4 * 3)
    assert 4 * len(words_at_entry) <= P.MISS_ENTRY
    for i, v in enumerate(words_at_entry):
        struct.pack_into(">I", code, 4 * i, v)
    for i, v in enumerate(extra):
        struct.pack_into(">I", code, P.MISS_ENTRY + 4 * i, v)
    return bytes(code)


def variants():
    miss = P.miss()
    yield "base", P.job(), miss
    yield "size", variant_size(), miss
    yield "mask", variant_mask(), miss
    yield "stall", variant_stall(), miss
    yield "latent", variant_latent(), miss


def c_array(f, name, data):
    f.write(f"static const unsigned char {name}[{len(data)}u] = {{\n")
    for i in range(0, len(data), 16):
        f.write("  " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",\n")
    f.write("};\n")


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    images = {}
    for name, words, miss in variants():
        code = image(words, miss)
        symbols = [{"name": "xr_job", "addr": P.ENTRY, "size": 4 * len(words)},
                   {"name": "xr_miss", "addr": P.MISS_ENTRY, "size": 4 * len(miss)}]
        with open(os.path.join(out, f"xr_{name}.elf"), "wb") as f:
            f.write(wrap(code, base=0, entry=P.ENTRY, symbols=symbols))
        images[name] = code
    nested = nested_words()
    ncode = image(nested)[:4 * len(nested)]
    symbols = [{"name": "xr_main", "addr": 0, "size": 0x14}, {"name": "xr_sub", "addr": 0x14, "size": 0x18}]
    with open(os.path.join(out, "xr_nested.elf"), "wb") as f:
        f.write(wrap(ncode, base=0, entry=0, symbols=symbols))

    with open(os.path.join(out, "xdiff_replay_jobs.h"), "w") as f:
        f.write("/* Generated by gen_xdiff_replay_jobs.py -- do not edit. */\n#pragma once\n")
        consts = (("ENTRY", P.ENTRY), ("MISS_ENTRY", P.MISS_ENTRY), ("MISS_TARGET", P.MISS_TARGET),
                  ("LS_GET", P.LS_GET), ("LS_LIST_DEST", P.LS_LIST_DEST), ("LS_ATOM", P.LS_ATOM),
                  ("LS_LIST", P.LS_LIST), ("EA_GET", P.EA_GET), ("EA_LIST0", P.EA_LIST0),
                  ("EA_LIST1", P.EA_LIST1), ("EA_ATOM", P.EA_ATOM), ("GET_SIZE", P.GET_SIZE),
                  ("LIST0_SIZE", P.LIST0_SIZE), ("LIST1_SIZE", P.LIST1_SIZE), ("TAG", P.TAG),
                  ("STOP_CODE", P.STOP_CODE), ("MBOX_VALUE", P.MBOX_VALUE),
                  ("MISS_STOP_CODE", P.MISS_STOP_CODE), ("MISS_MARK", P.MISS_MARK),
                  ("LS_PUT", P.LS_PUT), ("EA_PUT", P.EA_PUT), ("PUT_SIZE", P.PUT_SIZE),
                  ("EA_ZERO", P.EA_ZERO), ("ZERO_SIZE", P.ZERO_SIZE), ("LS_SLIST", P.LS_SLIST),
                  ("LS_SLIST_DEST", P.LS_SLIST_DEST), ("EA_SL0", P.EA_SL0), ("EA_SL1", P.EA_SL1),
                  ("EA_SL2", P.EA_SL2), ("SL_SIZE", P.SL_SIZE), ("TAG2", P.TAG2),
                  ("SIZE_BAD", SIZE_BAD), ("MASK_BAD", MASK_BAD), ("STALL_WHERE", STALL_WHERE),
                  ("LATENT_LS", LATENT_LS), ("NESTED_SUB_STOP", NESTED_SUB_STOP),
                  ("NESTED_AFTER_STOP", NESTED_AFTER_STOP), ("NESTED_MARK", NESTED_MARK))
        f.writelines(f"#define XR_{name} 0x{value:X}u\n" for name, value in consts)
        f.write(f"#define XR_CODE_BYTES {len(images['base'])}u\n")
        c_array(f, "xr_code", images["base"])
        f.write(f"#define XR_NESTED_CODE_BYTES {len(ncode)}u\n")
        c_array(f, "xr_nested_code", ncode)


if __name__ == "__main__":
    main()
