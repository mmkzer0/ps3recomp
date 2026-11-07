#!/usr/bin/env python3
"""SPU channels and atomics: wake ordering, RdEventStat, SRR0, aligned LSA.

Builds runtime/spu/tests/test_spu_channel_semantics.c against the SPU channel
layer (the build line of test_spu_decrementer.c) and runs it.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
sources = ['runtime/spu/tests/test_spu_channel_semantics.c',
           'runtime/spu/spu_drain.c', 'runtime/spu/spu_lockstep.c',
           'runtime/spu/spu_coherency.c', 'runtime/spu/spu_workload.c',
           'runtime/spu/spurs_policy.c', 'runtime/spu/spurs_job.c',
           'runtime/spu/spu_tsp_weak.c', 'runtime/spu/spu_vm_pagemap.c',
           'runtime/spu/spurs_policy_blob_weak.c', 'runtime/spu/spu_interp.c',
           'runtime/platform/win32_compat.c', 'runtime/platform/host_clock.c',
           'runtime/platform/host_wait.c']
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-channel-semantics-') as tmp:
    binary = Path(tmp) / 'test'
    spu_fixture_link.build([root / s for s in sources], binary,
                           ['-std=gnu17', '-O1', '-w',
                            '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu'),
                            '-I' + str(root / 'runtime/platform')], tmp)
    subprocess.run([str(binary)], check=True, timeout=60, env=env, cwd=tmp)
