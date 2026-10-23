#!/usr/bin/env python3
"""Build and run the synthetic SPU observer fixture (spu_observe.h)."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
# Real interpreter, channels, MFC engine backing, lock-line and drain code:
# the hook sites under test must be the production ones.
sources = ['spu_channels.c', 'spu_interp.c', 'spu_coherency.c', 'spu_lockstep.c',
           'spu_drain.c', 'spu_vm_pagemap.c']
with tempfile.TemporaryDirectory(prefix='spu-observe-') as tmp:
    binary = Path(tmp) / 'test_spu_observe'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-I' + str(root / 'include'), '-I' + str(spu),
                    '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_observe.c'),
                    *[str(spu / name) for name in sources],
                    str(root / 'runtime/platform/win32_compat.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=30, cwd=tmp, env=env)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-4000:])
        raise SystemExit(1)
