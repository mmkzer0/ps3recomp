#!/usr/bin/env python3
"""Everybody in spu_context has one class in the census (docs/SPU_XDIFF.md 9).

A shadow context is built from a fixture, never copied from a live one, so
every member of spu_context a job's entry state sets must be carried by the
fixture. runtime/spu/xdiff/spu_xdiff_census.c states that per member; this test
holds the table to the compiler:

  - the compiler prints spu_context's record layout (clang's
    -Xclang -fdump-record-layouts on a probe translation unit) and the table
    must list exactly those members, in declaration order, at those offsets;
  - a probe program linked with spu_xdiff_census.c prints the table, so the test
    also compares the numbers the running code carries, not the source text;
  - spu_xdiff_context_census() must be the documented FNV-1a-32 over
    sizeof(spu_context) and each (offset, class), recomputed here.

A member added to spu_context fails this test until someone gives it a class.
A compiler that cannot print a layout skips the test visibly, with its reason.

Usage:  python3 tools/test_spu_context_census.py
"""

import re
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))

from host_cc import EXE, c_compiler

SPU = ROOT / 'runtime' / 'spu'
CENSUS_C = SPU / 'xdiff' / 'spu_xdiff_census.c'
INCLUDES = [ROOT / 'include', ROOT / 'runtime', SPU, SPU / 'xdiff']

# spu_xdiff_member_class, in the header's numbering.
CLASSES = {1: 'carried', 2: 'zero', 3: 'host'}

PROBE_MAIN = '''\
#include "spu_context.h"
#include "spu_xdiff_census.h"
#include <stdio.h>

int main(void)
{
    size_t count = 0;
    const spu_xdiff_member* members = spu_xdiff_context_members(&count);
    printf("sizeof %u\\n", (unsigned)sizeof(spu_context));
    for (size_t i = 0; i < count; i++)
        printf("member %s %u %u\\n", members[i].name, (unsigned)members[i].offset,
               (unsigned)members[i].klass);
    printf("census %08x\\n", (unsigned)spu_xdiff_context_census());
    return 0;
}
'''

PROBE_LAYOUT = '''\
#include "spu_context.h"
/* sizeof forces the record's layout, which is what the dump needs. */
const unsigned long census_probe_bytes = sizeof(spu_context);
'''

DUMP_HEADER = re.compile(r'^\s*\d+ \|\s+(?:struct|class|union) spu_context\s*$')
DUMP_MEMBER = re.compile(r'^\s*(\d+) \|\s{3}(\S.*)$')
DUMP_SIZE = re.compile(r'^\s*\| \[sizeof=(\d+), align=(\d+)\]')
ANY_RECORD = re.compile(r'^\s*\d+ \|')


def member_name(rest):
    """The member's name from a dump line's type-and-name text: the last
    identifier, with a bitfield width or an array suffix on the name removed.
    A member of an anonymous struct type reads `struct (unnamed struct at
    path:line:col)[4] resident_code`, whose last identifier is the name."""
    rest = re.sub(r'\s*:\s*\d+(-\d+)?$', '', rest)
    rest = re.sub(r'\[[^\]]*\]\s*$', '', rest)
    names = re.findall(r'[A-Za-z_]\w*', rest)
    return names[-1] if names else ''


def parse_layout(text):
    """(members, sizeof) for spu_context from a record-layout dump, or
    (None, None) when the dump has no such record. Nested records are their own
    blocks; only the top-level fields (three spaces after the bar) count."""
    lines = text.splitlines()
    start = None
    for i, line in enumerate(lines):
        if DUMP_HEADER.match(line):
            start = i + 1
            break
    if start is None:
        return None, None
    members = []
    size = None
    for line in lines[start:]:
        found = DUMP_SIZE.match(line)
        if found:
            size = int(found.group(1))
            break
        found = DUMP_MEMBER.match(line)
        if found:
            members.append((member_name(found.group(2)), int(found.group(1))))
            continue
        if not ANY_RECORD.match(line):
            break
    return members, size


def fnv1a32_u32(fields):
    h = 2166136261  # FNV-1a-32 offset basis
    for value in fields:
        for byte in range(4):
            h ^= (value >> (8 * byte)) & 0xFF
            h = (h * 16777619) & 0xFFFFFFFF
    return h


class ContextCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = c_compiler()
        if cc is None:
            raise unittest.SkipTest('no C compiler found (CC, or cc/clang/gcc on PATH)')

        with tempfile.TemporaryDirectory(prefix='spu-context-census-') as tmp_name:
            tmp = Path(tmp_name)

            # 1. the compiler's own layout of spu_context
            probe = tmp / 'census_layout.c'
            probe.write_text(PROBE_LAYOUT, encoding='utf-8')
            if cc.msvc:
                flags = ['/clang:-Xclang', '/clang:-fdump-record-layouts', '/Zs']
            else:
                flags = ['-Xclang', '-fdump-record-layouts', '-fsyntax-only']
            result = cc.run(cc.exe([probe], tmp / 'census_layout.out', std='c17',
                                   includes=INCLUDES, flags=flags))
            dump = (result.stdout or '') + (result.stderr or '')
            members, size = parse_layout(dump)
            if members is None:
                detail = (result.stderr or result.stdout or '').strip().splitlines()
                reason = detail[-1] if detail else f'exit status {result.returncode}'
                raise unittest.SkipTest(
                    f'{Path(cc.argv[0]).name} cannot print a record layout ({reason}); this '
                    'test needs clang\'s -Xclang -fdump-record-layouts')
            cls.layout = members
            cls.layout_size = size

            # 2. the table the compiled code carries
            source = tmp / 'census_probe.c'
            source.write_text(PROBE_MAIN, encoding='utf-8')
            exe = tmp / ('census_probe' + EXE)
            built = cc.run(cc.exe([CENSUS_C, source], exe, std='c17', includes=INCLUDES))
            if built.returncode != 0:
                raise AssertionError('the census probe does not build:\n'
                                     + (built.stderr or built.stdout)[-3000:])
            ran = cc.run([str(exe)])
            if ran.returncode != 0:
                raise AssertionError('the census probe does not run:\n'
                                     + (ran.stderr or ran.stdout)[-3000:])
            table, printed_size, census = [], None, None
            for line in ran.stdout.splitlines():
                fields = line.split()
                if fields[:1] == ['sizeof'] and len(fields) == 2:
                    printed_size = int(fields[1])
                elif fields[:1] == ['member'] and len(fields) == 4:
                    table.append((fields[1], int(fields[2]), int(fields[3])))
                elif fields[:1] == ['census'] and len(fields) == 2:
                    census = int(fields[1], 16)
            if not table or printed_size is None or census is None:
                raise AssertionError('the census probe printed no table:\n'
                                     + ran.stdout[-3000:])
        cls.table = table
        cls.sizeof = printed_size
        cls.census = census

    def test_members_are_the_compilers(self):
        """Every member, in declaration order, at the compiler's offset."""
        names = [name for name, _, _ in self.table]
        table_offsets = {name: offset for name, offset, _ in self.table}
        compiler_names = [name for name, _ in self.layout]
        compiler_offsets = dict(self.layout)
        missing = [n for n in compiler_names if n not in table_offsets]
        extra = [n for n in names if n not in compiler_offsets]
        order = [f'{i}: {have} != {want}'
                 for i, (have, want) in enumerate(zip(names, compiler_names)) if have != want]
        offsets = [f'{name}: {table_offsets[name]} != {compiler_offsets[name]}'
                   for name in names if name in compiler_offsets
                   and table_offsets[name] != compiler_offsets[name]]
        self.assertEqual(
            (missing, extra, order, offsets), ([], [], [], []),
            'runtime/spu/xdiff/spu_xdiff_census.c does not match the compiler\'s layout of '
            'spu_context:\n'
            f'  missing from the table (classify them): {missing}\n'
            f'  not members of spu_context: {extra}\n'
            f'  out of declaration order: {order}\n'
            f'  at another offset: {offsets}')
        self.assertEqual(self.sizeof, self.layout_size,
                         'the probe and the layout dump compiled different spu_context headers '
                         f'({self.sizeof} != {self.layout_size} bytes)')

    def test_every_member_has_a_known_class(self):
        bad = [f'{name} ({klass})' for name, _, klass in self.table if klass not in CLASSES]
        self.assertEqual(bad, [], 'spu_xdiff_census.c gives these members no class of '
                                  'spu_xdiff_member_class: ' + ', '.join(bad))

    def test_census_is_the_documented_hash(self):
        """FNV-1a-32 over sizeof and each (offset, class), low byte first."""
        fields = [self.sizeof]
        for _, offset, klass in self.table:
            fields += [offset, klass]
        self.assertEqual(self.census, fnv1a32_u32(fields),
                         f'spu_xdiff_context_census() is 0x{self.census:08x}, the documented hash of '
                         f'this table is 0x{fnv1a32_u32(fields):08x}')


if __name__ == '__main__':
    unittest.main()
