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
    # Its sleeps read the host clock (host_monotonic_ns, platform group).
    runtime_link.build([root / 'tests/test_usleep_yield.c'], binary, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                               '-I' + str(root / 'include')],
                       ldflags=[linker], runtime=('diag', 'platform'))
    env = {k: v for k, v in os.environ.items() if k != 'PS3RECOMP_PPU_USLEEP_SPIN_US'}
    # The test counts nanosleep calls. On macOS ppu.precise_usleep (default on) takes sleeps of 20 us and
    # more off nanosleep onto a kevent timer, so it is turned off here; the spin branch comes before it.
    env['PS3RECOMP_PPU_PRECISE_USLEEP'] = '0'
    subprocess.run([str(binary), 'off'], env=env, check=True, timeout=60)
    subprocess.run([str(binary), 'spin'], env=dict(env, PS3RECOMP_PPU_USLEEP_SPIN_US='100'),
                   check=True, timeout=60)
