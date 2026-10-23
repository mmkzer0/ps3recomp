#!/usr/bin/env python3
"""Exercise real SPU interpretation/drain boundaries without game binaries."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
cflags = ['-std=gnu17', '-O1', '-UNDEBUG',
          '-I' + str(root / 'include'), '-I' + str(spu),
          '-I' + str(root / 'runtime/platform')]
cxxflags = ['-std=c++20', '-O1', '-UNDEBUG', '-I' + str(root / 'include')]
with tempfile.TemporaryDirectory(prefix='spu-interp-handoff-') as tmp:
    tmp = Path(tmp)
    # spu_dma.h calls ps3_cfg_* and ps3_stop directly (strong references), so
    # the fixture must link the typed configuration and the named-stop runtime.
    c_sources = [spu / 'tests/test_spu_interpreter_handoff.c',
                 *[spu / name for name in sources],
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
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), *objects, '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    failed = []
    for mode in ['top', 'top-zero', 'nested-return', 'nested-return-unlifted', 'nested-calls', 'stop-zero',
                 'nested-stop', 'stop-code', 'halt', 'smc', 'pure-interpreter', 'stepcap', 'negative-stepcap']:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
        if mode in ('stepcap', 'negative-stepcap'):
            env.update(YDKJ_SPU_TRACE='1', SPU_STEPCAP='2')
        result = subprocess.run([str(binary), mode], capture_output=True,
                                text=True, timeout=5, cwd=tmp, env=env)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
            failed.append(mode)
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
