#!/usr/bin/env python3
"""The handler mask word at driver_info + 0x12C0 follows FW 4.93 libgcm_sys (issue #68).

libs/video/tests/test_gcm_handler_mask.c, which compiles cellGcmSys.c in, in two modes (the dossier
GCM-CALLBACK-OPD-CONTRACT.md §1, §2, §6 T6 is the evidence):

  mask  after init the word is the start value 4; each Set*Handler raises or clears only its own firmware
        bit (vblank 0x2, flip 0x10, second vblank 0x800, queue 0x40, user 0x80; never graphics 0x4) and
        leaves every other bit alone; a refused second init and a missing driver info write nothing
  race  two threads registering different handlers at once keep every bit, the foreign ones included.
        Built with -fsanitize=thread where the compiler has it, so a non-atomic read-modify-write is a
        report as well as a lost bit; a compiler without it runs the same fixture uninstrumented and says so.

With no argument both modes run.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def compile_fixture(tmp, extra):
    binary = tmp / ('test-' + ('tsan' if extra else 'plain'))
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-D_GNU_SOURCE', *extra,
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'libs/video'),
                    str(ROOT / 'libs/video/tests/test_gcm_handler_mask.c'),
                    str(ROOT / 'runtime/memory/label_window.c'),
                    str(ROOT / 'libs/video/gcm_default_state.c'), *linker, '-o', str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def tsan_binary(tmp):
    """The fixture built with -fsanitize=thread, or None when the compiler cannot link it."""
    try:
        return compile_fixture(tmp, ['-fsanitize=thread', '-g'])
    except subprocess.CalledProcessError as e:
        sys.stderr.write(e.stderr[-1500:])
        print('SKIP: no -fsanitize=thread here; the race mode runs without the sanitizer')
        return None


def run(binary, mode):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PS3_', 'PS3RECOMP_', 'GCM_', 'YDKJ_'))}
    env.setdefault('TSAN_OPTIONS', 'halt_on_error=1:suppressions=%s' % (ROOT / 'tests/tsan.supp'))
    r = subprocess.run([str(binary), mode], env=env, capture_output=True, text=True, timeout=300)
    for line in r.stdout.splitlines():
        if line.startswith('[FAIL]') or line.startswith('[gcm_handler_mask'):
            print(line)
    if r.returncode:
        sys.stderr.write(r.stderr[-3000:])
    return r.returncode


def main():
    if os.name == 'nt':
        print('SKIP: fixture uses POSIX linking')
        return 0
    modes = sys.argv[1:] or ['mask', 'race']
    failed = 0
    with tempfile.TemporaryDirectory(prefix='gcm-handler-mask-') as tmp:
        tmp = Path(tmp)
        for mode in modes:
            binary = (tsan_binary(tmp) if mode == 'race' else None) or compile_fixture(tmp, [])
            rc = run(binary, mode)
            print(('PASS' if rc == 0 else 'FAIL'), mode)
            failed += rc != 0
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
