#!/usr/bin/env python3
"""Timed-wait overshoot, tree before #128 against tree after, rounds interleaved (old, new, old, new ...).

usage: run_timed_wait_bench.py OLD_TREE NEW_TREE [--rounds N] [--spin K] [--out FILE]

Each tree builds tests/bench/wait/bench_timed_wait.c (the NEW tree's copy, so both run the same program) against
its own sys_event/sys_semaphore/sys_cond/sys_mutex/win32_compat. Per (kind, T) cell it pools the overshoot samples
of every round (p50/p90/p99/max over all samples, ns) and averages CPU per call and voluntary context switches
per call. Not a test: it prints a table.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

KINDS = ['cond', 'queue', 'sem', 'shim', 'mutex']
TIMES_US = [1, 20, 100, 1000, 2000, 10000, 16000, 33000]
ITERS = {1: 2000, 20: 2000, 100: 1500, 1000: 400, 2000: 250, 10000: 80, 16000: 60, 33000: 40}


def build(tree, bench, out, tmp):
    t = Path(tree)
    srcs = [bench, t / 'tests/sync_stress/test_shims.c', t / 'runtime/syscalls/sys_event.c',
            t / 'runtime/syscalls/sys_semaphore.c', t / 'runtime/syscalls/sys_cond.c',
            t / 'runtime/syscalls/sys_mutex.c', t / 'runtime/platform/win32_compat.c']
    for extra in ('host_clock.c', 'host_wait.c'):
        if (t / 'runtime/platform' / extra).exists():
            srcs.append(t / 'runtime/platform' / extra)
    inc = ['-I' + str(t), '-I' + str(t / 'include'), '-I' + str(t / 'runtime'), '-I' + str(t / 'runtime/syscalls'),
           '-I' + str(t / 'runtime/platform')]
    objs = []
    for i, s in enumerate(srcs):
        o = Path(tmp) / f'{out.name}-{i}.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-w', '-D_GNU_SOURCE', '-ffunction-sections',
                        *inc, '-c', str(s), '-o', str(o)], check=True)
        objs.append(str(o))
    o = Path(tmp) / f'{out.name}-stop.o'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-w', '-I' + str(t / 'include'), '-c',
                    str(t / 'runtime/diag/stop.cpp'), '-o', str(o)], check=True)
    objs.append(str(o))
    subprocess.run([os.environ.get('CXX', 'c++'), *objs, '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                    '-pthread', '-lm', '-o', str(out)], check=True)


def pct(sorted_vals, p):
    return sorted_vals[min(len(sorted_vals) - 1, int(len(sorted_vals) * p / 100))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('old'); ap.add_argument('new')
    ap.add_argument('--rounds', type=int, default=4)
    ap.add_argument('--spin', type=int, default=0)
    ap.add_argument('--kinds', default=','.join(KINDS))
    ap.add_argument('--out')
    a = ap.parse_args()
    bench = Path(a.new) / 'tests/bench/wait/bench_timed_wait.c'
    tmp = tempfile.mkdtemp(prefix='wait-bench-')
    exes = {}
    for name, tree in (('old', a.old), ('new', a.new)):
        exes[name] = Path(tmp) / f'bench_{name}'
        build(tree, bench, exes[name], tmp)
    rows = {}
    for rnd in range(a.rounds):
        for kind in a.kinds.split(','):
            for us in TIMES_US:
                for name in (('old', 'new') if rnd % 2 == 0 else ('new', 'old')):
                    n = ITERS[us] // a.rounds + 1
                    p = subprocess.run([str(exes[name]), kind, str(us), str(n), str(a.spin)], capture_output=True, text=True,
                                       timeout=600, check=False)
                    f = p.stdout.split()
                    if p.returncode or len(f) < 9:
                        rows.setdefault((kind, us, name), []).append(None)
                        continue
                    # kind T n p50 p90 p99 max cpu vcsw: pool the per-round quantiles by weighting with n
                    rows.setdefault((kind, us, name), []).append([int(x) for x in f[3:7]] + [float(f[7]), float(f[8]), int(f[2])])
    lines = [f'# timed-wait overshoot (ns), spinners={a.spin}, rounds={a.rounds}, {sys.platform}',
             f'{"kind":6} {"T_us":>6} {"tree":4} {"p50":>9} {"p90":>9} {"p99":>9} {"max":>10} {"cpu_ns/call":>11} {"vcsw/call":>9}']
    for kind in a.kinds.split(','):
        for us in TIMES_US:
            for name in ('old', 'new'):
                r = [x for x in rows.get((kind, us, name), []) if x]
                if not r:
                    lines.append(f'{kind:6} {us:>6} {name:4}   n/a')
                    continue
                cols = [sorted(x[i] for x in r)[len(r) // 2] for i in range(3)]
                lines.append(f'{kind:6} {us:>6} {name:4} {cols[0]:>9} {cols[1]:>9} {cols[2]:>9} {max(x[3] for x in r):>10} '
                             f'{sum(x[4] for x in r) / len(r):>11.0f} {sum(x[5] for x in r) / len(r):>9.2f}')
    text = '\n'.join(lines)
    print(text)
    if a.out:
        Path(a.out).write_text(text + '\n')


if __name__ == '__main__':
    main()
