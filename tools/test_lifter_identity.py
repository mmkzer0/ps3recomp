#!/usr/bin/env python3
"""The lifter records the SHA-256 of the file it lifted, as ppu_lift_image_sha256().

The runtime's identity gate (ppu_load_elf) compares the executable it is asked to
load against that function, so the lifter must record the whole input file, must
declare the function in the header it generates, must not write output without
an identity, and must keep a symbol prefix (a relocated PRX linking beside the
main image) from colliding.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ppu_lifter import LiftedFunction, PPULifter  # noqa: E402

BLR = bytes.fromhex('4e800020')


def lift(root, data, *extra):
    """Run the lifter CLI on `data` (a raw one-function image); return its output dir."""
    (root / 'code.bin').write_bytes(data)
    (root / 'functions.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10004'}]))
    output = root / ('out%d' % len(list(root.glob('out*'))))
    subprocess.run([sys.executable, str(Path(__file__).with_name('ppu_lifter.py')),
                    str(root / 'code.bin'), '--raw', '--base', '0x10000',
                    '--functions', str(root / 'functions.json'), '--jobs', '1',
                    '--output', str(output), *extra],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return output


def recorded(output, source='ppu_recomp.c'):
    text = (output / source).read_text()
    m = re.search(r'const char\* (\w*ppu_lift_image_sha256)\(void\) \{ return "([0-9a-f]{64})"; \}', text)
    assert m, 'no ppu_lift_image_sha256 in ' + source
    return m.group(1), m.group(2)


def test_cli_records_the_hash_of_the_whole_input_file():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        # Bytes past the lifted function are not lifted, and still change the identity.
        for data in (BLR, BLR + b'\x00', BLR + b'\x01'):
            out = lift(root, data, '--single-file')
            name, digest = recorded(out)
            assert name == 'ppu_lift_image_sha256'
            assert digest == hashlib.sha256(data).hexdigest(), (data, digest)
            assert 'const char* ppu_lift_image_sha256(void);' in (out / 'ppu_recomp.h').read_text()


def test_split_output_carries_it_in_the_table_chunk():
    with tempfile.TemporaryDirectory() as tmp:
        want = hashlib.sha256(BLR).hexdigest()
        out = lift(Path(tmp), BLR, '--chunks', '1')
        tables = list(out.glob('ppu_recomp_table.*'))
        assert len(tables) == 1, tables
        assert recorded(out, tables[0].name)[1] == want
        # the default line-count split puts the table in the last chunk
        out = lift(Path(tmp), BLR)
        assert recorded(out, sorted(p.name for p in out.glob('ppu_recomp_*.cpp'))[-1])[1] == want


def test_identical_input_gives_identical_output_so_a_relift_leaves_it_untouched():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        a = (lift(root, BLR, '--single-file') / 'ppu_recomp.c').read_text()
        b = (lift(root, BLR, '--single-file') / 'ppu_recomp.c').read_text()
        assert a == b


def test_a_symbol_prefix_prefixes_the_function_in_header_and_source():
    with tempfile.TemporaryDirectory() as tmp:
        out = lift(Path(tmp), BLR, '--single-file', '--symbol-prefix', 'libsre_')
        assert recorded(out)[0] == 'libsre_ppu_lift_image_sha256'
        assert 'const char* libsre_ppu_lift_image_sha256(void);' in (out / 'ppu_recomp.h').read_text()
        assert 'const char* ppu_lift_image_sha256(void);' not in (out / 'ppu_recomp.h').read_text()


def make_lifter():
    lf = PPULifter()
    lf.functions.append(LiftedFunction(name='func_00010000', start_addr=0x10000, end_addr=0x10004,
                                       body_lines=['return;']))
    return lf


def test_no_output_without_an_image_identity():
    for bad in (None, '', 'ab' * 31, 'AB' * 32, 'zz' * 32):
        lf = make_lifter()
        lf.image_sha256 = bad
        try:
            lf.emit_source()
        except ValueError as e:
            assert 'image_sha256' in str(e)
        else:
            raise AssertionError(f'wrote output with image_sha256={bad!r}')
    lf = make_lifter()
    lf.image_sha256 = '5a' * 32
    assert 'return "' + '5a' * 32 + '";' in lf.emit_source()


if __name__ == '__main__':
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith('test_'):
            try:
                fn()
                print('PASS', name)
            except Exception as e:  # noqa: BLE001
                failed += 1
                print('FAIL', name, e, file=sys.stderr)
    sys.exit(1 if failed else 0)
