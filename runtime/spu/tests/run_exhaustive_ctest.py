#!/usr/bin/env python3
"""CTest driver for test_spu_exhaustive (label `long`): a strided parity sweep plus the harness self-test.

  run_exhaustive_ctest.py --bindir DIR [--stride N] [--heavy-stride N] [--work DIR]

DIR holds test_spu_exhaustive.{scalar,native,mutant}, the one source built three ways:
  scalar  -DSPU_SCALAR_HELPERS -DSPU_SCALAR_INT: no vector fast path anywhere, the reference;
  native  the host's vector helpers (NEON, or SSE4.1/AVX2 where the host runs them);
  mutant  native plus -DEXH_MUTANT, which plants three single-point bugs.

1. Parity: scalar and native hash every (op, domain) block that is a multiple of the stride. A block is
   2^20 calls, so a stride of 16 runs 1/16 of the blocks, a seeded uniform subset; stride 1 is the full
   sweep (tens of minutes per build). native also compares each fast path with its own _ref
   (--refcheck). Any differing block or fast!=_ref call fails.
2. Self-test: the sweep must be able to see a one-in-2^32 bug. The planted bugs (cgth: one halfword pair;
   frest: one input word; fm/fma: one boundary product) sit in ops that are swept at stride 1 here, native
   against mutant. Each planted op must differ, otherwise a clean parity result above says nothing.
3. Coverage: fewer than MIN_BLOCKS compared blocks fails, so a sweep that silently ran nothing cannot pass.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import compare_exhaustive as ce

PLANTED_OPS = "cgth,frest,fm,fma"
MIN_BLOCKS = 1000


def start(exe, out, *args):
    return subprocess.Popen([exe, "--out", str(out), *args], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)


def finish(proc, what):
    err = proc.communicate()[1]
    if proc.returncode:
        sys.exit(f"FAILED: {what} exited {proc.returncode}\n{err[-2000:]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bindir", required=True)
    ap.add_argument("--stride", type=int, default=16, help="run every Nth block of every op/domain")
    ap.add_argument("--heavy-stride", type=int, default=64, help="stride of the 44 non-nominal-scale convert ops")
    ap.add_argument("--work", default=None)
    a = ap.parse_args()
    bindir = Path(a.bindir)
    work = Path(a.work) if a.work else bindir / "exh_work"
    work.mkdir(parents=True, exist_ok=True)
    exe = {v: str(bindir / f"test_spu_exhaustive.{v}") for v in ("scalar", "native", "mutant")}
    for p in exe.values():
        if not os.path.exists(p):
            sys.exit(f"FAILED: {p} is not built")

    strides = ["--stride-all", str(a.stride), "--stride-heavy", str(a.heavy_stride)]
    procs = {
        "scalar sweep": start(exe["scalar"], work / "scalar.txt", *strides),
        "native sweep": start(exe["native"], work / "native.txt", "--refcheck", *strides),
        "self-test reference": start(exe["native"], work / "native_planted.txt", "--ops", PLANTED_OPS),
        "self-test mutant": start(exe["mutant"], work / "mutant_planted.txt", "--ops", PLANTED_OPS),
    }
    for what, proc in procs.items():
        finish(proc, what)

    # Parity. compare_exhaustive prints the mismatches and re-runs the first differing block of each
    # op/domain in both builds to name the first differing call.
    sys.argv = ["compare_exhaustive.py", str(work / "scalar.txt"), str(work / "native.txt"),
                "--bindir", str(bindir), "--variant", "native"]
    if ce.main():
        sys.exit("FAILED: native differs from the scalar reference")
    ref_hashes = ce.load(str(work / "scalar.txt"))[0]
    compared = sum(1 for k in ce.load(str(work / "native.txt"))[0] if k in ref_hashes)
    if compared < MIN_BLOCKS:
        sys.exit(f"FAILED: only {compared} blocks compared (< {MIN_BLOCKS}); the sweep did not run")

    # Self-test.
    ref = ce.load(str(work / "native_planted.txt"))[0]
    mut = ce.load(str(work / "mutant_planted.txt"))[0]
    differing = {k[0] for k in mut if k in ref and ref[k] != mut[k]}
    missed = [op for op in PLANTED_OPS.split(",") if op not in differing]
    if missed:
        sys.exit(f"FAILED: the planted bug in {', '.join(missed)} was not found; "
                 "the sweep cannot see single-point bugs")
    print(f"ok: {compared} blocks match the scalar reference; planted bugs found in {', '.join(sorted(differing))}")


if __name__ == "__main__":
    main()
