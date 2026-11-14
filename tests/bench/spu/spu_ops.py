"""Opcode table for gen_spu_bench.py: (mnemonic, field width in bits, opcode).

gen_spu_bench.py expects the full SPU opcode list from
tests/conformance/spu/spu_ops.py, which this tree does not have. This file is
the subset the loop control, the memory and branch kernels and the channel,
dma and atomic rows need, so the driver builds and measures those rows. The
register-operation rows (adds, shifts, floats, ...) exist only for the
mnemonics listed here; to benchmark the rest, extend this table with the
Cell SPU ISA encodings (width 11 = RR, 10 = RI8, 9 = RI16, 8 = RI10, 7 = RI18,
4 = RRR).
"""

OPCODES = [
    # hints and no-ops: the loop-control floor
    ("lnop", 11, 0x001), ("nop", 11, 0x201), ("sync", 11, 0x002), ("dsync", 11, 0x003),
    ("stop", 11, 0x000),
    # constants and the loop control's own arithmetic
    ("il", 9, 0x081), ("ai", 8, 0x1C), ("or", 11, 0x041),
    # local store
    ("lqd", 8, 0x34), ("lqx", 11, 0x1C4), ("lqa", 9, 0x061), ("lqr", 9, 0x067),
    ("stqd", 8, 0x24), ("stqx", 11, 0x144), ("stqa", 9, 0x041), ("stqr", 9, 0x047),
    # branches
    ("br", 9, 0x064), ("bra", 9, 0x060), ("brsl", 9, 0x066), ("brasl", 9, 0x062),
    ("brz", 9, 0x040), ("brnz", 9, 0x042), ("brhz", 9, 0x044), ("brhnz", 9, 0x046),
    ("bi", 11, 0x1A8), ("bisl", 11, 0x1A9), ("biz", 11, 0x128), ("binz", 11, 0x129),
    ("bihz", 11, 0x12A), ("bihnz", 11, 0x12B),
    # channels
    ("rdch", 11, 0x00D), ("rchcnt", 11, 0x00F), ("wrch", 11, 0x10D),
]
