#!/usr/bin/env python3
"""SPU_DIRECT: lifted known-target branches tail-call when the drain has nothing to do."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
with tempfile.TemporaryDirectory(prefix='spu-direct-') as tmp:
    binary = Path(tmp) / 'test'
    spu_fixture_link.build([spu/'tests/test_spu_direct.c',
                            *[spu / name for name in sources],
                            root/'runtime/platform/win32_compat.c'], binary,
                           ['-std=gnu17', '-O1', '-UNDEBUG',
                            '-I'+str(root/'include'), '-I'+str(spu),
                            '-I'+str(root/'runtime/platform')], tmp)
    failed = []
    for mode in ['direct', 'off', 'irq', 'reset-hint', 'ls0', 'nested']:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('SPU_', 'SPURS_', 'PS3_SPU_', 'YDKJ_', 'LBP_', 'GH3_'))}
        if mode == 'off':
            env['PS3_SPU_DIRECT'] = '0'
        result = subprocess.run([str(binary), mode], capture_output=True,
                                text=True, timeout=60, cwd=tmp, env=env)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
            failed.append(mode)
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
