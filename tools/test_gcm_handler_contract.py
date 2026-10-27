#!/usr/bin/env python3
"""The HLE's GCM handler delivery against the firmware's OPD contract (issue #70).

FW 4.93 libgcm_sys stores the registered OPD pointer and reads {code, toc} from guest memory at every
delivery; it has no repair path (dossier GCM-CALLBACK-OPD-CONTRACT.md §1, §2, §6 T1-T3). The fixture
libs/video/tests/test_gcm_handler_contract.c registers a synthetic lifted function as the flip and the
vblank handler and drives cellGcmSys.c's own pump, against the real runtime/ppu/ppu_loader.cpp:

  intact       both handlers run, r2 = the OPD's toc, the completed flip gets r3 = 1 (the dispatcher's
               0x10 slot); the request-time flip call (r3 = 0) and the vblank argument are pinned as
               implemented (#69 is to change them)
  zero-*       the code word zeroed after registration: the named stop unresolved-guest-callback, origin
               opd, code 0x00000000; the handler never runs and the pump never returns
  swap-*       {toc, code} swapped, ppu.opd_swap_fixup off: the same stop with code = the toc; the lifted
               function behind the swapped toc never runs

No title or firmware bytes: the OPDs and the lifted functions are synthetic. YDKJ_HANDLERFIX is unset and
ppu.opd_swap_fixup is off (the fixture checks it).
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests' / 'gates'))
from check_ppu_scaffold import lifter_abi_sources, loader_support_objects, loader_support_sources, stub_header  # noqa: E402
import gates_common as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

# The fixture's constants (test_gcm_handler_contract.c): OPD, code, toc per handler.
FLIP = dict(opd=0x20000, code=0x30000, toc=0x28000)
VBLANK = dict(opd=0x20010, code=0x30010, toc=0x29000)

# mode -> (handler, expected stop code word, expected stop toc word, descriptor prefix)
STOPS = {
    'zero-flip': (FLIP, 0, FLIP['toc']),
    'zero-vblank': (VBLANK, 0, VBLANK['toc']),
    'swap-flip': (FLIP, FLIP['toc'], FLIP['code']),
    'swap-vblank': (VBLANK, VBLANK['toc'], VBLANK['code']),
}


def build(tmp):
    (tmp / 'ppu_recomp.h').write_text(stub_header())
    cc = os.environ.get('CC', 'cc')
    cxx = os.environ.get('CXX', 'c++')
    objects = []
    for src, name in (('libs/video/tests/test_gcm_handler_contract.c', 'fixture.o'),
                      ('libs/video/gcm_default_state.c', 'gcm_default_state.o')):
        obj = tmp / name
        subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections', '-c',
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'libs/video'),
                        str(ROOT / src), '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / 'test'
    flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl', '-lm']
    subprocess.run([cxx, '-std=c++17', '-O1', '-w', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(tmp), '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime/platform'),
                    str(ROOT / 'runtime/ppu/ppu_loader.cpp'),
                    str(ROOT / 'libs/video/tests/test_gcm_handler_contract_stubs.cpp'),
                    *objects, *loader_support_sources(), *lifter_abi_sources(),
                    *loader_support_objects(tmp), *flags, '-o', str(binary)], check=True)
    return binary


def run(binary, mode):
    # Configuration and diagnostics knobs are not the test's: a developer's shell must not leak in.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PS3_', 'PS3RECOMP_', 'PPU_', 'SPU_', 'GCM_', 'YDKJ_'))}
    return subprocess.run([str(binary), mode], env=env, capture_output=True, text=True, timeout=60)


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX threads and mmap')
        return 0
    with tempfile.TemporaryDirectory(prefix='gcm-handler-contract-') as tmp:
        binary = build(Path(tmp))
        failed = 0

        p = run(binary, 'intact')
        print(p.stdout, end='')
        if p.returncode:
            sys.stderr.write(p.stderr[-2000:])
            print('FAIL intact')
            failed += 1
        else:
            print('PASS intact')

        for mode, (h, code, toc) in STOPS.items():
            p = run(binary, mode)
            try:
                assert 'pumping' in p.stdout, f'the fixture never reached the pump\n{p.stdout}'
                # never a call: a flip mode ran the handler once at request time, before the corruption;
                # nothing runs from the pump on. Never a skip that lets the pump return.
                assert 'HANDLER' not in p.stdout.split('pumping', 1)[1], f'a handler ran\n{p.stdout}'
                assert 'RETURNED' not in p.stdout, f'the pump returned\n{p.stdout}'
                f = g.expect_stop(p, 'unresolved-guest-callback')
                want = dict(origin='opd', opd='0x%08X' % h['opd'], code='0x%08X' % code, toc='0x%08X' % toc,
                            pc='0x0')
                assert {k: f[k] for k in want} == want, (f, want)
                assert f['descriptor'].startswith('%08X %08X' % (code, toc)), f
                print('PASS', mode)
            except AssertionError as e:
                failed += 1
                print('FAIL', mode, '--', e, file=sys.stderr)
        return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
