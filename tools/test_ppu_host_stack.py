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
                    *linker, '-o', str(binary)], check=True)
    quiet = subprocess.DEVNULL if os.environ.get('CI') else None
    for mode in ('on', 'off'):
        subprocess.run([str(binary), mode], check=True, timeout=120, stderr=quiet)
