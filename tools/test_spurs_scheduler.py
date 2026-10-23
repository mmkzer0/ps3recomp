#!/usr/bin/env python3
"""Real bounded SPURS scheduling with manufactured policy modules only."""
import os, platform, subprocess, tempfile
from pathlib import Path
if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-scheduler-') as tmp:
    tmp=Path(tmp)
    out=tmp/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    cflags=['-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
            '-I'+str(r/'include'),'-I'+str(r/'runtime/spu'),'-I'+str(r/'runtime/platform')]
    cxxflags=['-std=c++20','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
              '-I'+str(r/'include')]
    # spu_dma.h calls ps3_cfg_* and ps3_stop directly (strong references), so
    # the fixture must link the typed configuration and the named-stop runtime.
    c_sources=[r/'tests/test_spurs_scheduler.c',
               *[r/'runtime/spu'/f'{u}.c' for u in units],
               r/'runtime/memory/reservation.c', r/'runtime/platform/win32_compat.c']
    objects=[]
    for index,src in enumerate(c_sources):
        obj=tmp/f'{index}.o'
        subprocess.run([os.environ.get('CC','cc'),*cflags,'-c',str(src),'-o',str(obj)],check=True)
        objects.append(str(obj))
    for src in (r/'runtime/config/config.cpp', r/'runtime/diag/stop.cpp'):
        obj=tmp/(src.stem+'.o')
        subprocess.run([os.environ.get('CXX','c++'),*cxxflags,'-c',str(src),'-o',str(obj)],check=True)
        objects.append(str(obj))
    subprocess.run([os.environ.get('CXX','c++'),*objects,
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-pthread','-lm','-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=15)
