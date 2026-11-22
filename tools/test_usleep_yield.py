#!/usr/bin/env python3
"""sys_timer_usleep on POSIX with and without PS3RECOMP_PPU_USLEEP_SPIN_US
(tests/test_usleep_yield.c). Each mode runs in its own process."""
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

if os.name == 'nt':
    print('SKIP: the opt-in yield is the POSIX branch (Windows already spins sub-millisecond sleeps)')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-usleep-yield-') as tmp:
    binary = Path(tmp) / 'test'
    linker = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    # sys_timer.c reads the typed configuration (ppu.usleep_spin_us): the real config.cpp answers ps3_config_get.
    runtime_link.build([root / 'tests/test_usleep_yield.c'], binary, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                               '-I' + str(root / 'include')],
                       ldflags=[linker])
    env = {k: v for k, v in os.environ.items() if k != 'PS3RECOMP_PPU_USLEEP_SPIN_US'}
    subprocess.run([str(binary), 'off'], env=env, check=True, timeout=60)
    subprocess.run([str(binary), 'spin'], env=dict(env, PS3RECOMP_PPU_USLEEP_SPIN_US='100'),
                   check=True, timeout=60)
