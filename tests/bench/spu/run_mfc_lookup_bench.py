#!/usr/bin/env python3
"""Build and measure many short, real MFC GET/PUT jobs on host workers.

Build the current tree with --output, then compare two binaries in alternating
rounds. Both binaries use the same -O2 -DNDEBUG benchmark and workload.
"""
import argparse
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
COMMON = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
          'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c',
          'spurs_policy_blob_weak.c', 'spu_interp.c']
RESULT = re.compile(
    r'RESULT threads=(\d+) iterations=(\d+) jobs=(\d+) cpu_s=([\d.]+) '
    r'wall_s=([\d.]+) ns_per_job=([\d.]+) dma_ok=(\d+)')


def build(exe, tmp):
    exe = Path(exe)
    exe.parent.mkdir(parents=True, exist_ok=True)
    spu_fixture_link.build(
        [ROOT / 'tests/bench/spu/mfc_lookup_bench.c', *[SPU / c for c in COMMON],
         ROOT / 'runtime/platform/win32_compat.c', ROOT / 'runtime/platform/host_clock.c',
         ROOT / 'runtime/platform/host_wait.c'],
        exe,
        ['-std=gnu17', '-O2', '-DNDEBUG', '-w',
         '-I' + str(ROOT / 'include'), '-I' + str(SPU),
         '-I' + str(ROOT / 'runtime/platform')],
        tmp)
    return exe


def clean_env():
    env = {k: v for k, v in os.environ.items()
           if not k.startswith('PS3RECOMP_SPU_MFC_')}
    env.pop('SPU_PCSAMPLE', None)
    env.pop('SPU_MFC_STATS', None)
    return env


def sample(exe, threads, iterations):
    result = subprocess.run(
        [str(exe), '--threads', str(threads), '--iterations', str(iterations)],
        env=clean_env(), capture_output=True, text=True, timeout=600, check=True)
    match = RESULT.search(result.stdout)
    if not match or match.group(7) != '1':
        raise RuntimeError(f'invalid benchmark result from {exe}:\n{result.stdout}\n{result.stderr}')
    return tuple(float(match.group(i)) for i in (4, 5, 6))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--threads', type=int, default=13)
    ap.add_argument('--iterations', type=int, default=20000,
                    help='short jobs per host worker')
    ap.add_argument('--output', type=Path,
                    help='write the current-tree executable here')
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--compare', nargs=2, type=Path, metavar=('BEFORE', 'AFTER'),
                    help='run these executables in alternating rounds')
    ap.add_argument('--rounds', type=int, default=3)
    args = ap.parse_args()
    if os.name == 'nt':
        print('SKIP: needs the POSIX toolchain')
        return 0
    if args.threads < 1 or args.threads > 64 or args.iterations < 1:
        ap.error('threads must be 1..64 and iterations must be positive')
    if args.compare and (args.output or args.build_only):
        ap.error('--compare cannot be combined with --output or --build-only')
    if args.build_only and not args.output:
        ap.error('--build-only requires --output')
    if args.rounds < 1:
        ap.error('--rounds must be positive')

    if args.compare:
        before, after = args.compare
        samples = {'before': [], 'after': []}
        for rnd in range(args.rounds):
            for label, exe in (('before', before), ('after', after)):
                cpu, wall, ns_job = sample(exe, args.threads, args.iterations)
                samples[label].append((cpu, wall, ns_job))
                print(f'round {rnd + 1} {label}: cpu_s={cpu:.6f} wall_s={wall:.6f} '
                      f'ns_per_job={ns_job:.2f}', flush=True)
        for label in ('before', 'after'):
            print(f'{label} median: cpu_s={statistics.median(x[0] for x in samples[label]):.6f} '
                  f'wall_s={statistics.median(x[1] for x in samples[label]):.6f} '
                  f'ns_per_job={statistics.median(x[2] for x in samples[label]):.2f}')
        return 0

    with tempfile.TemporaryDirectory(prefix='mfc-job-bench-') as tmp:
        exe = args.output or (Path(tmp) / 'mfc_lookup_bench')
        build(exe, tmp)
        print(f'build flags: -O2 -DNDEBUG; threads={args.threads} '
              f'jobs_per_thread={args.iterations}')
        if args.build_only:
            print(f'built {exe}')
            return 0
        cpu, wall, ns_job = sample(exe, args.threads, args.iterations)
        print(f'cpu_s={cpu:.6f} wall_s={wall:.6f} ns_per_job={ns_job:.2f}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
