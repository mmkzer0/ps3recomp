#!/usr/bin/env python3
"""cellGcmGetLastFlipTime: the guest system time of the last completed flip
(libs/video/tests/test_gcm_last_flip_time.c, which compiles cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM last flip time fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-last-flip-time-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
    # The fixture takes the stops and the configuration from gcm_fixture_stubs.h, so the runtime adds the run log
    # (ps3_log_*, its exit hook and the mirror stub), the label window and the default state.
    runtime_link.build([root / 'libs/video/tests/test_gcm_last_flip_time.c',
                        root / 'libs/video/gcm_default_state.c'], binary, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                               '-I' + str(root / 'include'), '-I' + str(root / 'libs/video')],
                       ldflags=linker, runtime=('log', 'label_window'))
    # Twice: as shipped, and with GCM_FLIP_NEEDS_FIFO (the fixture sets it itself, so a fresh process).
    for args in ([], ['needs-fifo']):
        result = subprocess.run([str(binary), *args], timeout=60, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_last_flip_time failed')
