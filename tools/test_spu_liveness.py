#!/usr/bin/env python3
"""tools/spu_liveness.py on synthetic images (docs/SPU_LIFTER.md "Liveness").

Every image is hand-encoded: a few functions at fixed offsets from an LS base, each padded with `nop`.
The cases are the ones the poison run relies on:

  leaf          the caller reads $r3 after the call: $r3 is live-out, $r4 is dead; a redefinition after the
                call makes the register dead again
  unknown       an indirect call, a function with no caller, an address-taken entry, an extra entry point and
                a data word holding the entry: every register is live-out
  callers       two callers with different uses: the union; a tail call passes the caller's live-out on
  stop          a terminating stop ends the program, a resuming stop and data read everything
  frame         a 0x40-byte frame is dead below the entry stack pointer; an unbalanced, indirect or
                absent frame is not claimed
  decoder       every mnemonic the disassembler can print has a def/use rule, so a new one cannot be silently
                treated as unknown

Usage:  python3 tools/test_spu_liveness.py
"""
import importlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(TOOLS, "..", "runtime", "spu", "tests"))

import spu_disasm
import spu_liveness as L
from wrap_spu_elf import wrap

G = importlib.import_module("gen_xdiff_poison_job")   # the job generator's encoders, which live with its C test

BASE = 0x7C00
NOP = 0x201 << 21
STOP_END, STOP_RESUME = 0x1234, 0x110


def word(v):
    return v & 0xFFFFFFFF


def br(pc, target):
    return word((0x64 << 23) | ((((target - pc) >> 2) & 0xFFFF) << 7))


def brz(rt, pc, target):
    return word((0x40 << 23) | ((((target - pc) >> 2) & 0xFFFF) << 7) | rt)


def brasl(rt, target):
    return word((0x62 << 23) | (((target >> 2) & 0xFFFF) << 7) | rt)


def bisl(rt, ra):
    return word((0x1A9 << 21) | (ra << 7) | rt)


def ila(rt, imm18):
    return word((0x21 << 25) | ((imm18 & 0x3FFFF) << 7) | rt)


def lqa(rt, addr):
    return word((0x61 << 23) | (((addr >> 2) & 0xFFFF) << 7) | rt)


def ori(rt, ra, imm):
    return G.ri10(0x04, imm, ra, rt)


class Image:
    """Functions at fixed offsets from BASE, one 0x40-byte slot each."""

    SLOT = 0x40

    def __init__(self, base=BASE, slots=8):
        self.base = base
        self.words = [NOP] * (self.SLOT // 4 * slots)
        self.used = {}

    def addr(self, slot, index=0):
        return self.base + slot * self.SLOT + 4 * index

    def put(self, slot, build):
        """`build(pc)` returns the words of the function whose entry is `pc`."""
        words = build(self.addr(slot))
        assert len(words) <= self.SLOT // 4
        at = slot * self.SLOT // 4
        self.words[at:at + len(words)] = words
        self.used[slot] = len(words)
        return self

    def data(self):
        return b"".join(w.to_bytes(4, "big") for w in self.words)

    def bounds(self):
        return [(self.addr(s), self.addr(s, n)) for s, n in sorted(self.used.items())]

    def analyze(self, **kw):
        kw.setdefault("functions", self.bounds())
        doc = L.analyze(self.data(), self.base, **kw)
        return {int(f["entry"], 16): f for f in doc["functions"]}


def leaf(pc):
    return [G.il(3, 0x1111), G.il(4, 0x2222), G.bi(0)]


def call_then(*after):
    """A function that calls the leaf in slot 1, runs `after`, and ends the program."""
    def build(pc):
        return [G.brsl(0, pc, BASE + Image.SLOT), *after, G.stop(STOP_END)]
    return build


class Leaf(unittest.TestCase):
    def test_caller_reads_r3_not_r4(self):
        f = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf).analyze()[BASE + 0x40]
        self.assertIn(3, f["live_out"])
        self.assertIn(1, f["live_out"])
        self.assertNotIn(4, f["live_out"])
        self.assertIn(4, f["dead_regs"])
        self.assertNotIn(3, f["dead_regs"])
        self.assertNotIn(1, f["dead_regs"])   # the stack pointer is never claimed dead
        self.assertEqual(sorted(f["live_out"] + f["dead_regs"]), list(range(128)))
        self.assertEqual(f["callers"], [f"0x{BASE:05x}"])
        self.assertFalse(f["unknown_callers"])
        self.assertEqual(f["exits"], [f"0x{BASE + 0x40 + 8:05x}"])

    def test_redefinition_after_the_call_kills_the_register(self):
        f = Image().put(0, call_then(G.il(3, 0), G.wrch(28, 3))).put(1, leaf).analyze()[BASE + 0x40]
        self.assertIn(3, f["dead_regs"])
        self.assertEqual(f["live_out"], [1])

    def test_value_the_callee_leaves_untouched_is_live_through(self):
        f = Image().put(0, call_then(G.wrch(28, 6))).put(1, leaf).analyze()[BASE + 0x40]
        self.assertIn(6, f["live_out"])   # the leaf never writes $r6, the caller reads it: it must come back unchanged

    def test_callee_read_before_write_is_live_before_the_call(self):
        def callee(pc):
            return [G.wrch(28, 9), G.il(3, 0), G.bi(0)]
        funcs = Image().put(0, call_then(G.wrch(28, 3))).put(1, callee)
        f = funcs.analyze()
        self.assertEqual(f[BASE + 0x40]["live_out"], [1, 3])

    def test_conditional_return_adds_the_fall_through(self):
        def callee(pc):
            return [G.il(3, 1), G.il(4, 2), word((0x128 << 21) | 5), G.bi(0)]   # biz $r5,$r0 then bi $r0
        f = Image().put(0, call_then(G.wrch(28, 3))).put(1, callee).analyze()[BASE + 0x40]
        self.assertEqual(f["live_out"], [1, 3])
        self.assertEqual(len(f["exits"]), 2)


class UnknownCallers(unittest.TestCase):
    def assert_all_live(self, f):
        self.assertTrue(f["unknown_callers"])
        self.assertEqual(f["live_out"], list(range(128)))
        self.assertEqual(f["dead_regs"], [])

    def test_function_without_a_caller(self):
        f = Image().put(0, leaf).analyze()[BASE]
        self.assert_all_live(f)
        self.assertIn("no-caller", f["unknown_reasons"])

    def test_indirect_call_through_an_address_constant(self):
        def main(pc):
            return [ila(9, BASE + Image.SLOT), bisl(0, 9), G.wrch(28, 3), G.stop(STOP_END)]
        f = Image().put(0, main).put(1, leaf).analyze()[BASE + 0x40]
        self.assert_all_live(f)
        self.assertEqual(f["unknown_reasons"], ["address-taken", "no-caller"])

    def test_address_materialised_with_ilhu_and_iohl(self):
        base = 0x20000
        entry = base + Image.SLOT

        def main(pc):
            return [G.word((0x082 << 23) | ((entry >> 16) << 7) | 9), G.word((0x0C1 << 23) | ((entry & 0xFFFF) << 7) | 9),
                    G.stop(STOP_END)]
        f = Image(base).put(0, main).put(1, leaf).analyze()[entry]
        self.assertIn("address-taken", f["unknown_reasons"])

    def test_address_in_a_data_word_even_with_a_direct_caller(self):
        img = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf)
        img.words[-1] = BASE + Image.SLOT   # a function pointer table slot, far from any code
        f = img.analyze()[BASE + 0x40]
        self.assertTrue(f["unknown_callers"])
        self.assertIn("address-taken", f["unknown_reasons"])
        self.assertEqual(f["callers"], [f"0x{BASE:05x}"])
        self.assertEqual(f["dead_regs"], [])

    def test_extra_entry_point_is_unknown_and_splits_its_bound(self):
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), G.wrch(28, 3), G.il(9, 0), G.stop(STOP_END)]
        img = Image().put(0, main).put(1, leaf)
        extra = BASE + Image.SLOT + 4
        self.assertEqual(L.function_bounds(img.data(), BASE, 0, [extra], img.bounds()),
                         [(BASE, BASE + 16), (BASE + 0x40, extra), (extra, extra + 8)])
        docs = img.analyze(entries=[extra])
        self.assertEqual(docs[extra]["unknown_reasons"], ["entry-point"])
        self.assertEqual(docs[extra]["dead_regs"], [])
        self.assertEqual(docs[extra]["tail_callers"], [f"0x{extra - 4:05x}"])
        self.assertFalse(docs[BASE + 0x40]["unknown_callers"])   # the first half is still only called directly

    def test_call_into_the_middle_of_a_function(self):
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT + 4), G.wrch(28, 3), G.stop(STOP_END)]
        docs = Image().put(0, main).put(1, leaf).analyze()
        self.assertIn("mid-entry-call", docs[BASE + 0x40]["unknown_reasons"])
        self.assertEqual(docs[BASE + 0x40]["dead_regs"], [])

    def test_unseen_callee_reads_everything(self):
        # the next thing the caller does is a call to an address outside the image: its callee may read any
        # register, so nothing the leaf leaves behind is dead
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), brasl(0, 0x3F000), G.stop(STOP_END)]
        f = Image().put(0, main).put(1, leaf).analyze()[BASE + 0x40]
        self.assertEqual(f["dead_regs"], [])


class Callers(unittest.TestCase):
    def test_two_callers_union_of_their_uses(self):
        def other(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), G.wrch(28, 4), G.stop(STOP_END)]
        img = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf).put(2, other)
        f = img.analyze()[BASE + 0x40]
        self.assertEqual(f["live_out"], [1, 3, 4])
        self.assertEqual(f["callers"], [f"0x{BASE:05x}", f"0x{BASE + 0x80:05x}"])
        self.assertNotIn(5, f["live_out"])
        self.assertIn(5, f["dead_regs"])
        self.assertFalse(f["unknown_callers"])

    def test_tail_call_inherits_the_callers_live_out(self):
        def mid(pc):
            return [G.il(5, 7), br(pc + 4, BASE + 2 * Image.SLOT)]
        img = Image().put(0, call_then(G.wrch(28, 3))).put(1, mid).put(2, leaf)
        docs = img.analyze()
        self.assertEqual(docs[BASE + 0x40]["live_out"], [1, 3])
        end = docs[BASE + 0x80]
        self.assertEqual(end["live_out"], [1, 3])
        self.assertEqual(end["callers"], [])
        self.assertEqual(end["tail_callers"], [f"0x{BASE + 0x44:05x}"])
        self.assertFalse(end["unknown_callers"])
        self.assertEqual(docs[BASE + 0x40]["tail_calls"], [f"0x{BASE + 0x80:05x}"])

    def test_conditional_tail_call_keeps_the_fall_through(self):
        def mid(pc):
            return [brz(5, pc, BASE + 2 * Image.SLOT), G.il(3, 9), G.bi(0)]
        docs = Image().put(0, call_then(G.wrch(28, 3))).put(1, mid).put(2, leaf).analyze()
        self.assertEqual(docs[BASE + 0x40]["live_out"], [1, 3])
        self.assertEqual(docs[BASE + 0x80]["live_out"], [1, 3])

    def test_recursion_reaches_a_fixed_point(self):
        def rec(pc):
            return [G.il(3, 1), brz(2, pc + 4, pc + 12), G.brsl(0, pc + 8, pc), G.bi(0)]
        docs = Image().put(0, call_then(G.wrch(28, 3))).put(1, rec).analyze()
        self.assertEqual(docs[BASE + 0x40]["live_out"], [0, 1, 3])


class Stops(unittest.TestCase):
    def test_terminating_stop_reads_nothing(self):
        f = Image().put(0, call_then()).put(1, leaf).analyze()[BASE + 0x40]
        self.assertEqual(f["live_out"], [1])

    def test_resuming_stop_reads_every_register(self):
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), G.stop(STOP_RESUME), G.stop(STOP_END)]
        f = Image().put(0, main).put(1, leaf).analyze()[BASE + 0x40]
        self.assertEqual(f["dead_regs"], [])

    def test_data_decoded_as_code_reads_every_register(self):
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), 0x0A000000, G.stop(STOP_END)]
        f = Image().put(0, main).put(1, leaf).analyze()[BASE + 0x40]
        self.assertEqual(f["dead_regs"], [])

    def test_computed_jump_reads_every_register(self):
        def main(pc):
            return [G.brsl(0, pc, BASE + Image.SLOT), G.bi(9), G.stop(STOP_END)]
        f = Image().put(0, main).put(1, leaf).analyze()[BASE + 0x40]
        self.assertEqual(f["dead_regs"], [])


def frame_of(*body, balanced=True, size=0x40):
    def build(pc):
        words = [G.ai(1, 1, -size), *body, G.il(3, 1)]
        if balanced:
            words.append(G.ai(1, 1, size))
        return [*words, G.bi(0)]
    return build


class Frame(unittest.TestCase):
    def call(self, build):
        return Image().put(0, call_then(G.wrch(28, 3))).put(1, build).analyze()[BASE + 0x40]

    def test_frame_of_the_prologue(self):
        f = self.call(frame_of(G.stqd(5, 1, 2)))
        self.assertEqual(f["dead_stack"], {"below_sp": 0x40})

    def test_generated_image(self):
        doc = L.analyze(G.image("ok", True), 0)
        callee = next(f for f in doc["functions"] if f["entry"] == f"0x{G.CALLEE:05x}")
        self.assertEqual(callee["dead_stack"], {"below_sp": G.FRAME})
        self.assertEqual(callee["live_out"], [1, 3])

    def test_no_frame(self):
        self.assertIsNone(self.call(leaf)["dead_stack"])

    def test_frame_not_given_back(self):
        self.assertIsNone(self.call(frame_of(balanced=False))["dead_stack"])

    def test_frame_given_back_with_another_amount(self):
        def build(pc):
            return [G.ai(1, 1, -0x40), G.ai(1, 1, 0x30), G.bi(0)]
        self.assertIsNone(self.call(build)["dead_stack"])

    def test_stack_pointer_rewritten_by_another_instruction(self):
        self.assertIsNone(self.call(frame_of(ori(1, 2, 0)))["dead_stack"])

    def test_indirect_branch_in_the_function(self):
        self.assertIsNone(self.call(frame_of(G.bi(9)))["dead_stack"])

    def test_frame_deeper_on_one_path(self):
        def build(pc):
            return [brz(5, pc, pc + 20), G.ai(1, 1, -0x20), G.ai(1, 1, -0x20), G.ai(1, 1, 0x40), G.bi(0), G.bi(0)]
        self.assertEqual(self.call(build)["dead_stack"], {"below_sp": 0x40})

    def test_paths_that_disagree_claim_nothing(self):
        def build(pc):
            # pc+4: join point reached with delta 0 (taken branch) and -0x20 (fall-through)
            return [brz(5, pc, pc + 12), G.ai(1, 1, -0x20), G.il(3, 0), G.bi(0)]
        self.assertIsNone(self.call(build)["dead_stack"])


class Decoder(unittest.TestCase):
    def test_every_mnemonic_has_a_rule(self):
        seen = set()
        for shift, width in ((28, 4), (25, 7), (24, 8), (23, 9), (22, 10), (21, 11)):
            for op in range(1 << width):
                seen.add(spu_disasm.spu_decode(op << shift).mnemonic)
        unknown = sorted(m for m in seen if m not in L.KNOWN and m != ".word")
        self.assertEqual(unknown, [])
        self.assertIn(spu_disasm.spu_decode((0x002 << 21) | 0x00100000).mnemonic, L.KNOWN)   # syncc

    def test_register_fields(self):
        class Ins:
            def __init__(self, mnemonic, raw):
                self.mnemonic, self.raw, self.addr = mnemonic, raw, 0x100

        bit = L._bit
        # selb writes the destination in bits 21-27 and reads RA, RB and the low seven bits
        raw = (0b1000 << 28) | (10 << 21) | (3 << 14) | (2 << 7) | 7
        self.assertEqual(L.describe(Ins("selb", raw), set())[1:3], (bit(2) | bit(3) | bit(7), bit(10)))
        # wrch reads RT; the channel number in RA is not a register
        self.assertEqual(L.describe(Ins("wrch", (0x10D << 21) | (28 << 7) | 9), set())[1:3], (bit(9), 0))
        # rdch writes RT and reads nothing
        self.assertEqual(L.describe(Ins("rdch", (0x00D << 21) | (3 << 7) | 9), set())[1:3], (0, bit(9)))
        # iohl and the accumulating forms read what they write
        self.assertEqual(L.describe(Ins("iohl", (0x0C1 << 23) | 5), set())[1:3], (bit(5), bit(5)))
        self.assertEqual(L.describe(Ins("addx", (0x340 << 21) | (3 << 14) | (2 << 7) | 1), set())[1:3],
                         (bit(1) | bit(2) | bit(3), bit(1)))
        # a store reads RT and writes nothing
        self.assertEqual(L.describe(Ins("stqd", (0x24 << 24) | (1 << 7) | 6), set())[1:3], (bit(6) | bit(1), 0))


class LinkBase(unittest.TestCase):
    """A job image linked at 0 and staged at BASE holds image-relative addresses."""

    @staticmethod
    def main(pc):
        # the leaf is called directly and its offset in the image is also formed: the code adds the
        # load delta to that immediate before branching, so it is a second way in
        return [G.brsl(0, pc, BASE + Image.SLOT), ila(9, Image.SLOT), G.wrch(28, 3), G.stop(STOP_END)]

    def test_image_relative_immediate_takes_the_entry_address(self):
        img = Image().put(0, self.main).put(1, leaf)
        plain = img.analyze()[BASE + 0x40]
        self.assertFalse(plain["unknown_callers"])
        self.assertEqual(plain["unknown_reasons"], [])
        linked = img.analyze(link_base=0)[BASE + 0x40]
        self.assertTrue(linked["unknown_callers"])
        self.assertEqual(linked["unknown_reasons"], ["address-taken"])
        self.assertEqual(linked["live_out"], list(range(128)))
        self.assertEqual(linked["dead_regs"], [])

    def test_image_relative_data_word_takes_the_entry_address(self):
        img = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf)
        img.words[-1] = Image.SLOT          # a function pointer slot holding the offset
        self.assertFalse(img.analyze()[BASE + 0x40]["unknown_callers"])
        self.assertIn("address-taken", img.analyze(link_base=0)[BASE + 0x40]["unknown_reasons"])

    def test_link_base_equal_to_base_changes_nothing(self):
        img = Image().put(0, self.main).put(1, leaf)
        self.assertEqual(img.analyze(link_base=BASE), img.analyze())

    def test_a_link_base_past_the_image_base_is_refused(self):
        img = Image().put(0, leaf)
        for link_base in (BASE + 4, -16):
            with self.assertRaises(ValueError):
                L.analyze(img.data(), BASE, functions=img.bounds(), link_base=link_base)

    def test_detected_bounds_follow_the_link_base(self):
        # the generator's position-independent job: four table-relative jump cases and a function
        # reached through an image-relative `ila`, none named by an absolute address
        from gen_test_raw_job import build, pic_layout
        blob = build(variant="pic-jump-table")
        layout = pic_layout()
        wanted = set(layout["cases"]) | {layout["pointer"]}
        def entries(doc):
            return {int(f["entry"], 16) for f in doc["functions"]}
        self.assertEqual(wanted & entries(L.analyze(blob, BASE)), set())
        doc = L.analyze(blob, BASE, link_base=0)
        self.assertEqual(wanted - entries(doc), set())
        pointer = next(f for f in doc["functions"] if int(f["entry"], 16) == layout["pointer"])
        self.assertIn("address-taken", pointer["unknown_reasons"])
        self.assertEqual(doc["image"]["link_base"], "0x00000")
        self.assertNotIn("link_base", L.analyze(blob, BASE)["image"])

    def test_cli_link_base(self):
        from gen_test_raw_job import build, pic_layout
        layout = pic_layout()
        with tempfile.TemporaryDirectory(prefix="spu-liveness-") as tmp:
            path = os.path.join(tmp, "pic.bin")
            out = os.path.join(tmp, "live.json")
            with open(path, "wb") as f:
                f.write(build(variant="pic-jump-table"))
            self.assertEqual(L.main([path, "--base", hex(BASE), "--link-base", "0", "-o", out]), 0)
            with open(out, encoding="utf-8") as f:
                doc = json.load(f)
            self.assertIn(f"0x{layout['pointer']:05x}", {f["entry"] for f in doc["functions"]})
            self.assertEqual(L.main([path, "--base", hex(BASE), "--link-base", hex(BASE + 16), "-o", out]), 1)
            with self.assertRaises(SystemExit):   # an ELF carries its own base
                L.main(["--elf", path, "--link-base", "0"])


class Document(unittest.TestCase):
    def test_json_document_and_cli(self):
        with tempfile.TemporaryDirectory(prefix="spu-liveness-") as tmp:
            blob = os.path.join(tmp, "img.bin")
            out = os.path.join(tmp, "live.json")
            img = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf)
            with open(blob, "wb") as f:
                f.write(img.data())
            funcs = os.path.join(tmp, "funcs.json")
            with open(funcs, "w", encoding="utf-8") as f:
                json.dump([{"start": hex(s), "end": hex(e)} for s, e in img.bounds()], f)
            proc = subprocess.run([sys.executable, os.path.join(TOOLS, "spu_liveness.py"), blob, "--base", hex(BASE),
                                   "--functions", funcs, "--code-end", hex(BASE + 0x100), "-o", out, "--summary"],
                                  check=False, capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("functions 2", proc.stderr)
            with open(out, encoding="utf-8") as f:
                doc = json.load(f)
        self.assertEqual(doc["schema"], "ps3recomp.spu-liveness/1")
        self.assertEqual(doc["image"]["ls_base"], f"0x{BASE:05x}")
        self.assertEqual(doc["image"]["bytes"], len(img.data()))
        self.assertEqual(doc["image"]["code_end"], f"0x{BASE + 0x100:05x}")
        self.assertEqual(doc["image"]["fingerprint"], f"0x{L.fnv1a64(img.data()):016x}")
        keys = {"entry", "end", "exits", "tail_calls", "live_out", "dead_regs", "dead_stack", "callers",
                "tail_callers", "unknown_callers", "unknown_reasons"}
        for f in doc["functions"]:
            self.assertEqual(set(f), keys)

    def test_elf_input_takes_bounds_base_and_entry_from_the_elf(self):
        code = G.image("ok", True)
        elf = wrap(code, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": 0x14}])
        with tempfile.TemporaryDirectory(prefix="spu-liveness-") as tmp:
            path = os.path.join(tmp, "job.elf")
            with open(path, "wb") as f:
                f.write(elf)
            out = os.path.join(tmp, "live.json")
            self.assertEqual(L.main(["--elf", path, "-o", out]), 0)
            with open(out, encoding="utf-8") as f:
                doc = json.load(f)
            with self.assertRaises(SystemExit):   # the ELF's own base is not overridable
                L.main(["--elf", path, "--base", "0x100", "-o", out])
            self.assertEqual(L.main(["--elf", os.path.join(tmp, "missing.elf")]), 1)   # an answer, not a traceback
            junk = os.path.join(tmp, "junk.elf")
            with open(junk, "wb") as f:
                f.write(b"not an elf at all")
            self.assertEqual(L.main(["--elf", junk]), 1)
        funcs = {f["entry"]: f for f in doc["functions"]}
        self.assertEqual(funcs["0x00000"]["unknown_reasons"], ["entry-point", "no-caller"])
        self.assertEqual(funcs[f"0x{G.CALLEE:05x}"]["live_out"], [1, 3])
        self.assertEqual(funcs[f"0x{G.CALLEE:05x}"]["dead_stack"], {"below_sp": G.FRAME})

    def test_bad_function_list_is_refused(self):
        img = Image().put(0, leaf)
        with self.assertRaises(ValueError):
            L.analyze(img.data(), BASE, functions=[(BASE, BASE + 8), (BASE + 4, BASE + 12)])
        with self.assertRaises(ValueError):
            L.analyze(img.data(), BASE, functions=[(BASE + 2, BASE + 8)])

    def test_code_end_caps_a_function(self):
        img = Image().put(0, call_then(G.wrch(28, 3))).put(1, leaf)
        docs = img.analyze(code_end=BASE + 0x48)
        self.assertEqual(docs[BASE + 0x40]["end"], f"0x{BASE + 0x48:05x}")

    def test_summary_line(self):
        doc = L.analyze(G.image("ok", True), 0)
        text = L.summary(doc)
        self.assertIn(f"functions {len(doc['functions'])}", text)
        self.assertIn("unknown callers", text)


if __name__ == "__main__":
    unittest.main()
