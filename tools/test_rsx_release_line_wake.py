#!/usr/bin/env python3
"""RSX main-memory semaphore releases and PS3RECOMP_RSX_RELEASE_LINE_WAKE
(libs/video/tests/test_rsx_release_line_wake.c, built against rsx_commands.c):
one run with the variable unset and one with it set."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runtime_link  # noqa: E402

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: builds with a POSIX cc')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-release-line-wake-') as tmp:
    binary = Path(tmp) / 'test'
    # rsx_commands.c's migrated lines call ps3_log_* and read the typed configuration: the real log,
    # configuration and stops (the fixture defines none of them).
    runtime_link.build([root / 'libs/video/tests/test_rsx_release_line_wake.c',
                        root / 'libs/video/rsx_commands.c'], binary, tmp,
                       cflags=['-std=gnu17', '-O1', '-UNDEBUG',
                               '-I' + str(root / 'include'), '-I' + str(root / 'libs/video')])
    for mode in ('off', 'on'):
        result = subprocess.run([str(binary), mode], timeout=60, capture_output=True, text=True)
        print('\n'.join(l for l in result.stdout.splitlines() if l.startswith('[') and not l.startswith('[RSX')))
        if result.returncode:
            print(result.stdout[-2000:])
            raise SystemExit('test_rsx_release_line_wake failed: ' + mode)
