#!/usr/bin/env python3
"""PPU thread join / detach / exit lifecycle through the real sys_ppu_thread.c.

The lv2 join/detach state machine (error codes, one reaper per thread) and the epilogue race: a worker parked past
its last table-lock section while the test joins, detaches and reuses its slot. The races are ordered with the
epilogue hook, not with sleeps.

PS3RECOMP_SANITIZE_THREAD=1 builds the fixture with -fsanitize=thread.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: fixture currently uses POSIX linking')
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix='ppu-thread-join-detach-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG', *san,
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'), '-I' + str(root / 'runtime/syscalls'),
                    str(root / 'tests/test_ppu_thread_join_detach.c'),
                    str(root / 'runtime/syscalls/lv2_enosys_bitmap.c'),
                    *linker, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120, stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
