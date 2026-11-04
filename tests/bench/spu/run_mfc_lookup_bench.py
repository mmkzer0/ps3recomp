#!/usr/bin/env python3
"""A/B of spu.mfc_lookup_cache on SPU-worker CPU time (mfc_lookup_bench.c).

Builds the benchmark against the real SPU channel layer, then alternates the
two modes (off, on, off, on, ...) so a drifting machine hits both alike, and
prints mean, standard deviation, min and max of the summed worker CPU time per
mode, with the relative change of the means. Each mode is a process: the option
is read once.

    tests/bench/spu/run_mfc_lookup_bench.py [--reps 5] [--threads 8]
        [--iterations 300000] [--workload dma|channel] [--work N] [--churn N]

Not part of any ctest tier. State the power mode next to any numbers taken on a
laptop (`pmset -g | grep powermode`, `pmset -g batt`).
"""
import argparse
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import spu_fixture_link  # noqa: E402

SPU = ROOT / 'runtime/spu'
COMMON = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
          'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c',
          'spu_interp.c']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reps', type=int, default=5)
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--iterations', type=int, default=300000)
    ap.add_argument('--workload', choices=('dma', 'channel'), default='dma')
    ap.add_argument('--work', type=int, default=0,
                    help='integer-mixing steps between sequences, standing in for SPU code')
    ap.add_argument('--churn', type=int, default=0,
                    help='release and re-create a spare context every N sequences (0: never)')
    args = ap.parse_args()
    if os.name == 'nt':
        print('SKIP: needs the POSIX toolchain')
        return 0

    with tempfile.TemporaryDirectory(prefix='mfc-bench-') as tmp:
        exe = Path(tmp) / 'mfc_lookup_bench'
        spu_fixture_link.build(
            [ROOT / 'tests/bench/spu/mfc_lookup_bench.c', *[SPU / c for c in COMMON],
             ROOT / 'runtime/platform/win32_compat.c'], exe,
            ['-std=gnu17', '-O2', '-DNDEBUG', '-w', '-I' + str(ROOT / 'include'), '-I' + str(SPU),
             '-I' + str(ROOT / 'runtime/platform')], tmp)
        cmd = [str(exe), '--threads', str(args.threads), '--iterations', str(args.iterations),
               '--workload', args.workload, '--work', str(args.work), '--churn', str(args.churn)]
        samples = {'0': [], '1': []}
        gens = {'0': 0, '1': 0}
        for rep in range(args.reps):
            for mode in ('0', '1'):
                env = {k: v for k, v in os.environ.items() if not k.startswith('PS3RECOMP_SPU_MFC_')}
                env['PS3RECOMP_SPU_MFC_LOOKUP_CACHE'] = mode
                out = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600,
                                     check=True).stdout
                m = re.search(r'RESULT .*cache=(\d) cpu_s=([\d.]+) wall_s=([\d.]+) '
                              r'ns_per_channel_op=([\d.]+) generations=(\d+)', out)
                assert m and m.group(1) == mode, out
                samples[mode].append((float(m.group(2)), float(m.group(3)), float(m.group(4))))
                gens[mode] = int(m.group(5))
                print(f'rep {rep + 1} cache={mode}: cpu {m.group(2)} s, wall {m.group(3)} s, '
                      f'{m.group(4)} ns/channel op, generations {m.group(5)}', flush=True)

    print(f'\nworkload={args.workload} threads={args.threads} iterations={args.iterations} '
          f'work={args.work} churn_every={args.churn} reps={args.reps}')
    means = {}
    for mode, label in (('0', 'off'), ('1', 'on ')):
        for idx, what in ((0, 'worker cpu_s'), (1, 'wall_s')):
            v = [s[idx] for s in samples[mode]]
            sd = statistics.stdev(v) if len(v) > 1 else 0.0
            print(f'cache {label} {what:<13} mean {statistics.mean(v):8.4f}  sd {sd:7.4f}  '
                  f'min {min(v):8.4f}  max {max(v):8.4f}')
            if idx == 0:
                means[mode] = (statistics.mean(v), sd, len(v))
    (m0, s0, n0), (m1, s1, n1) = means['0'], means['1']
    se = math.sqrt(s0 * s0 / n0 + s1 * s1 / n1)
    print(f'cpu change on vs off: {100.0 * (m1 - m0) / m0:+.1f} % '
          f'(difference of means {m1 - m0:+.4f} s, standard error {se:.4f} s)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
