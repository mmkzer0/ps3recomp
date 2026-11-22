#!/usr/bin/env python3
"""MFC transfers to the SPU thread local-store window (0xF0000000 + spu_num * 0x100000) move data between local
stores, and a reader copying out of a store never meets a context that a workload removal frees under it.

Builds runtime/spu/tests/test_spu_thread_ls_dma.c against the real SPU runtime (the units spurs_scheduler links),
the run log, configuration, stops, host clock, label window and reservation model (tools/runtime_link.py). With
SANITIZERS=thread (the tsan ctest preset) every object is instrumented: the reader/RemoveWorkload case is then a
report if the copy ever ran outside the registry lock.
"""
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
units = ['spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy', 'spurs_job',
         'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
cflags = ['-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
          '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform')]
if os.environ.get('SANITIZERS'):
    cflags += ['-fsanitize=' + os.environ['SANITIZERS'], '-fno-sanitize-recover=all', '-g']
with tempfile.TemporaryDirectory(prefix='spu-thread-ls-dma-') as tmp:
    binary = Path(tmp) / 'test'
    runtime_link.build([spu / 'tests/test_spu_thread_ls_dma.c', *[spu / f'{u}.c' for u in units]], binary, tmp,
                       cflags=cflags,
                       ldflags=['-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'],
                       runtime=('diag', 'platform', 'label_window', 'reservation'))
    p = subprocess.run([str(binary)], timeout=120, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.returncode:
        sys.stderr.write(p.stderr[-3000:])
        raise SystemExit('test_spu_thread_ls_dma failed')
