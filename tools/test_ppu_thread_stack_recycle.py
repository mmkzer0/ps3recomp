#!/usr/bin/env python3
"""PPU thread stack recycling through the real sys_ppu_thread.c (Issue #18 / AM-26).

Checks:
- 300 creates/joins with 1 MB stacks: no ENOMEM, next_addr high-water mark stays bounded.
- Reused stack retains its PROT_NONE guard page below the stack base.
- Detached and non-joinable threads return their stack upon exit.

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

with tempfile.TemporaryDirectory(prefix='ppu-thread-stack-recycle-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    san = ['-fsanitize=thread', '-g'] if os.environ.get('PS3RECOMP_SANITIZE_THREAD') else []
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-UNDEBUG', *san,
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime'), '-I' + str(root / 'runtime/syscalls'),
                    str(root / 'tests/test_ppu_thread_stack_recycle.c'), *linker, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=120, stderr=subprocess.DEVNULL if os.environ.get('CI') else None)
