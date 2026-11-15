#!/usr/bin/env python3
"""Run SPU MFC ownership and reservation regressions with >8 live contexts."""
import os
import signal
import subprocess
import sys
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
# SANITIZERS=thread (set by the tsan ctest preset) instruments every object.
sanitize = []
if os.environ.get('SANITIZERS'):
    sanitize = ['-fsanitize=' + os.environ['SANITIZERS'], '-fno-sanitize-recover=all', '-g']
    cflags += sanitize
    cxxflags += sanitize
common = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
          'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
          'spurs_policy_blob_weak.c', 'spu_interp.c', '../memory/reservation.c']
# Each fixture but the standalone coherence one compiles spu_channels.c into
# itself (directly or through test_spu_mfc_contexts.c). MFC engine state is
# embedded in spu_context; the engine still reaches config and stop handlers.
failed = []
with tempfile.TemporaryDirectory(prefix='spu-context-registry-') as tmp:
    tmp = Path(tmp)
    runtime_objects = []
    for src in (root / 'runtime/config/config.cpp', root / 'runtime/diag/stop.cpp'):
        obj = tmp / (src.stem + '.o')
        subprocess.run([cxx, *cxxflags, '-c', str(src), '-o', str(obj)], check=True)
        runtime_objects.append(str(obj))
    # test_spu_lockline_peer runs once: every GETLLAR, a registered holder's
    # included, takes the lock-line lock. The lock-free variant upstream added
    # is gone with the second bitmap it relied on (#57).
    variants = ['test_spu_mfc_contexts', 'test_spu_mfc_has_engine', 'test_spu_coherency',
                'test_spu_lockline_peer', 'test_spu_registry_allocation',
                'test_spu_mfc_env_once']
    # The CTest entry selects the MFC owner, presence-reader and env-gate fixtures.
    unknown = [a for a in sys.argv[1:] if a not in variants]
    if unknown:
        raise SystemExit('unknown variant: ' + ', '.join(unknown))
    for name in [v for v in variants if not sys.argv[1:] or v in sys.argv[1:]]:
        tag = name
        binary = tmp / tag
        sources = [str(spu / 'tests' / (name + '.c'))]
        link_flags = []
        if name != 'test_spu_coherency':
            extra = [str(spu / c) for c in common if not
                     (name == 'test_spu_registry_allocation' and c == 'spu_coherency.c')]
            sources += extra + [str(root/'runtime/platform/win32_compat.c'),str(root/'runtime/platform/host_clock.c'),str(root/'runtime/platform/host_wait.c'),
                                str(root / 'runtime/memory/label_window.c')]
            objects = []
            for index, src in enumerate(sources):
                obj = tmp / f'{tag}-{index}.o'
                subprocess.run([cc, *cflags, '-c', src, '-o', str(obj)], check=True)
                objects.append(str(obj))
            subprocess.run([cxx, *objects, *runtime_objects, *sanitize, '-pthread', '-lm',
                            '-o', str(binary)], check=True)
        else:
            subprocess.run([cc, *cflags, *sources, *sanitize,
                            '-o', str(binary)], check=True)
        if name == 'test_spu_registry_allocation':
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('PS3RECOMP_SPU_MFC_')}
            env.pop('SPU_PCSAMPLE', None)
            env.pop('SPU_MFC_STATS', None)
            for mode in ('mfc', 'coh'):
                result = subprocess.run([str(binary), mode], timeout=10,
                                        capture_output=True, text=True, env=env)
                if mode == 'mfc':
                    assert result.returncode == 0, result
                    print(name + '/mfc: PASS (inline engine needs no allocation)')
                else:
                    assert result.returncode == -signal.SIGABRT, result
                    assert 'cannot register reservation holder' in result.stderr, result.stderr
                    print(name + '/coh: PASS (visible allocation failure)')
            continue
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('PS3RECOMP_SPU_MFC_')}
        env.pop('SPU_PCSAMPLE', None)
        env.pop('SPU_MFC_STATS', None)
        # The slowest fixture (test_spu_lockline_peer) takes 8-9 s on an M-series Mac and hit
        # 30 s on a hosted macOS runner (#121); 120 s leaves room for a slow runner.
        result = subprocess.run([str(binary)], timeout=120, capture_output=True, text=True,
                                env=env)
        print(tag + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'))
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
            failed.append(tag)
if failed:
    raise SystemExit('Failed: ' + ', '.join(failed))
