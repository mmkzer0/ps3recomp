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
           'spu_drain.c', 'spu_vm_pagemap.c', '../memory/reservation.c']
cflags = ['-std=gnu17', '-O1', '-UNDEBUG',
          '-I' + str(root / 'include'), '-I' + str(spu),
          '-I' + str(root / 'runtime/platform')]
cxxflags = ['-std=c++20', '-O1', '-UNDEBUG', '-I' + str(root / 'include')]
with tempfile.TemporaryDirectory(prefix='spu-observe-') as tmp:
    tmp = Path(tmp)
    # spu_dma.h calls ps3_cfg_* and ps3_stop directly (strong references), so
    # the fixture must link the typed configuration and the named-stop runtime.
    c_sources = [spu / 'tests/test_spu_observe.c', *[spu / name for name in sources],
                 root / 'runtime/platform/win32_compat.c']
    objects = []
    for index, src in enumerate(c_sources):
        obj = tmp / f'{index}.o'
        subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-c', str(src),
                        '-o', str(obj)], check=True)
        objects.append(str(obj))
    for src in (root / 'runtime/config/config.cpp', root / 'runtime/diag/stop.cpp'):
        obj = tmp / (src.stem + '.o')
        subprocess.run([os.environ.get('CXX', 'c++'), *cxxflags, '-c', str(src),
                        '-o', str(obj)], check=True)
        objects.append(str(obj))
    binary = tmp / 'test_spu_observe'
    subprocess.run([os.environ.get('CXX', 'c++'), *objects, '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=30, cwd=tmp, env=env)
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-4000:])
        raise SystemExit(1)
