#!/usr/bin/env python3
"""Run SPU MFC ownership and reservation regressions with >8 live contexts."""
import os
import signal
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: SPU registry fixtures require POSIX pthreads')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
cc = os.environ.get('CC', 'cc')
cxx = os.environ.get('CXX', 'c++')
cflags = ['-std=gnu17', '-O1', '-UNDEBUG',
          '-I' + str(root / 'include'), '-I' + str(spu),
          '-I' + str(root / 'runtime/platform')]
cxxflags = ['-std=c++20', '-O1', '-UNDEBUG', '-I' + str(root / 'include')]
common = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
          'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
          'spurs_policy_blob_weak.c', 'spu_interp.c', '../memory/reservation.c']
# Every fixture but the standalone coherence one compiles spu_channels.c into
# itself (directly, or through test_spu_mfc_slots.c), which reaches the MFC
# engine in spu_dma.h; the engine calls ps3_cfg_* and ps3_stop directly, so
# those fixtures must link the typed configuration and named-stop runtime.
failed = []
with tempfile.TemporaryDirectory(prefix='spu-context-registry-') as tmp:
    tmp = Path(tmp)
    runtime_objects = []
    for src in (root / 'runtime/config/config.cpp', root / 'runtime/diag/stop.cpp'):
        obj = tmp / (src.stem + '.o')
        subprocess.run([cxx, *cxxflags, '-c', str(src), '-o', str(obj)], check=True)
        runtime_objects.append(str(obj))
    for name in ['test_spu_mfc_slots', 'test_spu_mfc_shared_lookup', 'test_spu_coherency', 'test_spu_lockline_peer', 'test_spu_registry_allocation']:
        binary = tmp / name
        sources = [str(spu / 'tests' / (name + '.c'))]
        link_flags = []
        if name != 'test_spu_coherency':
            extra = [str(spu / c) for c in common if not
                     (name == 'test_spu_registry_allocation' and c == 'spu_coherency.c')]
            sources += extra + [str(root / 'runtime/platform/win32_compat.c'),
                                str(root / 'runtime/memory/label_window.c')]
            objects = []
            for index, src in enumerate(sources):
                obj = tmp / f'{name}-{index}.o'
                subprocess.run([cc, *cflags, '-c', src, '-o', str(obj)], check=True)
                objects.append(str(obj))
            subprocess.run([cxx, *objects, *runtime_objects, '-pthread', '-lm',
                            '-o', str(binary)], check=True)
        else:
            subprocess.run([cc, *cflags, *sources,
                            '-o', str(binary)], check=True)
        if name == 'test_spu_registry_allocation':
            for mode, diagnostic in [('mfc', 'cannot allocate private MFC engine'),
                                     ('coh', 'cannot register reservation holder')]:
                result = subprocess.run([str(binary), mode], timeout=10,
                                        capture_output=True, text=True)
                assert result.returncode == -signal.SIGABRT, result
                assert diagnostic in result.stderr, result.stderr
                print(name + '/' + mode + ': PASS (visible allocation failure)')
            continue
        result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
        print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'))
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
        if result.returncode: failed.append(name)
if failed:
    raise SystemExit('Failed: ' + ', '.join(failed))
