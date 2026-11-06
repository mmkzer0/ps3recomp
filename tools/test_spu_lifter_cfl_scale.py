#!/usr/bin/env python3
"""cflts/cfltu lift only at I8 >= 15; smaller scales stop as unmodelled.

At I8 <= 14 the conversion can reach the hardware's unmodelled shift region:
with sh = be + 23 - I8, a biased exponent be >= 241 + I8 leaves the saturating
path of a CBE SPU and the fit found no bit-exact rule (489 measured lanes;
hwprobe `spuexh` lanes r007/r008, CECHC04 4.81 REBUG; issue #126). The
interpreter stops at such a word, so lifted code must emit the same named stop
(scale, word, pc, image) instead of calling a helper and running on with a
value the console never produced. I8 15 is the lowest modelled scale (max
sh = 255 + 23 - 15 = 263) and keeps lifting to its helper.

Usage:
    python3 tools/test_spu_lifter_cfl_scale.py
"""

import struct
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from spu_disasm import disassemble_spu
from spu_lifter import SPULifter

# RI8 format: opcd(10) | i8(8) | ra(7) | rt(7); cflts 0x1D8, cfltu 0x1D9
# (tools/spu_disasm.py ri8_table, runtime/spu/spu_interp_tables.inc).
CFLTS, CFLTU = 0x1D8, 0x1D9
PC = 0x12340


def word(op10, i8, ra=2, rt=3):
    return (op10 << 22) | ((i8 & 0xFF) << 14) | (ra << 7) | rt


def lift(w):
    data = struct.pack(">I", w)
    insns = disassemble_spu(data, PC)
    lifter = SPULifter()
    lifter.func_starts = {PC, PC + 4}
    lifter.lift_function(insns, PC, PC + 4)
    return "\n".join(lifter.functions[0].body_lines)


class UnmodelledCflScale(unittest.TestCase):
    def test_small_scales_stop_and_name_the_scale(self):
        for mn, op10 in (("cflts", CFLTS), ("cfltu", CFLTU)):
            for i8 in (0, 14):
                with self.subTest(op=mn, i8=i8):
                    w = word(op10, i8)
                    body = lift(w)
                    # No helper call: no modelled instruction result exists there.
                    self.assertNotIn("spu_cflts(", body)
                    self.assertNotIn("spu_cfltu(", body)
                    # The stop: named with the scale, the word, its pc and the image,
                    # at the word's own pc, through the same halt as #124's stops.
                    self.assertIn("SPU_STATUS_STOPPED_BY_HALT", body)
                    self.assertIn("spu_halt(ctx); return;", body)
                    self.assertIn(f"ctx->pc = 0x{PC:X}u;", body)
                    self.assertIn(f"INVALID-OP {mn}", body)
                    self.assertIn(f"unmodelled cflts/cfltu scale (I8={i8}, #126)", body)
                    self.assertIn(f"0x{w:08X}u", body)

    def test_modelled_scales_still_lift_to_their_helper(self):
        for mn, op10, helper in (("cflts", CFLTS, "spu_cflts"),
                                 ("cfltu", CFLTU, "spu_cfltu")):
            for i8 in (15, 173):
                with self.subTest(op=mn, i8=i8):
                    body = lift(word(op10, i8))
                    self.assertIn(f"ctx->gpr[3] = {helper}(ctx->gpr[2], {i8});", body)
                    self.assertNotIn("SPU_STATUS_STOPPED_BY_HALT", body)


if __name__ == "__main__":
    unittest.main()
