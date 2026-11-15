#!/usr/bin/env python3
"""Build and run MFC local-lookup and context-lifetime regressions."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link

if os.name == 'nt':
    print('SKIP: MFC context fixtures require POSIX pthreads and mmap')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
common = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
          'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
          'spurs_policy_blob_weak.c', 'spu_interp.c']
fixtures = ['test_spu_mfc_local_lookup', 'test_spu_mfc_context_lifetime']
flags = ['-std=gnu17', '-O2', '-UNDEBUG']
if os.environ.get('SANITIZERS'):
    flags += ['-fsanitize=' + os.environ['SANITIZERS'],
              '-fno-sanitize-recover=all', '-g']

with tempfile.TemporaryDirectory(prefix='spu-mfc-context-') as tmp:
    for name in fixtures:
        exe = Path(tmp) / name
        spu_fixture_link.build(
            [spu / 'tests' / (name + '.c'), *[spu / c for c in common],
             root / 'runtime/platform/win32_compat.c',
             root / 'runtime/platform/host_clock.c',
             root / 'runtime/platform/host_wait.c'],
            exe,
            [*flags, '-I' + str(root / 'include'), '-I' + str(spu),
             '-I' + str(root / 'runtime/platform')],
            tmp)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('PS3RECOMP_SPU_MFC_')}
        env.pop('SPU_PCSAMPLE', None)
        env.pop('SPU_MFC_STATS', None)
        result = subprocess.run([str(exe)], env=env, capture_output=True,
                                text=True, timeout=120)
        print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'))
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr)
            raise SystemExit(result.returncode)
