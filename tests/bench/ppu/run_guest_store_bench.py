#!/usr/bin/env python3
"""Guest store cost, a baseline checkout against the tree under test (not a test; prints a table).

  run_guest_store_bench.py --old /path/to/baseline/checkout [--stores N] [--rounds 3]

Builds tests/bench/ppu/bench_guest_store.cpp twice, each against its own tree's ppu_loader.cpp, reservation.c and
label_window.c (so the baseline's loader and the new one are each exactly their own), at -O3 -DNDEBUG like a
Release build, then alternates old and new for --rounds rounds and reports the median ns per store of each
width in each situation. A baseline is `git archive <rev> | tar -x -C DIR`. Run on an idle host (check uptime).
"""
import argparse
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

NEW_ROOT = Path(__file__).resolve().parents[3]


def build(root: Path, tmp: Path, name: str) -> Path:
    sys.path.insert(0, str(root / 'tools'))
    import importlib
    scaffold = importlib.import_module('check_ppu_scaffold')
    scaffold = importlib.reload(scaffold)
    sys.path.pop(0)
    work = tmp / name
    work.mkdir()
    (work / 'ppu_recomp.h').write_text(scaffold.stub_header())
    binary = work / 'bench'
    flags = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-ldl']
    objects = []
    cc = os.environ.get('CC', 'cc')
    # ps3_log.c's quick-exit registration comes from host_exit.c, which is C only (stdatomic): each tree's
    # loader may reference the log, so both builds take the logger and its exit table from the tree under
    # test (they are identical in every tree that has the log).
    for src_root, rel in ((root, 'runtime/memory/reservation.c'), (root, 'runtime/memory/label_window.c'),
                          (root, 'runtime/syscalls/sdk_version_weak.c'), (root, 'runtime/syscalls/lv2_enosys_bitmap.c'),
                          (NEW_ROOT, 'runtime/diag/host_exit.c')):
        obj = work / (os.path.basename(rel) + '.o')
        subprocess.run([cc, '-std=gnu17', '-O3', '-DNDEBUG', '-I', str(NEW_ROOT / 'include'),
                        '-c', str(src_root / rel), '-o', str(obj)], check=True)
        objects.append(str(obj))
    support = [str(root / p) for p in ('runtime/diag/stop.cpp', 'runtime/diag/sha256.cpp', 'runtime/config/config.cpp',
                                       'runtime/config/identity.cpp', 'runtime/diag/lifter_abi.c')]
    # The loader's migrated diagnostics call ps3_log_*, so the real logger is linked.
    support.append(str(NEW_ROOT / 'runtime/diag/ps3_log.c'))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O3', '-DNDEBUG', '-w',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(work), '-I' + str(root / 'include'), '-I' + str(root / 'runtime/platform'),
                    str(root / 'runtime/ppu/ppu_loader.cpp'),
                    str(NEW_ROOT / 'tests/bench/ppu/bench_guest_store.cpp'),
                    *support, *objects, *flags, '-o', str(binary)], check=True)
    return binary


def run(binary: Path, stores: int) -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'PS3RECOMP_', 'SPU_'))}
    out = subprocess.run([str(binary), str(stores)], env=env, capture_output=True, text=True, timeout=600, check=True)
    rows = {}
    for line in out.stdout.splitlines():
        f = line.split()
        rows[f[0]] = {f[i]: float(f[i + 1]) for i in range(1, len(f), 2)}
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old', required=True, help='a checkout of the baseline tree')
    ap.add_argument('--stores', type=int, default=50_000_000)
    ap.add_argument('--rounds', type=int, default=3)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='guest-store-bench-') as tmp:
        tmp = Path(tmp)
        bins = {'old': build(Path(a.old), tmp, 'old'), 'new': build(NEW_ROOT, tmp, 'new')}
        print('load:', subprocess.run(['uptime'], capture_output=True, text=True).stdout.strip())
        res = {'old': [], 'new': []}
        for _ in range(a.rounds):
            for which in ('old', 'new'):
                res[which].append(run(bins[which], a.stores))
        print(f'{"situation":8} {"width":8} {"old ns":>8} {"new ns":>8} {"speedup":>8}   (median of {a.rounds} alternating rounds)')
        for sit in ('idle', 'window', 'mark', 'marked'):
            for w in ('write8', 'write16', 'write32', 'write64'):
                med = {k: statistics.median(r[sit][w] for r in res[k]) for k in res}
                print(f'{sit:8} {w:8} {med["old"]:8.3f} {med["new"]:8.3f} {med["old"] / med["new"]:7.2f}x')


if __name__ == '__main__':
    main()
