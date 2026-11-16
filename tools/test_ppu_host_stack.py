#!/usr/bin/env python3
"""The host stack of a guest PPU thread through the real sys_ppu_thread.c (tests/test_ppu_host_stack.c).

Two runs of one fixture:
- `on` (PS3_HOST_STACK_HWM=1): the high-water mark a thread reports at its exit, and the exit summary reports for a
  thread still running, equals the depth its recursion reached to within one host page; a thread that recursed
  nowhere reports little; a host that refuses the configured stack ends the create in the stop host-thread-stack.
- `off` (variable unset): nothing is recorded or reported.

PS3RECOMP_SANITIZE_THREAD=1 builds the fixture with -fsanitize=thread.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix='ppu-host-stack-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG', *san,
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'), '-I' + str(root / 'runtime/syscalls'),
                    str(root / 'tests/test_ppu_host_stack.c'), str(root / 'runtime/diag/ps3_log.c'),
                    str(root / 'runtime/diag/host_exit.c'),
                    *linker, '-o', str(binary)], check=True)
    # Under CI the fixture's log (every thread's start and end line) stays off the success path, but a failing run
    # names its mode and prints what it said, which carries the failing CHECK's line.
    ci = bool(os.environ.get('CI'))
    for mode in ('on', 'off'):
        run = subprocess.run([str(binary), mode], timeout=120, stderr=subprocess.PIPE if ci else None, text=True)
        if run.returncode != 0:
            print(f'FAIL: fixture mode `{mode}` exited {run.returncode}', file=sys.stderr)
            if ci and run.stderr:
                print(run.stderr, file=sys.stderr, end='')
            # What a stack's residency can depend on, for a run that cannot be repeated.
            thp = Path('/sys/kernel/mm/transparent_hugepage/enabled')
            print(f'host: {os.uname().sysname} {os.uname().release} {os.uname().machine}; THP enabled: '
                  f'{thp.read_text().strip() if thp.exists() else "n/a"}', file=sys.stderr)
            raise SystemExit(1)
