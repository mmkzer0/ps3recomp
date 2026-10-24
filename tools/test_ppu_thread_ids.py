#!/usr/bin/env python3
"""PPU thread ids through the real sys_ppu_thread.c: monotonic, never reused, stale ids are ESRCH.

PS3RECOMP_SANITIZE_THREAD=1 builds the fixture with -fsanitize=thread (the concurrent-create test is the one it is for).
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
with tempfile.TemporaryDirectory(prefix='ppu-thread-ids-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG', *san,
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'), '-I' + str(root / 'runtime/syscalls'),
                    str(root / 'tests/test_ppu_thread_ids.c'), *linker, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120)
