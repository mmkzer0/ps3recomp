#!/usr/bin/env python3
"""ppu_loader.cpp's store hook runs before a guest store of ANY form into its window lands.

Builds the real runtime/ppu/ppu_loader.cpp with runtime/ppu/tests/test_ppu_store_hook.cpp and with the code
the real lifter (tools/ppu_lifter.py) emits for each guest store form: the integer stores, the multiple-word
and string stores, the byte-reversed stores, the store-conditionals, the vector stores, dcbz, and the float
stores. Each form stores into a window the test registers; the hook has to run before the bytes land, once per
store, for a store that overlaps the window by even one byte, and not for a store next to it.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ppu_disasm  # noqa: E402
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources, stub_header  # noqa: E402
from ppu_lifter import LiftedFunction, PPULifter  # noqa: E402

ROOT = TOOLS.parent


def x31(xo, rs, ra, rb, rc=0):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def d_form(op, rs, ra, imm=0):
    return (op << 26) | (rs << 21) | (ra << 16) | (imm & 0xFFFF)


# (mnemonic, word, kind, ea offset from the probe base, first stored byte, stored length, hook calls)
#   kind 0: plain store; 1: stwcx. (a lwarx reserves first); 2: stdcx. (an ldarx reserves first)
#   The hook runs once per store, but a multiple-word store is one word store per register and a string
#   store one byte store per byte, so those run it once per element.
# Registers: r4 holds the EA (X forms: rA = 0, rB = r4; update X forms: rA = r4, rB = r7 = 0), r5 the data,
# v5 / f5 the vector / float data.
FORMS = [
    ('stb',    d_form(38, 5, 4),   0,  5,  5,   1,   1),
    ('sth',    d_form(44, 5, 4),   0,  3,  3,   2,   1),
    ('stw',    d_form(36, 5, 4),   0,  6,  6,   4,   1),
    ('std',    d_form(62, 5, 4),   0,  4,  4,   8,   1),
    ('stfs',   d_form(52, 5, 4),   0,  6,  6,   4,   1),
    ('stfd',   d_form(54, 5, 4),   0,  4,  4,   8,   1),
    ('stbu',   d_form(39, 5, 4),   0,  5,  5,   1,   1),
    ('sthu',   d_form(45, 5, 4),   0,  3,  3,   2,   1),
    ('stwu',   d_form(37, 5, 4),   0,  6,  6,   4,   1),
    ('stdu',   d_form(62, 5, 4, 1), 0, 4,  4,   8,   1),
    ('stfsu',  d_form(53, 5, 4),   0,  6,  6,   4,   1),
    ('stfdu',  d_form(55, 5, 4),   0,  4,  4,   8,   1),
    ('stbx',   x31(215, 5, 0, 4),  0,  5,  5,   1,   1),
    ('sthx',   x31(407, 5, 0, 4),  0,  3,  3,   2,   1),
    ('stwx',   x31(151, 5, 0, 4),  0,  6,  6,   4,   1),
    ('stdx',   x31(149, 5, 0, 4),  0,  4,  4,   8,   1),
    ('stbux',  x31(247, 5, 4, 7),  0,  5,  5,   1,   1),
    ('sthux',  x31(439, 5, 4, 7),  0,  3,  3,   2,   1),
    ('stwux',  x31(183, 5, 4, 7),  0,  6,  6,   4,   1),
    ('stdux',  x31(181, 5, 4, 7),  0,  4,  4,   8,   1),
    ('stfsx',  x31(663, 5, 0, 4),  0,  6,  6,   4,   1),
    ('stfdx',  x31(727, 5, 0, 4),  0,  4,  4,   8,   1),
    ('stfsux', x31(695, 5, 4, 7),  0,  6,  6,   4,   1),
    ('stfdux', x31(759, 5, 4, 7),  0,  4,  4,   8,   1),
    ('stfiwx', x31(983, 5, 0, 4),  0,  6,  6,   4,   1),
    ('stmw',   d_form(47, 29, 4),  0,  8,  8,   12,  3),
    ('stswi',  x31(725, 5, 4, 7),  0,  3,  3,   7,   7),
    ('stswx',  x31(661, 5, 0, 4),  0,  3,  3,   7,   7),
    ('stwcx.', x31(150, 5, 0, 4, 1), 1, 8, 8,   4,   1),
    ('stdcx.', x31(214, 5, 0, 4, 1), 2, 8, 8,   8,   1),
    ('stwbrx', x31(662, 5, 0, 4),  0,  6,  6,   4,   1),
    ('sthbrx', x31(918, 5, 0, 4),  0,  5,  5,   2,   1),
    ('stdbrx', x31(660, 5, 0, 4),  0,  4,  4,   8,   1),
    ('stvx',   x31(231, 5, 0, 4),  0,  23, 16,  16,  1),
    ('stvxl',  x31(487, 5, 0, 4),  0,  23, 16,  16,  1),
    ('stvlx',  x31(647, 5, 0, 4),  0,  20, 20,  12,  1),
    ('stvlxl', x31(903, 5, 0, 4),  0,  20, 20,  12,  1),
    ('stvrx',  x31(679, 5, 0, 4),  0,  20, 16,  4,   1),
    ('stvrxl', x31(935, 5, 0, 4),  0,  20, 16,  4,   1),
    ('stvebx', x31(135, 5, 0, 4),  0,  21, 21,  1,   1),
    ('stvehx', x31(167, 5, 0, 4),  0,  21, 20,  2,   1),
    ('stvewx', x31(199, 5, 0, 4),  0,  23, 20,  4,   1),
    ('dcbz',   x31(1014, 0, 0, 4), 0,  0x30, 0, 128, 1),
]


def forms_source():
    lifter = PPULifter()
    dummy = LiftedFunction(name='forms', start_addr=0, end_addr=0x10000)
    out = ['#include "ppu_recomp.h"', '#include <string.h>', 'extern "C" {',
           'typedef struct { const char* name; void (*run)(ppu_context*); int kind; '
           'unsigned ea_off, first, len, calls; } store_form;']
    rows = []
    for i, (mn, word, kind, ea_off, first, length, calls) in enumerate(FORMS):
        insn = ppu_disasm.decode(word, 0x10000 + 4 * i)
        if insn.mnemonic.rstrip('.') != mn.rstrip('.'):
            raise SystemExit(f'FAIL: the test encodes {mn} as {word:#010x}, which decodes as {insn.mnemonic}')
        code = lifter._translate(insn, dummy)
        if code.lstrip().startswith('/*'):
            raise SystemExit(f'FAIL: the lifter does not lower {mn}: {code[:80]}')
        out.append(f'static void run_{i}(ppu_context* ctx) {{ {code} }}')
        rows.append(f'    {{"{mn}", run_{i}, {kind}, {ea_off}u, {first}u, {length}u, {calls}u}},')
    out += ['extern const store_form g_forms[] = {', *rows, '};',
            f'extern const unsigned g_nforms = {len(FORMS)};', '}']
    return '\n'.join(out) + '\n'


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX linking')
        return
    with tempfile.TemporaryDirectory(prefix='ppu-store-hook-') as tmp:
        tmp = Path(tmp)
        (tmp / 'ppu_recomp.h').write_text(stub_header())
        (tmp / 'store_forms.cpp').write_text(forms_source())
        binary = tmp / 'test'
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w', *san,
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                        str(ROOT / 'runtime/ppu/tests/test_ppu_store_hook.cpp'),
                        str(tmp / 'store_forms.cpp'),
                        *loader_support_sources(), *lifter_abi_sources(), *loader_support_objects(tmp),
                        *flags, '-o', str(binary)], check=True)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
        r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)
        print(r.stdout, end='')
        if r.returncode:
            sys.stderr.write(r.stderr[-2000:])
            raise SystemExit('FAIL: the guest store hook')
        print(f'PASS: the guest store hook runs before every store form ({len(FORMS)} forms) into its window')


if __name__ == '__main__':
    main()
