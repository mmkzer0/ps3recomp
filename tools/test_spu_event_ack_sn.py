#!/usr/bin/env python3
"""SPU_WrEventAck keeps a stall-and-notify pending while a DMA list is parked."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: fixture requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-event-ack-sn-') as tmp:
    binary = Path(tmp) / 'test'
    spu_fixture_link.build([spu/'tests/test_spu_event_ack_sn.c',
                            *[spu / name for name in sources],
                            root/'runtime/platform/win32_compat.c'], binary,
                           ['-std=gnu17', '-O1', '-UNDEBUG',
                            '-I'+str(root/'include'), '-I'+str(spu),
                            '-I'+str(root/'runtime/platform')], tmp)
    subprocess.run([str(binary)], check=True, timeout=5, cwd=tmp, env=env)
