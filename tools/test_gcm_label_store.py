#!/usr/bin/env python3
"""A guest store to a label is ordered after the RSX walker
(libs/video/tests/test_gcm_label_store.c, which compiles cellGcmSys.c in).

An equality ACQUIRE(N) on a label the title overwrites with N+1 as soon as it kicks the segment holding the
ACQUIRE needs the RSX to read the kick first. Four runs of one fixture with a walker that is late on purpose:
  control    a plain store: it overtakes the walker, which is held at ACQUIRE(N) (the failure, reproduced);
  ordered    the store goes through the hook the runtime registers: it waits for the walker, which passes
             ACQUIRE(N) against the old value, and the next frame's ACQUIRE(N+1) passes as well;
  no-walker  nobody walks: gcm-wait-timeout what=rsx-walker-pass;
  paused     nobody walks, and the host active clock (cellGcm_set_host_active_clock) stands still: the store
             outlives the timeout many times over without stopping, and stops once the clock runs again.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM label-store fixture uses POSIX threads and linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-label-store-') as tmp:
    binary = Path(tmp) / 'test'
    # cellGcmSys.c's migrated diagnostics call ps3_log_*; the fixture supplies ps3_config_get and the
    # stops (gcm_fixture_stubs.h), so the log links with host_exit.c and the mirror stub.
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_label_store.c'),
                    str(root / 'runtime/memory/label_window.c'),
                    str(root / 'runtime/diag/host_exit.c'),
                    str(root / 'runtime/diag/ps3_log.c'),
                    str(root / 'tests/ps3_log_mirror_stub.c'),
                    str(root / 'libs/video/gcm_default_state.c'), *linker,
                    '-o', str(binary)], check=True)
    for mode, timeout_ms, want in (('control', 2000, 0), ('ordered', 2000, 0), ('no-walker', 400, 70),
                                   ('paused', 100, 70)):
        result = subprocess.run([str(binary), mode, str(timeout_ms)], timeout=60, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode != want:
            print(result.stderr[-2000:])
            raise SystemExit(f'test_gcm_label_store {mode}: exit {result.returncode}, want {want}')
        if mode in ('no-walker', 'paused') and ('what=rsx-walker-pass' not in result.stdout or
                                                'func=cellGcm_label_store' not in result.stdout):
            raise SystemExit(f'test_gcm_label_store {mode}: the stop does not name the label-store wait')
        if mode == 'paused':
            # The pause held the clock through PAUSE_POLLS polls (far more wall time than the 100 ms timeout):
            # the fixture says so before the stop, and the stop only follows the pause's end.
            still, stop = result.stdout.find('still waiting after'), result.stdout.find('[STOP]')
            if still < 0 or stop < still:
                raise SystemExit('test_gcm_label_store paused: the wait stopped while the host pause was held')
    print('[gcm_label_store] PASS')
