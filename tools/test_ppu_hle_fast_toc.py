#!/usr/bin/env python3
"""The HLE fast path writes the caller's TOC slot (real dispatcher, synthetic handlers)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    if os.name == 'nt':
        print('SKIP: standalone dispatcher fixture requires a POSIX toolchain')
        return 0
    with tempfile.TemporaryDirectory(prefix='ppu-hle-fast-toc-') as directory:
        tmp = Path(directory)
        (tmp / 'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        includes = ['-I' + str(p) for p in (ROOT / 'include', ROOT / 'runtime/ppu', tmp)]
        # ppu_hle.cpp reads the typed configuration (hle stub list), raises named stops and logs.
        runtime_link.build([ROOT / 'tests/test_ppu_hle_fast_toc.cpp', ROOT / 'runtime/ppu/ppu_hle.cpp'],
                           tmp / 'fixture', tmp, cxxflags=['-std=c++17', '-O1', '-UNDEBUG', *includes],
                           timeout=180)
        env = {k: v for k, v in os.environ.items()
               if k in ('PATH', 'HOME', 'TMPDIR', 'SYSTEMROOT')}
        subprocess.run([str(tmp / 'fixture')], env=env, check=True, timeout=10)
    print('PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
