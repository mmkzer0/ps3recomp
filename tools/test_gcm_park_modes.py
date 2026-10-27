#!/usr/bin/env python3
"""The park heuristic, qualified in both modes on one synthetic parked FIFO
(libs/video/tests/test_gcm_park_modes.c, which compiles cellGcmSys.c in).

  reference      (gcm.park_resync off, the default) GET never moves to put, however long the park stands past
                 gcm.park_grace_ms; with a small gcm.wait_timeout_ms the run ends in the named stop
                 gcm-wait-timeout what=jump-park, and no intervention is logged;
  compatibility  (gcm.park_resync on) GET moves to put once the grace has passed, the command between the
                 park and put is bypassed, and every intervention is logged with the mode and the effective
                 grace."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM park fixture uses POSIX linking')
    raise SystemExit(0)

TIMEOUT_MS = 600     # gcm.wait_timeout_ms: short, so that the Reference stop arrives
GRACE_MS = 20        # gcm.park_grace_ms: a small fraction of the timeout

with tempfile.TemporaryDirectory(prefix='gcm-park-modes-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_park_modes.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(root / 'libs/video/gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)

    def run(mode):
        r = subprocess.run([str(binary), mode, str(TIMEOUT_MS), str(GRACE_MS)], timeout=60,
                           capture_output=True, text=True)
        print(r.stdout.strip())
        return r

    ref = run('reference')
    if ref.returncode != 70:
        print(ref.stderr[-2000:])
        raise SystemExit(f'test_gcm_park_modes reference: exit {ref.returncode}, want 70 (the named stop)')
    m = re.search(r'\[STOP\] gcm-wait-timeout func=cellGcm_rsx_process_fifo what=jump-park .* waited_ms=(\d+)',
                  ref.stdout)
    if not m:
        raise SystemExit('test_gcm_park_modes reference: the stop is not gcm-wait-timeout what=jump-park')
    if int(m.group(1)) < TIMEOUT_MS:
        raise SystemExit(f'test_gcm_park_modes reference: stopped after {m.group(1)} ms, before the {TIMEOUT_MS} ms timeout')
    if '[FAIL]' in ref.stdout:
        raise SystemExit('test_gcm_park_modes reference: a check failed before the stop')
    if 'jump-park intervention' in ref.stderr:
        raise SystemExit('test_gcm_park_modes reference: Reference mode logged an intervention')

    compat = run('compat')
    if compat.returncode != 0:
        print(compat.stderr[-2000:])
        raise SystemExit(f'test_gcm_park_modes compat: exit {compat.returncode}')
    lines = [l for l in compat.stderr.splitlines() if 'jump-park intervention' in l]
    if len(lines) != 2:
        raise SystemExit(f'test_gcm_park_modes compat: {len(lines)} intervention lines, want 2 (one per park)')
    for l in lines:
        if f'mode=compatibility grace={GRACE_MS} ms' not in l:
            raise SystemExit(f'test_gcm_park_modes compat: the intervention line lacks mode and grace: {l}')
    print('[gcm_park_modes] PASS')
