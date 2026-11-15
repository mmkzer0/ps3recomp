#!/usr/bin/env python3
"""tools/find_spu_functions.py on position-independent raw job images.

A JobQueue2 job image is linked at 0 and staged at LS 0x7C00; its code adds the
load delta to image-relative immediates. With a `link_base` every immediate and
data word that can name an address is read both ways. The synthetic images come
from tools/gen_test_raw_job.py (`pic-jump-table`) or are built here.

Usage:  python3 tools/test_find_spu_functions.py
"""
import os
import sys
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import find_spu_functions as F
from gen_test_raw_job import (
    BI,
    CODE_START,
    IL,
    ILA,
    LS_BASE,
    _be32,
    _ri16,
    _ri18,
    _rr,
    build,
    pic_layout,
)
from spu_disasm import disassemble_spu

DELTA = LS_BASE            # an image linked at 0 and staged at LS_BASE


def starts(blob, link_base=None):
    bounds, _ = F.detect_functions(blob, verbose=False, raw=True, base_override=LS_BASE,
                                   link_base=link_base)
    return {start for start, _end in bounds}


def raw_phs(blob):
    return [{"type": 1, "flags": 1, "off": 0, "vaddr": LS_BASE, "filesz": len(blob), "memsz": len(blob)}]


def returning(value):
    """`il $r3, value; bi $r0`: a two-word function."""
    return [_ri16(IL, value, 3), _rr(BI, 0, 0, 0)]


class Readings(unittest.TestCase):
    def test_no_delta_is_the_value_itself(self):
        self.assertEqual(F.image_delta(LS_BASE, None), 0)
        self.assertEqual(F.immediate_ls_addresses(0x100, 0), (0x100,))
        self.assertEqual(F.immediate_values_naming(0x7D00, 0), (0x7D00,))

    def test_delta_names_both_readings(self):
        self.assertEqual(F.image_delta(LS_BASE, 0), DELTA)
        self.assertEqual(F.immediate_ls_addresses(0x100, DELTA), (0x100, 0x7D00))

    def test_naming_is_the_inverse_of_addresses(self):
        # The resolver asks which values name an address; the finder asks which
        # addresses a value names. They must be one relation, or an address the
        # finder seeds could be one the resolver does not see as taken.
        for delta in (0, 0x10, 0x7C00):
            for value in (0, 4, 0x100, 0x3F000, 0xFFFFFFF0):
                for address in F.immediate_ls_addresses(value, delta):
                    self.assertIn(value, F.immediate_values_naming(address, delta))
            for address in (0x7C00, 0x7D00, 0x3FFFC):
                for value in F.immediate_values_naming(address, delta):
                    self.assertIn(address, F.immediate_ls_addresses(value, delta))


class PicJumpTable(unittest.TestCase):
    def setUp(self):
        self.blob = build(variant="pic-jump-table")
        self.layout = pic_layout()
        self.insns = disassemble_spu(self.blob, LS_BASE)
        self.end = LS_BASE + len(self.blob)

    def test_table_is_invisible_without_a_delta(self):
        found = F.collect_jump_table_targets(self.insns, self.blob, LS_BASE, self.end)
        self.assertEqual(found & set(self.layout["cases"]), set())

    def test_table_relative_cases_resolve_against_the_relocated_base(self):
        found = F.collect_jump_table_targets(self.insns, self.blob, LS_BASE, self.end, delta=DELTA)
        self.assertEqual(set(self.layout["cases"]) - found, set())

    def test_function_pointer_immediate_needs_the_delta(self):
        pointer = self.layout["pointer"]
        self.assertNotIn(pointer, F.collect_ila_continuation_targets(self.insns, LS_BASE, self.end))
        self.assertIn(pointer, F.collect_ila_continuation_targets(self.insns, LS_BASE, self.end,
                                                                  delta=DELTA))

    def test_detector_seeds_cases_and_pointer_only_with_a_link_base(self):
        wanted = set(self.layout["cases"]) | {self.layout["pointer"]}
        self.assertEqual(wanted & starts(self.blob), set())
        self.assertEqual(wanted - starts(self.blob, link_base=0), set())

    def test_a_link_base_only_adds_starts(self):
        self.assertLessEqual(starts(self.blob), starts(self.blob, link_base=0))
        # A link base equal to the staging address is today's reading.
        self.assertEqual(starts(self.blob, link_base=LS_BASE), starts(self.blob))

    def test_image_relative_seeds_are_decodable_code(self):
        # `value + delta` of any small number reaches the job header's marker
        # text, which is data: the lifter refuses the image for every such word.
        undecoded = {i.addr for i in self.insns if i.mnemonic == F.UNDECODED}
        self.assertTrue(undecoded, "the synthetic header has no data word to land on")
        self.assertEqual(starts(self.blob, link_base=0) & undecoded, set())


class ImageRelativeTables(unittest.TestCase):
    """Tables whose words are image-relative handler offsets, not table-relative."""

    def image(self):
        # 0x00 ila $r14, <table - base> ; 0x04 bi $r0 ; table at 0x08 ; two handlers
        table = 0x08
        handlers = [0x10, 0x18]
        words = [_ri18(ILA, table, 14), _rr(BI, 0, 0, 0)] + handlers
        words += returning(1) + returning(2)
        return b"".join(_be32(w) for w in words), [LS_BASE + h for h in handlers]

    def test_jump_table_of_image_relative_offsets(self):
        blob, handlers = self.image()
        self.assertEqual(set(handlers) & starts(blob), set())
        self.assertEqual(set(handlers) - starts(blob, link_base=0), set())

    def test_function_pointer_run_of_image_relative_words(self):
        # Three offsets in a data run, no `ila` anywhere: only the data scan
        # can see them, and only when its words are read as image-relative.
        filler = [_rr(BI, 0, 0, 0)] * 8                 # 0x00..0x1C
        offsets = [0x2C, 0x34, 0x3C]                    # the table sits at 0x20
        words = filler + offsets + returning(1) + returning(2) + returning(3)
        blob = b"".join(_be32(w) for w in words)
        by_addr = {i.addr: i for i in disassemble_spu(blob, LS_BASE)}
        end = LS_BASE + len(blob)
        targets = {LS_BASE + o for o in offsets}
        self.assertLessEqual(targets, set(by_addr))
        self.assertEqual(
            F.collect_function_pointer_tables(blob, raw_phs(blob), LS_BASE, end, by_addr) & targets,
            set())
        self.assertEqual(
            F.collect_function_pointer_tables(blob, raw_phs(blob), LS_BASE, end, by_addr,
                                              delta=DELTA) & targets,
            targets)

    def test_function_pointer_run_into_data_is_not_seeded(self):
        # Offsets that land on undecodable words (the job header's marker text)
        # are counters, not pointers: nothing is seeded.
        blob = build()
        text = [i.addr - LS_BASE for i in disassemble_spu(blob, LS_BASE)
                if i.mnemonic == F.UNDECODED and i.addr < LS_BASE + CODE_START]
        self.assertGreaterEqual(len(text), 3)
        blob += b"".join(_be32(o) for o in text[:3])
        by_addr = {i.addr: i for i in disassemble_spu(blob, LS_BASE)}
        targets = {LS_BASE + o for o in text[:3]}
        self.assertLessEqual(targets, set(by_addr))     # each is a word the scan could take
        found = F.collect_function_pointer_tables(blob, raw_phs(blob), LS_BASE, LS_BASE + len(blob),
                                                  by_addr, delta=DELTA)
        self.assertEqual(found & targets, set())


class CleanRuns(unittest.TestCase):
    def test_run_to_a_terminator_is_clean_and_to_data_is_not(self):
        data = 0x4A4F4243                               # "JOBC": no instruction
        words = returning(1) + [data] + returning(2)
        blob = b"".join(_be32(w) for w in words)
        by_addr = {i.addr: i for i in disassemble_spu(blob, LS_BASE)}
        self.assertEqual(by_addr[LS_BASE + 8].mnemonic, F.UNDECODED)
        self.assertTrue(F.starts_clean_run(LS_BASE, by_addr))
        self.assertFalse(F.starts_clean_run(LS_BASE + 8, by_addr))
        # a run that falls through into data is not clean either
        fall = b"".join(_be32(w) for w in [_ri16(IL, 1, 3), data, _rr(BI, 0, 0, 0)])
        by_addr = {i.addr: i for i in disassemble_spu(fall, LS_BASE)}
        self.assertFalse(F.starts_clean_run(LS_BASE, by_addr))


if __name__ == "__main__":
    unittest.main()
