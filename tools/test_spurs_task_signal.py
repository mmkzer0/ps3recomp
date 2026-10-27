#!/usr/bin/env python3
"""Real WAIT_SIGNAL must observe guest writes promptly, without inventing bits."""
import os, platform, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import spu_fixture_link
if os.name == 'nt':
    print('SKIP: POSIX thread fixture'); raise SystemExit(0)
r = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-signal-') as tmp:
    out = Path(tmp)/'test'
    units = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency',
             'spu_workload', 'spurs_policy', 'spurs_job', 'spu_tsp_weak',
             'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
    spu_fixture_link.build([r/'tests/test_spurs_task_signal.c',
        *[r/'runtime/spu'/f'{u}.c' for u in units],
        r/'runtime/platform/win32_compat.c'], out,
        ['-std=gnu17', '-O1', '-UNDEBUG', '-Dpthread_cond_timedwait=signal_test_cond_wait',
         '-ffunction-sections', '-fdata-sections',
         '-I'+str(r/'include'), '-I'+str(r/'runtime/spu'), '-I'+str(r/'runtime/platform')],
        tmp, ldflags=['-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections'])
    subprocess.run([str(out)],check=True,timeout=15)
    print('SPURS guest task signal wake PASS')
