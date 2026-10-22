#!/usr/bin/env python3
"""Every gap the loader used to paper over is a named stop.

Lifts a synthetic image with the real ppu_lifter.py (one function that is fine,
one that contains an instruction the lifter cannot translate), builds the real
ppu_loader.cpp against that lift, and runs tests/test_loader_gaps.cpp:

  - an lv2 syscall nobody implements: stop unimplemented-syscall, naming the
    number; allow-listed in [[lv2.stub]], it returns exactly the configured value;
  - a call into a function the lift does not contain: unlifted-function, naming
    its address and the caller;
  - an indirect call to no function: unresolved-indirect-call at the first hit;
  - the lifted function that runs up to the unknown instruction: it does not stop
    before it, it stops AT it (unknown-instruction, with the pc and word);
  - the lift report lists that instruction.

A line "RETURNED" from the fixture means the runtime carried on; every case that
must stop treats it as failure.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests' / 'gates'))
from check_ppu_scaffold import loader_support_sources  # noqa: E402
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# A: li r3,5 ; blr          B: li r3,7 ; .word 0 (no instruction) ; blr
CODE = struct.pack('>IIIII', 0x38600005, 0x4E800020, 0x38600007, 0x00000000, 0x4E800020)


def main():
    if os.name == 'nt':
        print('SKIP: POSIX fixture')
        return 0
    with tempfile.TemporaryDirectory(prefix='ppu-gaps-') as tmp:
        tmp = Path(tmp)
        (tmp / 'code.bin').write_bytes(CODE)
        (tmp / 'fn.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10008'},
                                                 {'start': '0x10008', 'end': '0x10014'}]))
        lift = tmp / 'lift'
        subprocess.run([sys.executable, str(ROOT / 'tools/ppu_lifter.py'), str(tmp / 'code.bin'), '--raw',
                        '--base', '0x10000', '--functions', str(tmp / 'fn.json'), '--jobs', '1',
                        '--single-file', '--source-name', 'ppu_recomp.cpp', '--output', str(lift)],
                       check=True, capture_output=True)
        flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
        binary = tmp / 'gaps'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-w', '-ffunction-sections',
                        '-fdata-sections', '-I' + str(lift), '-I' + str(ROOT / 'include'),
                        '-I' + str(ROOT / 'runtime/ppu'), '-I' + str(ROOT / 'runtime/platform'),
                        str(ROOT / 'runtime/ppu/ppu_loader.cpp'), str(ROOT / 'tests/test_loader_gaps.cpp'),
                        str(lift / 'ppu_recomp.cpp'), *loader_support_sources(), *flags, '-o', str(binary)],
                       check=True, timeout=300)

        def run(*args, cfg=None):
            env = g.clean_env(**({'PS3RECOMP_CONFIG': str(cfg)} if cfg else {}))
            return g.run([str(binary), *args], env=env)

        # the lift report records the instruction with its pc, word and text
        report = json.loads((lift / 'ppu_lift_report.json').read_text())
        assert report['summary']['trapped_instructions'] == 1, report
        fn = report['functions'][0]
        assert fn['name'] == 'func_00010008' and fn['traps'][0][:2] == ['0x0001000C', '0x00000000'], fn
        assert 'word' in fn['traps'][0][2], fn

        # a function that is fine runs; the one with the unknown instruction does not stop before it
        p = run('call', '0x10000')
        assert p.returncode == 0 and 'RETURNED r3=0x5' in p.stdout, (p.stdout, p.stderr)
        f = g.expect_stop(run('call', '0x10008'), 'unknown-instruction')
        assert (f['pc'], f['word']) == ('0x1000C', '0x00000000') and f['tid'] == '1', f

        # unimplemented syscall
        p = run('syscall', '999')
        assert 'RETURNED' not in p.stdout, 'the runtime answered an unimplemented syscall'
        f = g.expect_stop(p, 'unimplemented-syscall')
        assert f['num'] == '999' and f['caller_lr'] == '0x00010100', f
        cfg = tmp / 'title.toml'
        cfg.write_text('schema = 1\n[[lv2.stub]]\nnum = 999\nret = 0x80010003\nreason = "synthetic test gap"\n')
        p = run('syscall', '999', cfg=cfg)
        assert p.returncode == 0 and 'RETURNED r3=0xFFFFFFFF80010003' in p.stdout, (p.stdout, p.stderr)
        assert 'ALLOW-LISTED unimplemented syscall 999' in p.stderr and 'synthetic test gap' in p.stderr
        g.expect_stop(run('syscall', '998', cfg=cfg), 'unimplemented-syscall')   # only the listed number

        # call into a function this lift does not contain
        p = run('unlifted', '0x12340')
        assert 'RETURNED' not in p.stdout
        f = g.expect_stop(p, 'unlifted-function')
        assert f['target'] == '0x00012340' and f['pc'] == '0x12340' and f['caller_lr'] == '0x00010100', f

        # indirect call to nothing: stops at the first hit
        p = run('indirect', '0x00020000')
        assert 'RETURNED' not in p.stdout
        f = g.expect_stop(p, 'unresolved-indirect-call')
        assert f['target'] == '0x00020000' and 'descriptors' in f and 'target_words' in f, f
        f = g.expect_stop(run('indirect', '0x39800000'), 'unresolved-indirect-call')
        assert 'li r12,0' in f['hint'], f
        f = g.expect_stop(run('indirect', '0x20000000'), 'unresolved-indirect-call')
        assert 'data/heap' in f['hint'], f
    print('PASS: unimplemented syscall, unlifted call, unresolved indirect call and unknown instruction all stop')
    return 0


if __name__ == '__main__':
    sys.exit(main())
