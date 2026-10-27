"""Build an SPU fixture the way the merged runtime needs it.

spu_dma.h and the reservation model call ps3_cfg_*, ps3_stop and resv_* directly
(strong references), so a fixture that compiles the SPU channel layer must also
link runtime/memory/reservation.c, runtime/memory/label_window.c and the C++
typed-configuration and named-stop runtime (config.cpp, stop.cpp), and link with
the C++ driver. Same recipe as tools/test_spu_machine_status.py.
"""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Added to every fixture's own C sources.
RUNTIME_C = [ROOT / 'runtime/memory/reservation.c', ROOT / 'runtime/memory/label_window.c']
RUNTIME_CXX = [ROOT / 'runtime/config/config.cpp', ROOT / 'runtime/diag/stop.cpp']


def build(c_sources, out, cflags, tmp, ldflags=(), runtime_c=True, timeout=None):
    """Compile c_sources (+ the runtime C files) with $CC, the C++ runtime with
    $CXX, and link everything with $CXX into `out`. Returns CompletedProcess of
    the link step; raises CalledProcessError on any failure."""
    tmp = Path(tmp)
    objects = []
    sources = [Path(s) for s in c_sources] + (RUNTIME_C if runtime_c else [])
    stem = Path(out).name
    for index, src in enumerate(sources):
        obj = tmp / f'{stem}-{index}-{src.stem}.o'
        subprocess.run([os.environ.get('CC', 'cc'), *cflags, '-c', str(src), '-o', str(obj)],
                       check=True, timeout=timeout)
        objects.append(str(obj))
    cxxflags = ['-std=c++20', '-O1', '-UNDEBUG', '-I' + str(ROOT / 'include')]
    cxxflags += [f for f in cflags if f.startswith('-fsanitize') or f == '-g']
    for src in RUNTIME_CXX:
        obj = tmp / f'{stem}-{src.stem}.o'
        subprocess.run([os.environ.get('CXX', 'c++'), *cxxflags, '-c', str(src), '-o', str(obj)],
                       check=True, timeout=timeout)
        objects.append(str(obj))
    sanitize = [f for f in cflags if f.startswith('-fsanitize')]
    return subprocess.run([os.environ.get('CXX', 'c++'), *objects, *sanitize, *ldflags,
                           '-pthread', '-lm', '-o', str(out)], check=True, timeout=timeout)
